#pragma once
// xfsWinPad - OaiClient: OpenAI-compatible chat completions 客户端
//（批次 13：本地模型直连——Ollama /v1、DeepSeek、Qwen、LM Studio、vLLM 等）。
//
// 与 OcClient（opencode agent 全功能：工具/diff/工场/权限）互补：
//   * openai 模式 = 纯对话降级。没有工具调用，模型只能基于附带的上文回答，
//     不能改文件 —— 适合离线/本地小模型问答场景（需求文档 41 节）。
//   * 传输：WinHTTP，同步阻塞（调用方放工作线程），非流式 stream:false
//     ——本地端点无网络抖动，一批先不做 SSE。
//   * endpoint 形如 http://127.0.0.1:11434/v1（Ollama 默认）；也支持
//     https://api.deepseek.com/v1 等远程端点（带 Authorization: Bearer）。
//
// 线程约定：Chat 可长阻塞（模型推理慢），必须在工作线程调用；
// JSON 构造/解析助手是纯函数，供单元测试直接调用。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace xfs {
namespace ai {

struct OaiMsg {
    std::wstring role;     // user / assistant / system
    std::wstring content;
};

class OaiClient {
public:
    // GET {endpoint}/models → 模型 id 列表（Ollama /v1/models 兼容形态）。
    // 探活+填模型下拉两用；失败返回 false。
    static bool ListModels(const std::wstring& endpoint,
                           const std::wstring& apiKey,
                           std::vector<std::wstring>* out,
                           DWORD timeoutMs = 2500);

    // POST {endpoint}/chat/completions（非流式）。
    // messages 原样上送（调用方负责组装历史与上文）；成功时完整回复文本
    // 写入 *reply，出错时人类可读原因写入 *err（空成功）。
    static bool Chat(const std::wstring& endpoint, const std::wstring& apiKey,
                     const std::wstring& model,
                     const std::vector<OaiMsg>& messages,
                     std::wstring* reply, std::wstring* err,
                     DWORD timeoutMs = 300000);

    // POST {endpoint}/chat/completions（SSE 流式，stream:true）。
    // 每收到一段 delta content 就以工作线程上下文回调 onDelta(delta)
    //（Ollama/LM Studio/vLLM/DeepSeek 等 OpenAI 兼容端点均支持）。
    // 返回 false 时 *err 带原因（HTTP 错误/中途 error 事件/连接失败）；
    // *cancelFlag 置 true 时尽快返回 false（*err = "aborted"，UI 已知
    // 用户取消不需提示）。onFinish 在流正常结束（[DONE] 或连接关闭）后
    // 调一次（可空），附带完整拼接文本。
    typedef void (*StreamDeltaFn)(void* user, const std::wstring& delta);
    typedef void (*StreamDoneFn)(void* user, const std::wstring& full);
    static bool ChatStream(const std::wstring& endpoint,
                           const std::wstring& apiKey,
                           const std::wstring& model,
                           const std::vector<OaiMsg>& messages,
                           StreamDeltaFn onDelta, void* user,
                           StreamDoneFn onFinish, std::wstring* err,
                           DWORD timeoutMs = 300000,
                           std::atomic<bool>* cancelFlag = nullptr);

    // ---- 纯函数助手（单测覆盖）------------------------------------------

    // 构造 POST body：{"model":"..","messages":[..],"stream":false}
    //（UTF-8，content 经 JSON 转义）。stream=true 时末位换 "stream":true。
    static std::string BuildChatBody(const std::wstring& model,
                                     const std::vector<OaiMsg>& messages,
                                     bool stream = false);
    // {"choices":[{"message":{"content":".."}}]} → content；失败空串
    static std::wstring ParseChatReply(const std::wstring& json);
    // SSE 单条 data payload（chat.completion.chunk JSON）→ delta.content；
    // 无 content（role-only/null）返回空串。纯函数，跨 TCP 包的行重组
    // 由 ChatStream 的缓冲层负责。
    static std::wstring ParseStreamDelta(const std::wstring& json);
    // {"data":[{"id":"qwen2.5-coder:7b"},..]}（OpenAI /v1/models 形态）
    // → id 列表，保持文档顺序，跳过缺 id 的条目
    static bool ParseModelsList(const std::wstring& json,
                                std::vector<std::wstring>* out);
    // 错误响应 {"error":{"message":".."}} → message（无则空串）
    static std::wstring ParseErrorMessage(const std::wstring& json);
    // ---- 本地对话历史落盘（ai-history.json）的序列化对 ------------------
    // 历史文件由 AiPanel 读写（%APPDATA%\xfsWinPad），这里只做纯序列化。
    // 形态：{"version":1,"history":[{"role":"user","content":".."},..]}
    static std::string HistoryToJson(const std::vector<OaiMsg>& history);
    // 解析失败/结构不符返回 false（*out 置空）；空 history 数组 = true+空。
    static bool ParseHistory(const std::string& utf8,
                             std::vector<OaiMsg>* out);
};

} // namespace ai
} // namespace xfs
