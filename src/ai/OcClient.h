#pragma once
// xfsWinPad - OcClient: opencode HTTP client (AI 面板的网络层)。
//
// 与 `opencode serve --port 4096 --hostname 127.0.0.1` 通信：
//   GET  /global/health            探活 → { healthy, version }
//   POST /session                  建会话 → { id: "ses_..." }
//   POST /session/:id/prompt_async 异步发消息（立即返回，响应走轮询）
//   GET  /session/:id/message      消息列表 [{ info:{role,finish}, parts:[{type,text}] }]
//   POST /session/:id/abort        中断生成
//
// 请求/响应均为 UTF-8 JSON。线程约定：所有 HTTP 调用应在工作线程执行
//（ProbeHealth 除外，超时很短可安全用于 UI 线程）；JSON 解析助手是纯函数，
// 供面板在 UI 线程解析响应体，也供单元测试直接调用。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace xfs {
namespace ai {

// ---- 数据模型（解析 /session/:id/message 用） -------------------------------

struct OcPart {
    std::wstring type;      // text / reasoning / tool / step-start ...
    std::wstring text;
    // tool part 扩展（type == "tool" 时）：
    std::wstring tool;      // 工具名（read/bash/edit/grep ...）
    std::wstring callId;    // 调用 ID（同一次调用的状态流转去重键）
    std::wstring toolState; // pending / running / completed / error
    std::wstring filePath;  // state.input.filePath（write/edit/patch 的目标）
    std::wstring diff;      // state.metadata.diff（edit 完成后的 unified diff）
};

struct OcMessage {
    std::wstring role;    // user | assistant
    std::wstring finish;  // assistant 流式完成后非空（"stop"/"error"…）
    // info.error 归一化文本（ProviderAuthError/APIError... → "name: message"）。
    // 网关 401/凭据错误在这——serve 吞掉 part 不发文本，面板必须看这里
    //（批次 18 实锤：坏 key 时唯一的错误信号）。
    std::wstring errText;
    std::vector<OcPart> parts;
};

// /session 列表条目（会话历史切换的数据源；tokens/cost 为会话累计，
// 服务端在 /session 与 /session/:id 都带——批次 27 token/cost 显示用）
struct OcSessionEntry {
    std::wstring id;      // ses_...
    std::wstring title;
    std::wstring directory;   // 项目目录（过滤非当前工作区的会话用）
    long long updated = 0;    // time.updated（epoch ms，排序用）
    double cost = 0;          // 累计成本（美元）
    long long tokIn = 0;      // tokens.input
    long long tokOut = 0;     // tokens.output
    long long tokReason = 0;  // tokens.reasoning
    long long tokCacheRead = 0;  // tokens.cache.read
};

// ---- 纯 JSON 助手（单测覆盖；输入为 UTF-8 已转宽的 JSON 文本） --------------

// {"id":"ses_..."} → id；失败返回空串
std::wstring ParseSessionId(const std::wstring& json);
// {"version":"1.18.27"} → version
std::wstring ParseVersion(const std::wstring& json);
// [{"info:{role,finish},parts:[{type,text}]}...] → 结构化消息
bool ParseMessagesJson(const std::wstring& json, std::vector<OcMessage>* out);
// 解析 /config/providers 响应里的 default 模型映射
// {"providers":[...],"default":{"opencode":"big-pickle",...}} → (provider,model) 对
// 保持 JSON 文档顺序（首个 = 服务端排序首位，作为回退选择）
bool ParseProviderDefaults(const std::wstring& json,
                           std::vector<std::pair<std::wstring, std::wstring>>* out);

// 解析 /session/status 响应：{"ses_x":{"type":"busy"},...} → 该会话是否 busy。
// agent 循环的每个 step 都是一条 assistant 消息且 finish 非空，光看消息列表
// 会把中间步误判成生成结束（2026-09-05 真机踩坑）；必须以本端点为准。
bool ParseSessionBusy(const std::wstring& json, const std::wstring& sessionId);

// 解析 /permission 响应：[{id, sessionID, permission, patterns, ...}, ...]
// → 本会话的 pending 权限请求（permission 类型 + id）。
// 会话过滤在解析层做（sessionID != sessionId 的条目跳过）。
bool ParsePendingPermissions(
    const std::wstring& json, const std::wstring& sessionId,
    std::vector<std::pair<std::wstring, std::wstring>>* out);

// [{"id":"ses_..","title":"..","directory":"D:\\..","time":{"updated":ms},
//   "cost":0.8,"tokens":{"input":n,"output":n,"reasoning":n,
//   "cache":{"read":n}}},..]
// → 会话列表（保持文档顺序；updated 缺失按 0）。id 缺失的条目跳过。
bool ParseSessionList(const std::wstring& json, std::vector<OcSessionEntry>* out);

// /session/:id 单会话详情（字段同列表条目）→ 同一结构。id 缺失 false。
bool ParseSessionDetail(const std::wstring& json, OcSessionEntry* out);

// token/cost 显示格式化（批次 27，纯函数可单测）：
// FormatTokens 一位小数缩写、整数不带 .0（0→"0"、999→"999"、1000→"1k"、
// 1500→"1.5k"、2500000→"2.5M"、123456789→"123M"）。
std::wstring FormatTokens(long long n);
// FormatCost：<=0 → 空（调用方整段省略）；<1 美分 → "<$0.01"；其余两位小数
//（0.799456→"$0.80"、12.5→"$12.50"）。
std::wstring FormatCost(double cost);

// /event SSE data 帧（UTF-8 已转宽）→ (type, sessionID)。
// sessionID 取 properties.sessionID（首次出现，info/part 内层同名键不影响）；
// 无 sessionID 的事件（server.*）返回空串。type 缺失返回 false。
bool ParseEventFrame(const std::wstring& json, std::wstring* type,
                     std::wstring* sessionId);

// /config/providers 的 providers 数组条目 → 模型目录（模型选择下拉数据源）
struct OcModelEntry {
    std::wstring provider;      // providerID（prompt body 用）
    std::wstring providerName;  // 显示名（空回退 provider）
    std::wstring modelId;       // modelID（prompt body 用）
    std::wstring modelName;     // 显示名（空回退 modelId）
};
// {"providers":[{"id":"..","name":"..","models":{"id":{"name":".."}|{},...}},...]}
// 保持 JSON 文档顺序；跳过缺 id/modelId 的条目
bool ParseProviderList(const std::wstring& json, std::vector<OcModelEntry>* out);
// 构造 POST /session/:id/prompt_async 请求体（UTF-8 JSON）
// providerID/modelID 非空时带上 model 字段（未配置全局默认模型的服务端
// 对无 model 的请求会静默挂起 —— 实测行为，见 e2e 探测记录）
std::string BuildPromptBody(const std::wstring& text,
                            const std::wstring& providerID = std::wstring(),
                            const std::wstring& modelID = std::wstring());

// ---- HTTP（每请求独立 WinHTTP 句柄；可在多线程各自调用） ---------------------

class OcClient {
public:
    // 短超时探活（resolve 500ms / connect 1s / receive 2s）。
    // 成功时把 server 版本写入 *version（可空指针）。
    static bool ProbeHealth(std::wstring* version = nullptr);

    // 在 PATH 中查找 opencode（opencode.cmd / opencode.exe / opencode）。
    // 未找到返回空串。
    static std::wstring FindOpencodeExe();

    // serve pidfile：%LOCALAPPDATA%\xfsWinPad\serve.pid，内容
    // "pid|exe路径"（spawn 时写）。复用前校验（升级换 exe → 杀旧拉新）。
    static std::wstring ServePidPath();
    static bool ReadPidFile(DWORD* pid, std::wstring* exePath);
    static void WritePidFile(DWORD pid, const std::wstring& exePath);
    static bool IsProcessAlive(DWORD pid, const wchar_t* expectName);

    // 探活失败时启动 serve：WMI Win32_Process.Create 完全脱离父进程
    //（服务宿主 WmiPrvSE 托管，不随编辑器退出而死）。
    // workDir = serve 的工作目录（opencode 的项目 worktree）。
    // 返回 true = 已发起启动（是否就绪仍需轮询 ProbeHealth）。
    static bool SpawnServeDetached(const std::wstring& workDir);

    // EnsureServer = 探活 →(失败) 找 opencode → 启动 → 轮询就绪（最多 12s）。
    // notInstalled 出参指示"找不到 opencode 可执行"。
    // 复用卫生：复用前校验 pidfile（我们 spawn 的 serve 的 pid+exe 路径），
    // exe 路径与当前解析结果不符（升级/换安装位）→ 杀旧拉新。
    static bool EnsureServer(const std::wstring& workDir, bool* notInstalled = nullptr);

    // 建会话；成功返回会话 id（ses_...），失败返回空串。
    static std::wstring CreateSession(const std::wstring& title);

    // 拉取服务端默认模型映射（providerID → modelID，按文档序）。
    // 用于 prompt 时带上 model（见 BuildPromptBody 说明）。
    static bool FetchProviderDefaults(
        std::vector<std::pair<std::wstring, std::wstring>>* out);

    // 拉取完整模型目录（providers × models，按文档序）——模型选择下拉数据源。
    static bool FetchModelList(std::vector<OcModelEntry>* out);

    // 异步发消息。providerID/modelID 来自 FetchProviderDefaults；
    // 二者为空时不带 model 字段（走服务端默认）。
    static bool PromptAsync(const std::wstring& sessionId, const std::wstring& text,
                            const std::wstring& providerID = std::wstring(),
                            const std::wstring& modelID = std::wstring());
    // 同上 + 失败原因回报（服务端拒绝 vs 不可达），UI 直接展示。
    static bool PromptAsync(const std::wstring& sessionId, const std::wstring& text,
                            const std::wstring& providerID,
                            const std::wstring& modelID, std::wstring* err);

    // 拉取会话消息（流式期间反复调用观察增量）。
    static bool ListMessages(const std::wstring& sessionId, std::vector<OcMessage>* out);

    // 会话是否仍在生成（GET /session/status → type=="busy"）。请求失败返回 true
    //（宁可多等，不能提前触发重载判定）。
    static bool SessionBusy(const std::wstring& sessionId);

    // 本会话 pending 的权限请求列表（GET /permission → [(permId, type), ...]）
    static bool ListPendingPermissions(
        const std::wstring& sessionId,
        std::vector<std::pair<std::wstring, std::wstring>>* out);

    // 应答权限请求（POST /session/:id/permissions/:permissionID）。
    // response="always"+remember=true：同目录后续操作服务端永久记住，不再询问
    //（已实测：跨 session 同样免询问）。
    static bool AnswerPermission(const std::wstring& sessionId,
                                 const std::wstring& permissionId);

    // 会话列表（GET /session）——历史切换数据源。
    static bool ListSessions(std::vector<OcSessionEntry>* out);
    // GET /session/:id single-session detail - cumulative cost/tokens (batch 27)
    static bool GetSession(const std::wstring& sessionId, OcSessionEntry* out);

    // 中断会话生成。
    static void Abort(const std::wstring& sessionId);

    // ---- SSE 事件流（批次 26，替代 250ms 盲轮询的低延迟触发器）-------------
    // GET /event 长连接（text/event-stream），每收到一个事件回调一次：
    //   onEvent(eventTypeUtf16, sessionID, user)
    // 事件形态（真机探针 2026-09-09）：
    //   data: {"id":"evt_..","type":"message.part.updated",
    //          "properties":{"sessionID":"ses_..","part":{...}}}
    // eventType ∈ server.connected/heartbeat、session.created/updated/
    // status/idle/error/diff、message.updated、message.part.updated …
    // 只做"有活动→通知 UI 立即拉消息"的触发器；解析仍走 ListMessages
    //（part.updated 的 part 是全量替换语义，面板渲染层无需感知）。
    // 连接断开（serve 重启/网络抖动）返回 false 由调用方重连。
    // 阻塞式——必须在专用工作线程调用；*cancelFlag 置 true 尽快返回。
    typedef void (*OcEventFn)(void* user, const std::wstring& eventType,
                              const std::wstring& sessionID);
    static bool EventSubscribe(OcEventFn onEvent, void* user,
                               std::atomic<bool>* cancelFlag);

    // 测试注入口：非 0 覆盖 serve 端口（mock ocserver e2e 用）。
    static void SetPortOverride(unsigned port);

    // auth.json 污染预检：检出 "opencode" 条目 + OpenRouter 风格 key →
    // WARN 进日志并返回 true（zen 免费模型将 401；批次 18 实锤）。
    static bool DetectPoisonedZenAuth();

private:
    // 通用请求：status>=200&&<300 返回 true，body 为响应体（UTF-8）。
    static bool Request(const wchar_t* verb, const std::wstring& path,
                        const std::string& bodyUtf8, std::string* body,
                        DWORD timeoutMs);
};

} // namespace ai
} // namespace xfs
