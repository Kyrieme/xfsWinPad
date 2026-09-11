#include "OaiClient.h"
#include "../core/Log.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

namespace xfs {
namespace ai {

namespace {

// ---- 极简 JSON 扫描器（风格同 OcClient.cpp；UTF-16 域内操作）----------------

struct JCursor {
    const wchar_t* p;
    const wchar_t* end;
};

void JSkipWs(JCursor& c) {
    while (c.p < c.end && (*c.p == L' ' || *c.p == L'\t' ||
                           *c.p == L'\r' || *c.p == L'\n'))
        ++c.p;
}

bool JReadString(JCursor& c, std::wstring* out) {
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'"') return false;
    ++c.p;
    std::wstring s;
    while (c.p < c.end) {
        wchar_t ch = *c.p++;
        if (ch == L'"') { *out = std::move(s); return true; }
        if (ch == L'\\' && c.p < c.end) {
            wchar_t e = *c.p++;
            switch (e) {
                case L'"': s += L'"'; break;
                case L'\\': s += L'\\'; break;
                case L'/': s += L'/'; break;
                case L'n': s += L'\n'; break;
                case L't': s += L'\t'; break;
                case L'r': s += L'\r'; break;
                case L'u': {
                    auto hex4 = [&c]() {
                        unsigned v = 0;
                        for (int i = 0; i < 4 && c.p < c.end; ++i) {
                            wchar_t h = *c.p++;
                            v <<= 4;
                            if (h >= L'0' && h <= L'9') v |= (h - L'0');
                            else if (h >= L'a' && h <= L'f') v |= (h - L'a' + 10);
                            else if (h >= L'A' && h <= L'F') v |= (h - L'A' + 10);
                            else { return 0xFFFDu; }
                        }
                        return v;
                    };
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        c.p + 1 < c.end && c.p[0] == L'\\' && c.p[1] == L'u') {
                        c.p += 2;
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp >= 0x10000) {
                        cp -= 0x10000;
                        s += (wchar_t)(0xD800 + (cp >> 10));
                        s += (wchar_t)(0xDC00 + (cp & 0x3FF));
                    } else {
                        s += (wchar_t)cp;
                    }
                    break;
                }
                default: s += e; break;
            }
        } else {
            s += ch;
        }
    }
    return false;
}

// 从当前位置向前找 "\"key\""（字符串级查找，跨层级——调用方控制语义）。
bool JSeekKey(JCursor& c, const wchar_t* key) {
    const std::wstring pat = std::wstring(L"\"") + key + L"\"";
    const wchar_t* hit = std::search(c.p, c.end, pat.begin(), pat.end());
    if (hit == c.end) return false;
    c.p = hit + pat.size();
    JSkipWs(c);
    return c.p < c.end && *c.p == L':' && ++c.p;
}

void JsonEscape(std::wstring* out, const std::wstring& s) {
    out->reserve(out->size() + s.size() + 8);
    for (wchar_t ch : s) {
        switch (ch) {
            case L'"':  *out += L"\\\""; break;
            case L'\\': *out += L"\\\\"; break;
            case L'\n': *out += L"\\n";  break;
            case L'\r': *out += L"\\r";  break;
            case L'\t': *out += L"\\t";  break;
            default:    *out += ch; break;
        }
    }
}

// ---- endpoint 解析 ----------------------------------------------------------

struct OaiEndpoint {
    std::wstring host;
    INTERNET_PORT port = 80;
    std::wstring basePath;   // 含前导 /，无尾 /（如 /v1）
    bool secure = false;
};

// http(s)://host[:port]/base —— 无 scheme 视为 http；无端口按默认 80/443；
// 无路径按 /v1（本地端点最常见形态）。host 需非空。
bool ParseEndpoint(const std::wstring& url, OaiEndpoint* out) {
    std::wstring s = url;
    // 去空白
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    if (s.empty()) return false;
    out->secure = false;
    if (s.compare(0, 7, L"http://") == 0) { s = s.substr(7); }
    else if (s.compare(0, 8, L"https://") == 0) { s = s.substr(8); out->secure = true; out->port = 443; }
    std::wstring path;
    size_t slash = s.find(L'/');
    if (slash != std::wstring::npos) {
        path = s.substr(slash);
        s = s.substr(0, slash);
    }
    if (s.empty()) return false;
    size_t colon = s.find(L':');
    if (colon != std::wstring::npos) {
        out->host = s.substr(0, colon);
        std::wstring ps = s.substr(colon + 1);
        int p = _wtoi(ps.c_str());
        if (p <= 0 || p > 65535) return false;
        out->port = (INTERNET_PORT)p;
    } else {
        out->host = s;
    }
    if (out->host.empty()) return false;
    out->basePath = path.empty() ? L"/v1" : path;
    while (out->basePath.size() > 1 && out->basePath.back() == L'/')
        out->basePath.pop_back();
    if (out->basePath.front() != L'/') out->basePath.insert(out->basePath.begin(), L'/');
    return true;
}

// 通用请求：status 2xx → true + body；否则 false（错误 body 照样带出供解析）。
bool HttpRequest(const wchar_t* verb, const OaiEndpoint& ep,
                 const std::wstring& path, const std::wstring& apiKey,
                 const std::string& bodyUtf8, std::string* bodyOut,
                 std::wstring* httpErr, DWORD timeoutMs) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"xfsWinPad-OAI/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    HINTERNET con = WinHttpConnect(ses, ep.host.c_str(), ep.port, 0);
    std::wstring full = ep.basePath + path;
    HINTERNET req = con ? WinHttpOpenRequest(con, verb, full.c_str(), nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             ep.secure ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    do {
        if (!req) break;
        DWORD to = timeoutMs;
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(req, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(req, WINHTTP_OPTION_SEND_TIMEOUT, &to, sizeof(to));
        std::wstring hdrs = L"Content-Type: application/json\r\n";
        if (!apiKey.empty()) hdrs += L"Authorization: Bearer " + apiKey + L"\r\n";
        if (!WinHttpSendRequest(req, hdrs.c_str(), (DWORD)-1,
                                bodyUtf8.empty() ? WINHTTP_NO_REQUEST_DATA
                                                 : (LPVOID)bodyUtf8.data(),
                                (DWORD)bodyUtf8.size(),
                                (DWORD)bodyUtf8.size(), 0))
            break;
        if (!WinHttpReceiveResponse(req, nullptr)) break;
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sz, WINHTTP_NO_HEADER_INDEX);
        std::string resp;
        char buf[8192];
        DWORD read = 0;
        while (WinHttpReadData(req, buf, sizeof(buf), &read) && read > 0)
            resp.append(buf, read);
        if (bodyOut) *bodyOut = std::move(resp);
        if (status >= 200 && status < 300) {
            ok = true;
        } else {
            if (httpErr)
                *httpErr = L"HTTP " + std::to_wstring(status);
            Logger::Warn("OaiClient: HTTP " + std::to_string(status) + " " +
                         WideToUtf8(full));
        }
    } while (false);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

} // namespace

// ------------------------------------------------------------- 纯函数助手

std::string OaiClient::BuildChatBody(const std::wstring& model,
                                     const std::vector<OaiMsg>& messages,
                                     bool stream) {
    std::wstring b = L"{\"model\":\"";
    JsonEscape(&b, model);
    b += L"\",\"messages\":[";
    bool first = true;
    for (const auto& m : messages) {
        if (!first) b += L",";
        first = false;
        b += L"{\"role\":\"";
        JsonEscape(&b, m.role);
        b += L"\",\"content\":\"";
        JsonEscape(&b, m.content);
        b += L"\"}";
    }
    b += L"],\"stream\":";
    b += stream ? L"true" : L"false";
    b += L"}";
    return WideToUtf8(b);
}

std::wstring OaiClient::ParseChatReply(const std::wstring& json) {
    JCursor c{json.c_str(), json.c_str() + json.size()};
    if (!JSeekKey(c, L"choices")) return std::wstring();
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'[') return std::wstring();
    ++c.p;   // 进首个元素
    if (!JSeekKey(c, L"message")) return std::wstring();
    if (!JSeekKey(c, L"content")) return std::wstring();
    std::wstring out;
    if (!JReadString(c, &out)) return std::wstring();
    return out;
}

std::wstring OaiClient::ParseStreamDelta(const std::wstring& json) {
    // chat.completion.chunk：
    // {"choices":[{"delta":{"content":"片段"},"finish_reason":null},...]}
    // role-only 帧 delta={role:..} 无 content；末帧 finish_reason 有值但
    // content 缺失——都返回空串。
    JCursor c{json.c_str(), json.c_str() + json.size()};
    if (!JSeekKey(c, L"choices")) return std::wstring();
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'[') return std::wstring();
    ++c.p;
    if (!JSeekKey(c, L"delta")) return std::wstring();
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return std::wstring();
    ++c.p;
    // 在 delta 对象内找 "content"（仅本层——用深度计数）
    int depth = 1;
    while (c.p < c.end && depth > 0) {
        JSkipWs(c);
        if (c.p >= c.end) break;
        if (*c.p == L'}') { --depth; ++c.p; break; }
        if (*c.p == L'"') {
            std::wstring key;
            const wchar_t* keyStart = c.p;
            if (!JReadString(c, &key)) break;
            JSkipWs(c);
            if (c.p >= c.end || *c.p != L':') break;
            ++c.p;
            JSkipWs(c);
            if (depth == 1 && key == L"content" && c.p < c.end &&
                *c.p == L'"') {
                std::wstring v;
                if (!JReadString(c, &v)) break;
                return v;
            }
            // 值是字符串就吃掉；对象/数组/字面量按深度跳过
            if (c.p < c.end && *c.p == L'"') {
                std::wstring dummy;
                JReadString(c, &dummy);
            } else if (c.p < c.end && (*c.p == L'{' || *c.p == L'[')) {
                wchar_t open = *c.p++, close = (open == L'{') ? L'}' : L']';
                int d2 = 1;
                while (c.p < c.end && d2 > 0) {
                    wchar_t ch = *c.p++;
                    if (ch == L'"') { std::wstring s; JReadString(c, &s); }
                    else if (ch == open) ++d2;
                    else if (ch == close) --d2;
                }
            } else {
                // true/false/null/数字 → 吃到逗号或结束
                while (c.p < c.end && *c.p != L',' && *c.p != L'}') ++c.p;
            }
            continue;
        }
        if (*c.p == L'{') ++depth;
        else if (*c.p == L'}') --depth;
        ++c.p;
    }
    return std::wstring();
}

std::string OaiClient::HistoryToJson(const std::vector<OaiMsg>& history) {
    std::wstring b = L"{\"version\":1,\"history\":[";
    bool first = true;
    for (const auto& m : history) {
        if (!first) b += L",";
        first = false;
        b += L"{\"role\":\"";
        JsonEscape(&b, m.role);
        b += L"\",\"content\":\"";
        JsonEscape(&b, m.content);
        b += L"\"}";
    }
    b += L"]}";
    return WideToUtf8(b);
}

bool OaiClient::ParseHistory(const std::string& utf8,
                             std::vector<OaiMsg>* out) {
    if (out) out->clear();
    std::wstring j = Utf8ToWide(utf8);
    JCursor c{j.c_str(), j.c_str() + j.size()};
    if (!JSeekKey(c, L"history")) return false;
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'[') return false;
    ++c.p;
    std::vector<OaiMsg> hist;
    for (;;) {
        JSkipWs(c);
        if (c.p < c.end && *c.p == L']') break;
        if (c.p < c.end && *c.p == L',') { ++c.p; continue; }
        if (c.p >= c.end || *c.p != L'{') return false;
        ++c.p;
        OaiMsg m;
        if (!JSeekKey(c, L"role")) return false;
        if (!JReadString(c, &m.role)) return false;
        if (!JSeekKey(c, L"content")) return false;
        if (!JReadString(c, &m.content)) return false;
        hist.push_back(std::move(m));
        // 吃到对象收尾 '}'（字符串内忽略）
        int depth = 1;
        while (c.p < c.end && depth > 0) {
            if (*c.p == L'"') {
                std::wstring s;
                if (!JReadString(c, &s)) return false;
                continue;
            }
            wchar_t ch = *c.p++;
            if (ch == L'{') ++depth;
            else if (ch == L'}') --depth;
        }
        if (depth != 0) return false;
    }
    if (out) *out = std::move(hist);
    return true;
}

bool OaiClient::ParseModelsList(const std::wstring& json,
                                std::vector<std::wstring>* out) {
    JCursor c{json.c_str(), json.c_str() + json.size()};
    if (!JSeekKey(c, L"data")) return false;
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'[') return false;
    ++c.p;
    out->clear();
    while (c.p < c.end) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L']') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'{') break;   // 容错：非对象条目即止
        // 每个条目对象：提取 "id"（缺失跳过），随后跳到对象结束
        std::wstring id;
        if (JSeekKey(c, L"id") && JReadString(c, &id) && !id.empty())
            out->push_back(id);
        int depth = 1;   // 收尾：吃掉本条目剩余部分
        while (depth > 0 && c.p < c.end) {
            wchar_t ch = *c.p++;
            if (ch == L'"') {
                std::wstring dummy;
                JReadString(c, &dummy);
            } else if (ch == L'{') {
                ++depth;
            } else if (ch == L'}') {
                --depth;
            }
        }
    }
    return true;
}

std::wstring OaiClient::ParseErrorMessage(const std::wstring& json) {
    JCursor c{json.c_str(), json.c_str() + json.size()};
    if (!JSeekKey(c, L"error")) return std::wstring();
    if (!JSeekKey(c, L"message")) return std::wstring();
    std::wstring out;
    if (!JReadString(c, &out)) return std::wstring();
    return out;
}

// ---------------------------------------------------------------- HTTP

bool OaiClient::ListModels(const std::wstring& endpoint,
                           const std::wstring& apiKey,
                           std::vector<std::wstring>* out,
                           DWORD timeoutMs) {
    OaiEndpoint ep;
    if (!ParseEndpoint(endpoint, &ep)) return false;
    std::string body;
    if (!HttpRequest(L"GET", ep, L"/models", apiKey, "", &body, nullptr,
                     timeoutMs))
        return false;
    return ParseModelsList(Utf8ToWide(body), out);
}

bool OaiClient::Chat(const std::wstring& endpoint, const std::wstring& apiKey,
                     const std::wstring& model,
                     const std::vector<OaiMsg>& messages,
                     std::wstring* reply, std::wstring* err,
                     DWORD timeoutMs) {
    OaiEndpoint ep;
    if (!ParseEndpoint(endpoint, &ep)) {
        if (err) *err = L"bad endpoint";
        return false;
    }
    std::string resp;
    std::wstring httpErr;
    if (!HttpRequest(L"POST", ep, L"/chat/completions", apiKey,
                     BuildChatBody(model, messages), &resp, &httpErr,
                     timeoutMs)) {
        // 4xx/5xx：服务端错误 message 优先（比 HTTP 状态码可操作）
        std::wstring j = Utf8ToWide(resp);
        std::wstring em = ParseErrorMessage(j);
        if (err) {
            *err = httpErr;
            if (!em.empty()) *err += L": " + em;
        }
        return false;
    }
    std::wstring j = Utf8ToWide(resp);
    std::wstring text = ParseChatReply(j);
    if (text.empty()) {
        if (err) *err = L"empty reply";
        return false;
    }
    if (reply) *reply = std::move(text);
    return true;
}

bool OaiClient::ChatStream(const std::wstring& endpoint,
                           const std::wstring& apiKey,
                           const std::wstring& model,
                           const std::vector<OaiMsg>& messages,
                           StreamDeltaFn onDelta, void* user,
                           StreamDoneFn onFinish, std::wstring* err,
                           DWORD timeoutMs, std::atomic<bool>* cancelFlag) {
    OaiEndpoint ep;
    if (!ParseEndpoint(endpoint, &ep)) {
        if (err) *err = L"bad endpoint";
        return false;
    }
    bool ok = false;
    std::wstring full;
    std::wstring httpErr;
    HINTERNET ses = WinHttpOpen(L"xfsWinPad-OAI/1.0",
                                WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, ep.host.c_str(), ep.port, 0)
                        : nullptr;
    std::wstring pathQ = ep.basePath + L"/chat/completions";
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", pathQ.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             ep.secure ? WINHTTP_FLAG_SECURE
                                                       : 0)
                        : nullptr;
    std::string body = BuildChatBody(model, messages, /*stream=*/true);
    do {
        if (!req) { httpErr = L"connection failed"; break; }
        // 接收超时=相邻两个 SSE 块的最大间隔。本地推理 token 间隔通常
        // <1s，留 30s 余量；整段生成不再受 300s 总时长限制（流式特性）。
        DWORD to = 30000;
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(req, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs,
                         sizeof(timeoutMs));
        WinHttpSetOption(req, WINHTTP_OPTION_SEND_TIMEOUT, &timeoutMs,
                         sizeof(timeoutMs));
        std::wstring hdrs = L"Content-Type: application/json\r\n";
        if (!apiKey.empty())
            hdrs += L"Authorization: Bearer " + apiKey + L"\r\n";
        if (!WinHttpSendRequest(req, hdrs.c_str(), (DWORD)-1,
                                (LPVOID)body.data(), (DWORD)body.size(),
                                (DWORD)body.size(), 0)) {
            httpErr = L"send failed";
            break;
        }
        if (!WinHttpReceiveResponse(req, nullptr)) {
            httpErr = L"no response";
            break;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                     WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        if (status < 200 || status >= 300) {
            // 错误体一次性读完（非 SSE），带出服务端 message
            std::string resp;
            char buf[8192];
            DWORD rd = 0;
            while (WinHttpReadData(req, buf, sizeof(buf), &rd) && rd > 0)
                resp.append(buf, rd);
            std::wstring em =
                ParseErrorMessage(Utf8ToWide(resp));
            httpErr = L"HTTP " + std::to_wstring(status);
            if (!em.empty()) httpErr += L": " + em;
            Logger::Warn("OaiClient: stream HTTP " + std::to_string(status));
            break;
        }

        // ---- SSE 行缓冲：TCP 包可能切断任意位置（行中/行间/UTF-8 字符中）----
        // 协议：事件以空行分隔；每行 "data: <payload>"；"data: [DONE]" 结束。
        // content 都是 UTF-8 → 逐块 append 进 pending，按 '\n' 切完整行，
        // 残尾留在缓冲直到下块补齐。
        std::string pending;
        std::string lineBytes;   // 已累积的当前行（可能含多字节 UTF-8）
        bool done = false;
        ok = true;
        char buf[4096];
        DWORD rd = 0, avail = 0;
        // 同步 ReadData(4096) 会凑满请求数才返回 → 流式 delta 全憋到
        // 连接关闭才一次性交付（批次 26 发现）。先 QueryDataAvailable
        // 问"已有"字节数再按需读，delta 才真正逐块到达。
        while (!done && WinHttpQueryDataAvailable(req, &avail) &&
               avail > 0) {
            DWORD want =
                avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (!WinHttpReadData(req, buf, want, &rd) || rd == 0) break;
            if (cancelFlag && cancelFlag->load()) {
                httpErr = L"aborted";
                ok = false;
                done = true;
                break;
            }
            pending.append(buf, rd);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                lineBytes.append(pending, 0, nl);
                pending.erase(0, nl + 1);
                if (!lineBytes.empty() && lineBytes.back() == '\r')
                    lineBytes.pop_back();
                std::wstring line = Utf8ToWide(lineBytes);
                lineBytes.clear();
                if (line.compare(0, 5, L"data:") == 0) {
                    std::wstring payload = line.substr(6);
                    while (!payload.empty() &&
                           (payload.front() == L' ' || payload.front() == L'\t'))
                        payload.erase(payload.begin());
                    if (payload == L"[DONE]") { done = true; break; }
                    std::wstring delta = ParseStreamDelta(payload);
                    if (!delta.empty()) {
                        full += delta;
                        if (onDelta) onDelta(user, delta);
                    } else if (full.empty() &&
                               payload.find(L"\"error\"") !=
                                   std::wstring::npos) {
                        // chunk 内嵌 error 帧（部分端点风格）。仅首帧前检查，
                        // 防 delta 文本内容里出现 "error" 字样误伤。
                        std::wstring e2 = ParseErrorMessage(payload);
                        if (!e2.empty()) {
                            httpErr = e2;
                            ok = false;
                            done = true;
                            break;
                        }
                    }
                }
            }
        }
        if (ok && !done) {
            // 连接关闭未发 [DONE]：主流端点（Ollama）也这样正常收尾——
            // 修复：EOF 视为正常结束（full 非空即成功）
            if (full.empty()) {
                httpErr = L"stream ended without data";
                ok = false;
            }
            Logger::Info("OaiClient: stream ended without [DONE], " +
                         std::to_string(full.size()) + " chars");
        }
    } while (false);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (ok) {
        Logger::Info("OaiClient: stream ok, " +
                     std::to_string(full.size()) + " chars total");
        if (onFinish) onFinish(user, full);
    } else {
        if (err && err->empty()) *err = httpErr;
        Logger::Warn("OaiClient: stream failed: " + WideToUtf8(httpErr));
    }
    return ok;
}

} // namespace ai
} // namespace xfs
