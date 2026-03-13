/*
 * VoCoType Fcitx5 Addon - IPC Client
 *
 * IPC 客户端，负责与 Python Backend 通过 Unix Socket 通信
 */

#ifndef VOCOTYPE_IPC_CLIENT_H
#define VOCOTYPE_IPC_CLIENT_H

#include <string>
#include <vector>
#include <memory>

namespace vocotype {

/**
 * Rime UI 状态
 *
 * 表示 Rime 的预编辑、候选词等 UI 状态
 */
struct RimeUIState {
    bool handled = false;               // 按键是否被 Rime 处理
    bool ascii_mode = false;            // 当前是否英文模式
    std::string commit_text;            // 提交的文本（如果有）
    std::string preedit_text;           // 预编辑文本
    int cursor_pos = 0;                 // 光标位置

    // 候选词列表: (text, comment)
    std::vector<std::pair<std::string, std::string>> candidates;
    int highlighted_index = 0;          // 高亮的候选词索引
    int page_size = 5;                  // 每页候选词数
};

/**
 * 语音识别结果
 */
struct TranscribeResult {
    bool success = false;
    std::string text;
    std::string error;
};

/**
 * IPC 客户端
 *
 * 通过 Unix Socket 与 Python Backend 通信
 */
class IPCClient {
public:
    /**
     * 构造函数
     *
     * @param socket_path Unix Socket 路径
     */
    explicit IPCClient(const std::string& socket_path);

    /**
     * 析构函数
     */
    ~IPCClient();

    /**
     * 语音识别
     *
     * @param audio_path 音频文件路径
     * @return 识别结果
     */
    TranscribeResult transcribeAudio(const std::string& audio_path);

    /**
     * 处理 Rime 按键
     *
     * @param keyval X11 keysym 值
     * @param mask Rime modifier mask
     * @return Rime UI 状态
     */
    RimeUIState processKey(int keyval, int mask);

    /**
     * 重置 Rime 状态
     */
    void reset();

    /**
     * 健康检查
     *
     * @return 是否连接成功
     */
    bool ping();

private:
    /**
     * 发送请求并接收响应
     *
     * @param request JSON 请求字符串
     * @return JSON 响应字符串
     */
    std::string sendRequest(const std::string& request);

    std::string socket_path_;
};

} // namespace vocotype

#endif // VOCOTYPE_IPC_CLIENT_H
