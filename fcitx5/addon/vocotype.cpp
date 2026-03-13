/*
 * VoCoType Fcitx5 Addon Implementation
 */

#include "vocotype.h"
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/candidatelist.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>

namespace {

fcitx::KeyList defaultPTTKeys() {
    return {
        fcitx::Key(FcitxKey_F9),
        fcitx::Key(FcitxKey_Alt_R),
        fcitx::Key(FcitxKey_ISO_Level3_Shift),
    };
}

std::string stopRecorderProcess(pid_t pid, int stdin_fd, FILE* stdout_file) {
    if (stdin_fd >= 0) {
        close(stdin_fd);
    }

    std::string audio_path;
    if (stdout_file) {
        char buffer[1024];
        if (fgets(buffer, sizeof(buffer), stdout_file) != nullptr) {
            audio_path = buffer;
            while (!audio_path.empty() &&
                   (audio_path.back() == '\n' || audio_path.back() == '\r')) {
                audio_path.pop_back();
            }
        }
        fclose(stdout_file);
    }

    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }

    return audio_path;
}

} // namespace

namespace vocotype {

VoCoTypeAddon::VoCoTypeAddon(fcitx::Instance* instance)
    : instance_(instance),
      ptt_keys_(defaultPTTKeys()),
      ipc_client_(std::make_unique<IPCClient>("/tmp/vocotype-fcitx5.sock")) {
    main_thread_dispatcher_.attach(&instance_->eventLoop());

    // 获取安装路径
    const char* home = std::getenv("HOME");
    if (home) {
        python_venv_path_ = std::string(home) + "/.local/share/vocotype-fcitx5/.venv/bin/python";
        recorder_script_path_ = std::string(home) + "/.local/share/vocotype-fcitx5/backend/audio_recorder.py";
    } else {
        FCITX_ERROR() << "HOME environment variable not set";
    }

    FCITX_INFO() << "VoCoType Addon initialized";
    FCITX_INFO() << "PTT keys: "
                 << fcitx::Key::keyListToString(
                        ptt_keys_, fcitx::KeyStringFormat::Portable);

    // 测试 Backend 连接
    if (ipc_client_->ping()) {
        FCITX_INFO() << "Backend connection OK";
    } else {
        FCITX_WARN() << "Backend not responding, please ensure fcitx5_server.py is running";
    }
}

VoCoTypeAddon::~VoCoTypeAddon() {
    if (recorder_pid_ > 0 || recorder_stdout_ || recorder_stdin_fd_ >= 0) {
        std::string audio_path =
            stopRecorderProcess(recorder_pid_, recorder_stdin_fd_, recorder_stdout_);
        if (!audio_path.empty()) {
            std::remove(audio_path.c_str());
        }
        recorder_pid_ = -1;
        recorder_stdin_fd_ = -1;
        recorder_stdout_ = nullptr;
        is_recording_ = false;
    }
    main_thread_dispatcher_.detach();
    FCITX_INFO() << "VoCoType Addon destroyed";
}

std::vector<fcitx::InputMethodEntry> VoCoTypeAddon::listInputMethods() {
    std::vector<fcitx::InputMethodEntry> result;

    auto entry = fcitx::InputMethodEntry("vocotype", "VoCoType", "zh_CN", "vocotype");
    entry.setNativeName("语音输入");
    entry.setIcon("audio-input-microphone");
    entry.setLabel("V");

    result.push_back(std::move(entry));
    return result;
}

void VoCoTypeAddon::keyEvent(const fcitx::InputMethodEntry& entry,
                              fcitx::KeyEvent& keyEvent) {
    auto ic = keyEvent.inputContext();

    // 获取按键信息
    auto key = keyEvent.key();
    bool is_release = keyEvent.isRelease();

    FCITX_DEBUG() << "Key event: key=" << key.toString()
                  << ", release=" << is_release
                  << ", ptt_keys="
                  << fcitx::Key::keyListToString(
                         ptt_keys_, fcitx::KeyStringFormat::Portable);

    // 处理 PTT 键（按住说话，松开识别）
    if (isPTTKey(key, is_release)) {
        if (is_release) {
            // 松开 PTT 键：停止录音并转录
            if (is_recording_) {
                stopAndTranscribe(ic);
            }
        } else {
            // 按下 PTT 键：开始录音
            if (!is_recording_) {
                startRecording(ic);
            }
        }
        keyEvent.filterAndAccept();
        return;
    }

    int keyval = key.sym();

    // 跳过输入法切换热键
    if (isIMSwitchHotkey(key, is_release)) {
        return;
    }

    // 普通字符键只转发按下事件；修饰键保留按下/松开，
    // 避免普通键的 release 被 Rime 再次解释，导致拼音重复。
    if (is_release && !key.isModifier()) {
        return;
    }

    // 构建 Rime modifier mask
    int mask = 0;
    if (key.states() & fcitx::KeyState::Shift) {
        mask |= (1 << 0);  // kShiftMask
    }
    if (key.states() & fcitx::KeyState::CapsLock) {
        mask |= (1 << 1);  // kLockMask
    }
    if (key.states() & fcitx::KeyState::Ctrl) {
        mask |= (1 << 2);  // kControlMask
    }
    if (key.states() & fcitx::KeyState::Alt) {
        mask |= (1 << 3);  // kAltMask
    }
    if (is_release) {
        mask |= (1 << 30);  // kReleaseMask
    }

    // 调用 IPC
    try {
        const bool old_ascii_mode = ascii_mode_;
        RimeUIState state = ipc_client_->processKey(keyval, mask);
        ascii_mode_ = state.ascii_mode;

        // 如果有提交文本，先提交
        if (!state.commit_text.empty()) {
            commitText(ic, state.commit_text);
        }

        // 更新 UI
        updateUI(ic, state);

        // 如果被 Rime 处理，则拦截此按键
        if (state.handled) {
            if (ascii_mode_ != old_ascii_mode) {
                updateStatusIndicator(ic);
            }
            keyEvent.filterAndAccept();
            return;
        }

        if (ascii_mode_ != old_ascii_mode) {
            updateStatusIndicator(ic);
        }

    } catch (const std::exception& e) {
        FCITX_ERROR() << "Rime key processing failed: " << e.what();
    }
}

void VoCoTypeAddon::reset(const fcitx::InputMethodEntry& entry,
                           fcitx::InputContextEvent& event) {
    auto ic = event.inputContext();
    clearUI(ic);
    ipc_client_->reset();
}

void VoCoTypeAddon::activate(const fcitx::InputMethodEntry& entry,
                              fcitx::InputContextEvent& event) {
    FCITX_DEBUG() << "VoCoType activated";
    updateStatusIndicator(event.inputContext());
}

void VoCoTypeAddon::deactivate(const fcitx::InputMethodEntry& entry,
                                fcitx::InputContextEvent& event) {
    auto ic = event.inputContext();
    clearUI(ic);

    // 如果正在录音，停止录音但不转录
    if (is_recording_) {
        stopRecording(ic, false);
    }

    FCITX_DEBUG() << "VoCoType deactivated";
}

void VoCoTypeAddon::startRecording(fcitx::InputContext* ic) {
    if (is_recording_) {
        return;
    }

    if (python_venv_path_.empty() || recorder_script_path_.empty()) {
        showError(ic, "录音配置无效");
        return;
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        showError(ic, "启动录音失败");
        return;
    }
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        showError(ic, "启动录音失败");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        showError(ic, "启动录音失败");
        return;
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        execl(python_venv_path_.c_str(),
              python_venv_path_.c_str(),
              recorder_script_path_.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    FILE* stdout_file = fdopen(stdout_pipe[0], "r");
    if (!stdout_file) {
        close(stdout_pipe[0]);
        close(stdin_pipe[1]);
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        showError(ic, "启动录音失败");
        return;
    }

    recorder_pid_ = pid;
    recorder_stdin_fd_ = stdin_pipe[1];
    recorder_stdout_ = stdout_file;
    is_recording_ = true;

    // 显示录音状态
    auto& inputPanel = ic->inputPanel();
    fcitx::Text preedit;
    preedit.append("🎤 录音中...");
    inputPanel.setClientPreedit(preedit);
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);

    FCITX_INFO() << "Recording started";
}

void VoCoTypeAddon::stopAndTranscribe(fcitx::InputContext* ic) {
    stopRecording(ic, true);
}

void VoCoTypeAddon::stopRecording(fcitx::InputContext* ic, bool transcribe) {
    if (!is_recording_) {
        return;
    }

    is_recording_ = false;

    if (ic) {
        if (transcribe) {
            auto& inputPanel = ic->inputPanel();
            fcitx::Text preedit;
            preedit.append("⏳ 识别中...");
            inputPanel.setClientPreedit(preedit);
            ic->updatePreedit();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        } else {
            clearUI(ic);
        }
    }

    pid_t pid = recorder_pid_;
    int stdin_fd = recorder_stdin_fd_;
    FILE* stdout_file = recorder_stdout_;
    recorder_pid_ = -1;
    recorder_stdin_fd_ = -1;
    recorder_stdout_ = nullptr;

    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();

    std::thread([this, pid, stdin_fd, stdout_file, transcribe, ic_ref]() mutable {
        std::string audio_path = stopRecorderProcess(pid, stdin_fd, stdout_file);
        if (audio_path.empty()) {
            if (transcribe) {
                main_thread_dispatcher_.schedule([this, ic_ref]() {
                    auto* ic_ptr = ic_ref.get();
                    if (ic_ptr) {
                        showError(ic_ptr, "录音失败");
                    }
                });
            }
            return;
        }

        if (!transcribe) {
            std::remove(audio_path.c_str());
            return;
        }

        TranscribeResult result = ipc_client_->transcribeAudio(audio_path);
        std::remove(audio_path.c_str());

        main_thread_dispatcher_.schedule([this, ic_ref, result]() {
            auto* ic_ptr = ic_ref.get();
            if (!ic_ptr) {
                return;
            }
            if (result.success && !result.text.empty()) {
                commitText(ic_ptr, result.text);
            } else if (!result.success) {
                showError(ic_ptr,
                          result.error.empty() ? "转录失败" : result.error);
            } else {
                clearUI(ic_ptr);
            }
        });
    }).detach();

    FCITX_INFO() << "Recording stopped";
}

bool VoCoTypeAddon::isPTTKey(const fcitx::Key& key, bool is_release) const {
    const auto normalized_key = key.normalize();

    for (const auto& ptt_key : ptt_keys_) {
        const auto normalized_ptt_key = ptt_key.normalize();
        if (key.check(ptt_key) || normalized_key.check(normalized_ptt_key)) {
            return true;
        }
        if (is_release &&
            (key.isReleaseOfModifier(ptt_key) ||
             normalized_key.isReleaseOfModifier(normalized_ptt_key))) {
            return true;
        }
    }

    return false;
}

void VoCoTypeAddon::updateUI(fcitx::InputContext* ic, const RimeUIState& state) {
    auto& inputPanel = ic->inputPanel();

    // 在经典 UI 中，preedit 应该显示在输入法面板上，而不是依赖
    // client preedit（后者更偏向应用内预编辑）。
    if (!state.preedit_text.empty()) {
        fcitx::Text preedit;
        preedit.append(state.preedit_text, fcitx::TextFormatFlag::Underline);
        inputPanel.setPreedit(preedit);
        inputPanel.setClientPreedit(fcitx::Text());
        ic->updatePreedit();
    } else {
        inputPanel.setPreedit(fcitx::Text());
        inputPanel.setClientPreedit(fcitx::Text());
        ic->updatePreedit();
    }

    // 更新候选词
    if (!state.candidates.empty()) {
        auto candidateList = std::make_unique<fcitx::CommonCandidateList>();
        candidateList->setPageSize(state.page_size);
        candidateList->setCursorPositionAfterPaging(
            fcitx::CursorPositionAfterPaging::ResetToFirst);

        // 设置候选词选择键（数字 1-0）
        candidateList->setSelectionKey({
            fcitx::Key(FcitxKey_1), fcitx::Key(FcitxKey_2), fcitx::Key(FcitxKey_3),
            fcitx::Key(FcitxKey_4), fcitx::Key(FcitxKey_5), fcitx::Key(FcitxKey_6),
            fcitx::Key(FcitxKey_7), fcitx::Key(FcitxKey_8), fcitx::Key(FcitxKey_9),
            fcitx::Key(FcitxKey_0)
        });

        for (size_t i = 0; i < state.candidates.size(); ++i) {
            const auto& [text, comment] = state.candidates[i];
            fcitx::Text candidate_text;
            candidate_text.append(text);
            candidateList->append<fcitx::DisplayOnlyCandidateWord>(candidate_text);
        }

        int cursor_index = state.highlighted_index;
        if (cursor_index < 0 ||
            cursor_index >= static_cast<int>(state.candidates.size())) {
            cursor_index = 0;
        }
        candidateList->setGlobalCursorIndex(cursor_index);
        inputPanel.setCandidateList(std::move(candidateList));
    } else {
        inputPanel.setCandidateList(nullptr);
    }

    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VoCoTypeAddon::clearUI(fcitx::InputContext* ic) {
    auto& inputPanel = ic->inputPanel();
    inputPanel.reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VoCoTypeAddon::commitText(fcitx::InputContext* ic, const std::string& text) {
    clearUI(ic);
    ic->commitString(text);
    FCITX_INFO() << "Committed text: " << text;
}

void VoCoTypeAddon::showError(fcitx::InputContext* ic, const std::string& error) {
    auto& inputPanel = ic->inputPanel();
    fcitx::Text preedit;
    preedit.append("❌ " + error);
    inputPanel.setClientPreedit(preedit);
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);

    // 简化：不自动清除，等待用户下次按键
    // 2 秒自动清除在 Fcitx5 中需要更复杂的实现
}

void VoCoTypeAddon::updateStatusIndicator(fcitx::InputContext* ic) {
    if (!ic) {
        return;
    }
    ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
}

std::string VoCoTypeAddon::subModeIconImpl(const fcitx::InputMethodEntry& entry,
                                           fcitx::InputContext& inputContext) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(inputContext);
    return ascii_mode_ ? "input-keyboard" : "audio-input-microphone";
}

std::string VoCoTypeAddon::subModeLabelImpl(const fcitx::InputMethodEntry& entry,
                                            fcitx::InputContext& inputContext) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(inputContext);
    return ascii_mode_ ? "A" : "中";
}

bool VoCoTypeAddon::isIMSwitchHotkey(const fcitx::Key& key, bool is_release) const {
    const auto normalized_key = key.normalize();
    const auto& config = instance_->globalConfig();

    auto matches_key_list = [&](const fcitx::KeyList& keys) {
        for (const auto& configured_key : keys) {
            const auto normalized_configured_key = configured_key.normalize();
            if (key.check(configured_key) ||
                normalized_key.check(normalized_configured_key)) {
                return true;
            }
            if (is_release &&
                (key.isReleaseOfModifier(configured_key) ||
                 normalized_key.isReleaseOfModifier(normalized_configured_key))) {
                return true;
            }
        }
        return false;
    };

    return matches_key_list(config.triggerKeys()) ||
           matches_key_list(config.altTriggerKeys()) ||
           matches_key_list(config.enumerateForwardKeys()) ||
           matches_key_list(config.enumerateBackwardKeys()) ||
           matches_key_list(config.enumerateGroupForwardKeys()) ||
           matches_key_list(config.enumerateGroupBackwardKeys()) ||
           matches_key_list(config.activateKeys()) ||
           matches_key_list(config.deactivateKeys());
}

} // namespace vocotype

// Fcitx5 插件注册
class VoCoTypeAddonFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new vocotype::VoCoTypeAddon(manager->instance());
    }
};

FCITX_ADDON_FACTORY(VoCoTypeAddonFactory);
