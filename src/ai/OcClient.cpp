#include "OcClient.h"
#include "../core/Log.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <ole2.h>
#include <oleauto.h>
#include <comdef.h>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

namespace xfs {
namespace ai {

namespace {

constexpr wchar_t kHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kPort = 4096;

// 测试注入口：非 0 时覆盖默认端口（批次 26）。e2e 用 mock ocserver；
// 正式路径恒为 4096（EnsureServer 拉起的 serve 固定端口）。
std::atomic<unsigned> g_portOverride{0};

// 进程级注入：环境变量 XFSWINPAD_OC_PORT（仅测试机设置；e2e 把全部
// OcClient 流量引到 mock ocserver）。
struct PortEnvInit {
    PortEnvInit() {
        wchar_t buf[16] = {};
        if (::GetEnvironmentVariableW(L"XFSWINPAD_OC_PORT", buf, 16)) {
            unsigned v = (unsigned)_wtoi(buf);
            if (v) g_portOverride.store(v, std::memory_order_relaxed);
        }
    }
};
PortEnvInit g_portEnvInit;

// ---- 极简 JSON 扫描器（容错风格同 Settings.cpp；UTF-16 域内操作）------------

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
                case L'u': {   // \uXXXX（含代理对）
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

// 跳过任意 JSON 值（字符串/数字/字面量/嵌套对象数组）
void JSkipValue(JCursor& c) {
    JSkipWs(c);
    if (c.p >= c.end) return;
    wchar_t ch = *c.p;
    if (ch == L'"') { std::wstring s; JReadString(c, &s); return; }
    if (ch == L'{' || ch == L'[') {
        int depth = 0;
        bool inStr = false;
        while (c.p < c.end) {
            wchar_t x = *c.p++;
            if (inStr) {
                if (x == L'\\' && c.p < c.end) ++c.p;
                else if (x == L'"') inStr = false;
                continue;
            }
            if (x == L'"') inStr = true;
            else if (x == L'{' || x == L'[') ++depth;
            else if ((x == L'}' || x == L']') && --depth == 0) return;
        }
        return;
    }
    while (c.p < c.end && *c.p != L',' && *c.p != L'}' && *c.p != L']') ++c.p;
}

// 从当前位置向前找 "\"key\""（字符串级查找，跨层级——调用方控制语义）。
//（批次 26 EventSubscribe 用；与 OaiClient.cpp 同名助手同实现）
bool JSeekKey(JCursor& c, const wchar_t* key) {
    const std::wstring pat = std::wstring(L"\"") + key + L"\"";
    const wchar_t* hit = std::search(c.p, c.end, pat.begin(), pat.end());
    if (hit == c.end) return false;
    c.p = hit + pat.size();
    JSkipWs(c);
    return c.p < c.end && *c.p == L':' && ++c.p;
}

} // namespace

bool ParseEventFrame(const std::wstring& json, std::wstring* type,
                     std::wstring* sessionId) {
    if (type) type->clear();
    if (sessionId) sessionId->clear();
    JCursor c{json.c_str(), json.c_str() + json.size()};
    if (!JSeekKey(c, L"type")) return false;
    if (!JReadString(c, type)) return false;
    // sessionID：找首个 "sessionID" 键（schema 保证 properties.sessionID
    // 是文档中该键的首次出现；info/part 内层即使同名也在其后）。
    if (sessionId) {
        const std::wstring sidKey = L"\"sessionID\"";
        size_t sidAt = json.find(sidKey);
        if (sidAt != std::wstring::npos) {
            JCursor s{json.c_str() + sidAt + sidKey.size(),
                      json.c_str() + json.size()};
            JSkipWs(s);
            if (s.p < s.end && *s.p == L':' && ++s.p)
                JReadString(s, sessionId);
        }
    }
    return true;
}

// ---------------------------------------------------------------- JSON helpers

std::wstring ParseSessionId(const std::wstring& json) {
    // {"id":"ses_..."}：顶层扫一遍 key
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return L"";
    ++c.p;   // 进入顶层对象
    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'"') { JSkipValue(c); continue; }
        std::wstring key;
        if (!JReadString(c, &key)) break;
        JSkipWs(c);
        if (c.p >= c.end || *c.p != L':') break;
        ++c.p;
        JSkipWs(c);
        if (key == L"id" && c.p < c.end && *c.p == L'"') {
            std::wstring id;
            JReadString(c, &id);
            return id;
        }
        JSkipValue(c);
    }
    return L"";
}

std::wstring ParseVersion(const std::wstring& json) {
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return L"";
    ++c.p;
    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'"') { JSkipValue(c); continue; }
        std::wstring key;
        if (!JReadString(c, &key)) break;
        JSkipWs(c);
        if (c.p >= c.end || *c.p != L':') break;
        ++c.p;
        JSkipWs(c);
        if (key == L"version" && c.p < c.end && *c.p == L'"') {
            std::wstring v;
            JReadString(c, &v);
            return v;
        }
        JSkipValue(c);
    }
    return L"";
}

// [{info:{role,finish},parts:[{type,text}]}...] → 结构化消息
// 容错策略：字段缺失/顺序任意/未知字段跳过；role 为空且无 parts 的条目丢弃。
bool ParseMessagesJson(const std::wstring& json, std::vector<OcMessage>* out) {
    if (!out) return false;
    out->clear();
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'[') return false;
    ++c.p;

    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L']') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'{') { JSkipValue(c); continue; }
        ++c.p;   // 进入消息对象

        OcMessage msg;
        for (;;) {   // 消息级 key 循环（info / parts，任意顺序、任意多余字段）
            JSkipWs(c);
            if (c.p >= c.end || *c.p == L'}') break;
            if (*c.p == L',') { ++c.p; continue; }
            std::wstring key;
            if (!JReadString(c, &key)) break;
            JSkipWs(c);
            if (c.p >= c.end || *c.p != L':') break;
            ++c.p;
            JSkipWs(c);

            if (key == L"info" && c.p < c.end && *c.p == L'{') {
                ++c.p;
                for (;;) {
                    JSkipWs(c);
                    if (c.p >= c.end || *c.p == L'}') break;
                    if (*c.p == L',') { ++c.p; continue; }
                    std::wstring ik;
                    if (!JReadString(c, &ik)) break;
                    JSkipWs(c);
                    if (c.p >= c.end || *c.p != L':') break;
                    ++c.p;
                    JSkipWs(c);
                    if (ik == L"role" && c.p < c.end && *c.p == L'"') {
                        JReadString(c, &msg.role);
                    } else if (ik == L"finish") {
                        if (c.p < c.end && *c.p == L'"') JReadString(c, &msg.finish);
                        else JSkipValue(c);
                    } else if (ik == L"error" && c.p < c.end && *c.p == L'{') {
                        // info.error：{name:"ProviderAuthError"/"APIError"/...,
                        // data:{message, providerID?...}} → 归一化一行文本
                        ++c.p;
                        std::wstring ename, emsg;
                        for (;;) {
                            JSkipWs(c);
                            if (c.p >= c.end || *c.p == L'}') break;
                            if (*c.p == L',') { ++c.p; continue; }
                            std::wstring ek;
                            if (!JReadString(c, &ek)) break;
                            JSkipWs(c);
                            if (c.p >= c.end || *c.p != L':') break;
                            ++c.p;
                            JSkipWs(c);
                            if (ek == L"name" && c.p < c.end && *c.p == L'"') {
                                JReadString(c, &ename);
                            } else if (ek == L"data" && c.p < c.end && *c.p == L'{') {
                                ++c.p;
                                for (;;) {
                                    JSkipWs(c);
                                    if (c.p >= c.end || *c.p == L'}') break;
                                    if (*c.p == L',') { ++c.p; continue; }
                                    std::wstring dk;
                                    if (!JReadString(c, &dk)) break;
                                    JSkipWs(c);
                                    if (c.p >= c.end || *c.p != L':') break;
                                    ++c.p;
                                    JSkipWs(c);
                                    if (dk == L"message" && c.p < c.end &&
                                        *c.p == L'"')
                                        JReadString(c, &emsg);
                                    else
                                        JSkipValue(c);
                                }
                                JSkipWs(c);
                                if (c.p < c.end && *c.p == L'}') ++c.p;
                            } else {
                                JSkipValue(c);
                            }
                        }
                        JSkipWs(c);
                        if (c.p < c.end && *c.p == L'}') ++c.p;
                        if (!ename.empty()) msg.errText = ename;
                        if (!emsg.empty())
                            msg.errText += (msg.errText.empty() ? L"" : L": ") + emsg;
                    } else {
                        JSkipValue(c);
                    }
                }
                JSkipWs(c);
                if (c.p < c.end && *c.p == L'}') ++c.p;
            } else if (key == L"parts" && c.p < c.end && *c.p == L'[') {
                ++c.p;
                for (;;) {
                    JSkipWs(c);
                    if (c.p >= c.end || *c.p == L']') break;
                    if (*c.p == L',') { ++c.p; continue; }
                    if (*c.p != L'{') { JSkipValue(c); continue; }
                    ++c.p;   // 进入 part 对象
                    OcPart part;
                    for (;;) {
                        JSkipWs(c);
                        if (c.p >= c.end || *c.p == L'}') break;
                        if (*c.p == L',') { ++c.p; continue; }
                        std::wstring pk;
                        if (!JReadString(c, &pk)) break;
                        JSkipWs(c);
                        if (c.p >= c.end || *c.p != L':') break;
                        ++c.p;
                        JSkipWs(c);
                        if (pk == L"type" && c.p < c.end && *c.p == L'"') {
                            JReadString(c, &part.type);
                        } else if (pk == L"text" && c.p < c.end && *c.p == L'"') {
                            JReadString(c, &part.text);
                        } else if (pk == L"tool" && c.p < c.end && *c.p == L'"') {
                            JReadString(c, &part.tool);
                        } else if (pk == L"callID" && c.p < c.end && *c.p == L'"') {
                            JReadString(c, &part.callId);
                        } else if (pk == L"state" && c.p < c.end && *c.p == L'{') {
                            ++c.p;
                            for (;;) {   // state: {status, input, time, ...}
                                JSkipWs(c);
                                if (c.p >= c.end || *c.p == L'}') break;
                                if (*c.p == L',') { ++c.p; continue; }
                                std::wstring sk;
                                if (!JReadString(c, &sk)) break;
                                JSkipWs(c);
                                if (c.p >= c.end || *c.p != L':') break;
                                ++c.p;
                                JSkipWs(c);
                                if (sk == L"status" && c.p < c.end &&
                                    *c.p == L'"') {
                                    JReadString(c, &part.toolState);
                                } else if (sk == L"input" && c.p < c.end &&
                                           *c.p == L'{') {
                                    // write/edit 的目标路径在 input.filePath
                                    ++c.p;
                                    for (;;) {
                                        JSkipWs(c);
                                        if (c.p >= c.end || *c.p == L'}') break;
                                        if (*c.p == L',') { ++c.p; continue; }
                                        std::wstring ik2;
                                        if (!JReadString(c, &ik2)) break;
                                        JSkipWs(c);
                                        if (c.p >= c.end || *c.p != L':') break;
                                        ++c.p;
                                        JSkipWs(c);
                                        if (ik2 == L"filePath" &&
                                            c.p < c.end && *c.p == L'"')
                                            JReadString(c, &part.filePath);
                                        else
                                            JSkipValue(c);
                                    }
                                    JSkipWs(c);
                                    if (c.p < c.end && *c.p == L'}') ++c.p;
                                } else if (sk == L"metadata" && c.p < c.end &&
                                           *c.p == L'{') {
                                    // edit 完成后的 unified diff 在 metadata.diff
                                    ++c.p;
                                    for (;;) {
                                        JSkipWs(c);
                                        if (c.p >= c.end || *c.p == L'}') break;
                                        if (*c.p == L',') { ++c.p; continue; }
                                        std::wstring mk2;
                                        if (!JReadString(c, &mk2)) break;
                                        JSkipWs(c);
                                        if (c.p >= c.end || *c.p != L':') break;
                                        ++c.p;
                                        JSkipWs(c);
                                        if (mk2 == L"diff" &&
                                            c.p < c.end && *c.p == L'"')
                                            JReadString(c, &part.diff);
                                        else
                                            JSkipValue(c);
                                    }
                                    JSkipWs(c);
                                    if (c.p < c.end && *c.p == L'}') ++c.p;
                                } else {
                                    JSkipValue(c);   // time/metadata 等整体跳过
                                }
                            }
                            JSkipWs(c);
                            if (c.p < c.end && *c.p == L'}') ++c.p;
                        } else {
                            JSkipValue(c);   // tool 调用等结构化字段整体跳过
                        }
                    }
                    JSkipWs(c);
                    if (c.p < c.end && *c.p == L'}') ++c.p;
                    if (!part.type.empty() || !part.text.empty())
                        msg.parts.push_back(std::move(part));
                }
                JSkipWs(c);
                if (c.p < c.end && *c.p == L']') ++c.p;
            } else {
                JSkipValue(c);
            }
        }
        JSkipWs(c);
        if (c.p < c.end && *c.p == L'}') ++c.p;   // 消息对象闭合

        if (!msg.role.empty() || !msg.parts.empty())
            out->push_back(std::move(msg));
    }
    return true;
}

// /config/providers → {"providers":[...],"default":{"opencode":"big-pickle",...}}
// 保持 JSON 文档顺序输出 (provider, model) 对。
bool ParseProviderDefaults(const std::wstring& json,
                           std::vector<std::pair<std::wstring, std::wstring>>* out) {
    if (!out) return false;
    out->clear();
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return false;
    ++c.p;
    for (;;) {   // 顶层 key 循环
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'"') { JSkipValue(c); continue; }
        std::wstring key;
        if (!JReadString(c, &key)) break;
        JSkipWs(c);
        if (c.p >= c.end || *c.p != L':') break;
        ++c.p;
        JSkipWs(c);
        if (key == L"default" && c.p < c.end && *c.p == L'{') {
            ++c.p;
            for (;;) {   // default 对象：provider → model
                JSkipWs(c);
                if (c.p >= c.end || *c.p == L'}') break;
                if (*c.p == L',') { ++c.p; continue; }
                std::wstring prov;
                if (!JReadString(c, &prov)) break;
                JSkipWs(c);
                if (c.p >= c.end || *c.p != L':') break;
                ++c.p;
                JSkipWs(c);
                std::wstring model;
                if (c.p < c.end && *c.p == L'"') JReadString(c, &model);
                else JSkipValue(c);
                if (!prov.empty() && !model.empty())
                    out->push_back({prov, model});
            }
            JSkipWs(c);
            if (c.p < c.end && *c.p == L'}') ++c.p;
        } else {
            JSkipValue(c);
        }
    }
    return true;
}

// {"providers":[{"id":"p","name":"P","models":{"m":{"name":"M"}|{},...}},...]}
// → 目录条目；provider name/model name 缺省回退 id。保持文档顺序。
bool ParseProviderList(const std::wstring& json, std::vector<OcModelEntry>* out) {
    if (!out) return false;
    out->clear();
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return false;
    ++c.p;
    for (;;) {   // 顶层 key 循环
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'"') { JSkipValue(c); continue; }
        std::wstring key;
        if (!JReadString(c, &key)) break;
        JSkipWs(c);
        if (c.p >= c.end || *c.p != L':') break;
        ++c.p;
        JSkipWs(c);
        if (key != L"providers" || c.p >= c.end || *c.p != L'[') {
            JSkipValue(c);
            continue;
        }
        ++c.p;
        for (;;) {   // providers 数组循环
            JSkipWs(c);
            if (c.p >= c.end || *c.p == L']') break;
            if (*c.p == L',') { ++c.p; continue; }
            if (*c.p != L'{') { JSkipValue(c); continue; }
            ++c.p;
            std::wstring pid, pname;
            for (;;) {   // provider 对象：抓 id/name/models
                JSkipWs(c);
                if (c.p >= c.end || *c.p == L'}') break;
                if (*c.p == L',') { ++c.p; continue; }
                if (*c.p != L'"') { JSkipValue(c); continue; }
                std::wstring pk;
                if (!JReadString(c, &pk)) break;
                JSkipWs(c);
                if (c.p >= c.end || *c.p != L':') break;
                ++c.p;
                JSkipWs(c);
                if (pk == L"id" && c.p < c.end && *c.p == L'"') {
                    JReadString(c, &pid);
                } else if (pk == L"name" && c.p < c.end && *c.p == L'"') {
                    JReadString(c, &pname);
                } else if (pk == L"models" && c.p < c.end && *c.p == L'{') {
                    ++c.p;
                    for (;;) {   // models 对象：key = modelId
                        JSkipWs(c);
                        if (c.p >= c.end || *c.p == L'}') break;
                        if (*c.p == L',') { ++c.p; continue; }
                        if (*c.p != L'"') { JSkipValue(c); continue; }
                        std::wstring mid;
                        if (!JReadString(c, &mid)) break;
                        JSkipWs(c);
                        if (c.p >= c.end || *c.p != L':') break;
                        ++c.p;
                        JSkipWs(c);
                        std::wstring mname;
                        if (c.p < c.end && *c.p == L'{') {   // 取 value.name
                            ++c.p;
                            for (;;) {
                                JSkipWs(c);
                                if (c.p >= c.end || *c.p == L'}') break;
                                if (*c.p == L',') { ++c.p; continue; }
                                if (*c.p != L'"') { JSkipValue(c); continue; }
                                std::wstring mk;
                                if (!JReadString(c, &mk)) break;
                                JSkipWs(c);
                                if (c.p >= c.end || *c.p != L':') break;
                                ++c.p;
                                JSkipWs(c);
                                if (mk == L"name" && c.p < c.end && *c.p == L'"')
                                    JReadString(c, &mname);
                                else
                                    JSkipValue(c);
                            }
                            JSkipWs(c);
                            if (c.p < c.end && *c.p == L'}') ++c.p;
                        } else {
                            JSkipValue(c);
                        }
                        if (!pid.empty() && !mid.empty())
                            out->push_back({pid, pname, mid, mname});
                    }
                    JSkipWs(c);
                    if (c.p < c.end && *c.p == L'}') ++c.p;
                } else {
                    JSkipValue(c);
                }
            }
            JSkipWs(c);
            if (c.p < c.end && *c.p == L'}') ++c.p;
        }
        JSkipWs(c);
        if (c.p < c.end && *c.p == L']') ++c.p;
    }
    return true;
}

// {"ses_x":{"type":"busy"},...} → ses_x 是否 busy；顶层 key 命中即 busy
//（"type" 值暂只见过 "busy"/"idle"——key 存在即忙；空对象 {} = 空闲）
bool ParseSessionBusy(const std::wstring& json, const std::wstring& sessionId) {
    if (sessionId.empty()) return false;
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return false;   // 损坏响应按空闲处理
    ++c.p;
    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L'}') return false;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'"') { JSkipValue(c); continue; }
        std::wstring key;
        if (!JReadString(c, &key)) return false;
        if (key == sessionId) return true;
        JSkipWs(c);
        if (c.p < c.end && *c.p == L':') {
            ++c.p;
            JSkipValue(c);
        }
    }
}

// [{"id":"per_..","sessionID":"ses_..","permission":"external_directory",...},..]
// → 本会话条目的 (id, permission)。数组元素字段缺失/顺序任意均容错。
bool ParsePendingPermissions(
    const std::wstring& json, const std::wstring& sessionId,
    std::vector<std::pair<std::wstring, std::wstring>>* out) {
    if (!out) return false;
    out->clear();
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'[') return false;
    ++c.p;
    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L']') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'{') { JSkipValue(c); continue; }
        ++c.p;
        std::wstring id, sid, perm;
        for (;;) {
            JSkipWs(c);
            if (c.p >= c.end || *c.p == L'}') break;
            if (*c.p == L',') { ++c.p; continue; }
            std::wstring key;
            if (!JReadString(c, &key)) break;
            JSkipWs(c);
            if (c.p >= c.end || *c.p != L':') break;
            ++c.p;
            JSkipWs(c);
            if (key == L"id" && c.p < c.end && *c.p == L'"')
                JReadString(c, &id);
            else if (key == L"sessionID" && c.p < c.end && *c.p == L'"')
                JReadString(c, &sid);
            else if (key == L"permission" && c.p < c.end && *c.p == L'"')
                JReadString(c, &perm);
            else
                JSkipValue(c);
        }
        JSkipWs(c);
        if (c.p < c.end && *c.p == L'}') ++c.p;
        if (!id.empty() && sid == sessionId)
            out->push_back({std::move(id), std::move(perm)});
    }
    return true;
}

// [{"id":"ses_..","title":"..","directory":"D:\\..","time":{"updated":ms}},..]
// → 会话列表。容错：字段缺失跳过、updated 非 number 按 0、id 空条目丢弃。
// tokens: {input, output, reasoning, cache:{read}} → e 字段（批次 27）。
// c 停在 '{'；返回时停在 '}' 之后。字段缺失/顺序任意均容错。
void ParseTokensObj(JCursor& c, OcSessionEntry* e) {
    ++c.p;
    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        if (*c.p == L',') { ++c.p; continue; }
        std::wstring tk;
        if (!JReadString(c, &tk)) break;
        JSkipWs(c);
        if (c.p >= c.end || *c.p != L':') break;
        ++c.p;
        JSkipWs(c);
        long long* dst = tk == L"input"    ? &e->tokIn
                       : tk == L"output"   ? &e->tokOut
                       : tk == L"reasoning"? &e->tokReason
                       : nullptr;
        if (dst && c.p < c.end &&
            (*c.p == L'-' || (*c.p >= L'0' && *c.p <= L'9'))) {
            *dst = wcstoll(c.p, nullptr, 10);
            JSkipValue(c);
        } else if (tk == L"cache" && c.p < c.end && *c.p == L'{') {
            ++c.p;
            for (;;) {   // cache: {read, write}
                JSkipWs(c);
                if (c.p >= c.end || *c.p == L'}') break;
                if (*c.p == L',') { ++c.p; continue; }
                std::wstring ck;
                if (!JReadString(c, &ck)) break;
                JSkipWs(c);
                if (c.p >= c.end || *c.p != L':') break;
                ++c.p;
                JSkipWs(c);
                if (ck == L"read" && c.p < c.end &&
                    (*c.p == L'-' || (*c.p >= L'0' && *c.p <= L'9'))) {
                    e->tokCacheRead = wcstoll(c.p, nullptr, 10);
                    JSkipValue(c);
                } else {
                    JSkipValue(c);
                }
            }
            JSkipWs(c);
            if (c.p < c.end && *c.p == L'}') ++c.p;
        } else {
            JSkipValue(c);
        }
    }
    JSkipWs(c);
    if (c.p < c.end && *c.p == L'}') ++c.p;
}

bool ParseSessionList(const std::wstring& json, std::vector<OcSessionEntry>* out) {
    if (!out) return false;
    out->clear();
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'[') return false;
    ++c.p;
    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L']') break;
        if (*c.p == L',') { ++c.p; continue; }
        if (*c.p != L'{') { JSkipValue(c); continue; }
        ++c.p;
        OcSessionEntry e;
        for (;;) {
            JSkipWs(c);
            if (c.p >= c.end || *c.p == L'}') break;
            if (*c.p == L',') { ++c.p; continue; }
            std::wstring key;
            if (!JReadString(c, &key)) break;
            JSkipWs(c);
            if (c.p >= c.end || *c.p != L':') break;
            ++c.p;
            JSkipWs(c);
            if (key == L"id" && c.p < c.end && *c.p == L'"')
                JReadString(c, &e.id);
            else if (key == L"title" && c.p < c.end && *c.p == L'"')
                JReadString(c, &e.title);
            else if (key == L"directory" && c.p < c.end && *c.p == L'"')
                JReadString(c, &e.directory);
            else if (key == L"cost" && c.p < c.end &&
                     (*c.p == L'-' || (*c.p >= L'0' && *c.p <= L'9'))) {
                e.cost = wcstod(c.p, nullptr);
                JSkipValue(c);   // 数值吃掉（wcstod 只读不前移）
            }
            else if (key == L"tokens" && c.p < c.end && *c.p == L'{')
                ParseTokensObj(c, &e);
            else if (key == L"time" && c.p < c.end && *c.p == L'{') {
                ++c.p;
                for (;;) {   // time: {created, updated}（epoch ms）
                    JSkipWs(c);
                    if (c.p >= c.end || *c.p == L'}') break;
                    if (*c.p == L',') { ++c.p; continue; }
                    std::wstring tk;
                    if (!JReadString(c, &tk)) break;
                    JSkipWs(c);
                    if (c.p >= c.end || *c.p != L':') break;
                    ++c.p;
                    JSkipWs(c);
                    if (tk == L"updated" && c.p < c.end &&
                        (*c.p == L'-' || (*c.p >= L'0' && *c.p <= L'9'))) {
                        e.updated = wcstoll(c.p, nullptr, 10);
                        JSkipValue(c);
                    } else {
                        JSkipValue(c);
                    }
                }
                JSkipWs(c);
                if (c.p < c.end && *c.p == L'}') ++c.p;
            } else {
                JSkipValue(c);
            }
        }
        JSkipWs(c);
        if (c.p < c.end && *c.p == L'}') ++c.p;
        if (!e.id.empty()) out->push_back(std::move(e));
    }
    return true;
}

bool ParseSessionDetail(const std::wstring& json, OcSessionEntry* out) {
    if (!out) return false;
    *out = OcSessionEntry{};
    JCursor c{json.data(), json.data() + json.size()};
    JSkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return false;
    ++c.p;
    for (;;) {
        JSkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        if (*c.p == L',') { ++c.p; continue; }
        std::wstring key;
        if (!JReadString(c, &key)) return false;
        JSkipWs(c);
        if (c.p >= c.end || *c.p != L':') return false;
        ++c.p;
        JSkipWs(c);
        if (key == L"id" && c.p < c.end && *c.p == L'"')
            JReadString(c, &out->id);
        else if (key == L"cost" && c.p < c.end &&
                 (*c.p == L'-' || (*c.p >= L'0' && *c.p <= L'9'))) {
            out->cost = wcstod(c.p, nullptr);
            JSkipValue(c);
        } else if (key == L"tokens" && c.p < c.end && *c.p == L'{')
            ParseTokensObj(c, out);
        else
            JSkipValue(c);
    }
    return !out->id.empty();
}

// ---- token/cost 显示格式化（批次 27） --------------------------------------
// FormatTokens：一位小数缩写，整数千/百万不带 .0（1000→"1k"、1500→"1.5k"、
// 2500000→"2.5M"、123456789→"123M"）；0/负 → "0"。
std::wstring FormatTokens(long long n) {
    if (n <= 0) return L"0";
    wchar_t buf[32];
    if (n < 1000) {
        swprintf_s(buf, L"%lld", n);
        return buf;
    }
    auto trim = [](const std::wstring& s) {
        // "1.0k"/"2.0M" → "1k"/"2M"（去掉 ".0" 两位，单位保留）
        if (s.size() > 3 &&
            (s.compare(s.size() - 3, 3, L".0k") == 0 ||
             s.compare(s.size() - 3, 3, L".0M") == 0))
            return s.substr(0, s.size() - 3) + s.substr(s.size() - 1);
        return s;
    };
    if (n < 1000000) {
        double v = n / 1000.0;
        if (v >= 100) { swprintf_s(buf, L"%.0fk", v); return buf; }
        swprintf_s(buf, L"%.1fk", v);
        return trim(buf);
    }
    double v = n / 1000000.0;
    if (v >= 100) { swprintf_s(buf, L"%.0fM", v); return buf; }
    swprintf_s(buf, L"%.1fM", v);
    return trim(buf);
}

// FormatCost：<=0 → 空（调用方整段省略）；<1 美分 → "<$0.01"；其余两位小数。
std::wstring FormatCost(double cost) {
    if (cost <= 0) return L"";
    if (cost < 0.01) return L"<$0.01";
    wchar_t buf[32];
    swprintf_s(buf, L"$%.2f", cost);
    return buf;
}

std::string BuildPromptBody(const std::wstring& text,
                            const std::wstring& providerID,
                            const std::wstring& modelID) {
    // {"parts":[{"type":"text","text":"..."}]}
    // 带 model 时在 parts 前插入 "model":{"providerID":"..","modelID":".."}
    // 转义在 wstring 域内完成，最后一次性 WideToUtf8——在 std::string 上
    // 直接 += wchar_t 会截断非 ASCII（中文变乱码，2026-09-05 review 发现）
    std::wstring body = L"{";
    if (!providerID.empty() && !modelID.empty()) {
        body += L"\"model\":{\"providerID\":\"" + providerID +
                L"\",\"modelID\":\"" + modelID + L"\"},";
    }
    body += L"\"parts\":[{\"type\":\"text\",\"text\":\"";
    for (wchar_t ch : text) {
        switch (ch) {
            case L'"':  body += L"\\\""; break;
            case L'\\': body += L"\\\\"; break;
            case L'\n': body += L"\\n";  break;
            case L'\r': body += L"\\r";  break;
            case L'\t': body += L"\\t";  break;
            default:    body += ch; break;
        }
    }
    body += L"\"}]}";
    return WideToUtf8(body);
}

// -------------------------------------------------------------------- HTTP

bool OcClient::Request(const wchar_t* verb, const std::wstring& path,
                       const std::string& bodyUtf8, std::string* body,
                       DWORD timeoutMs) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"xfsWinPad-AI/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    unsigned port = g_portOverride.load(std::memory_order_relaxed);
    HINTERNET con = WinHttpConnect(ses, kHost,
                                   port ? (INTERNET_PORT)port : kPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, verb, path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                        : nullptr;
    do {
        if (!req) break;
        // 127.0.0.1 直连禁代理；超时统一收口（探活短、消息长由入参控制）
        DWORD to = timeoutMs;
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(req, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(req, WINHTTP_OPTION_SEND_TIMEOUT, &to, sizeof(to));
        const wchar_t* hdrs = L"Content-Type: application/json\r\n";
        if (!WinHttpSendRequest(req, hdrs, (DWORD)-1,
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
        if (status >= 200 && status < 300) {
            if (body) *body = std::move(resp);
            ok = true;
        } else {
            Logger::Warn("OcClient: HTTP " + std::to_string(status) + " " +
                         WideToUtf8(path) + " resp=" + resp.substr(0, 200));
        }
    } while (false);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

bool OcClient::ProbeHealth(std::wstring* version) {
    std::string body;
    if (!Request(L"GET", L"/global/health", "", &body, 2500))
        return false;
    std::wstring j = Utf8ToWide(body);
    if (j.find(L"healthy") == std::wstring::npos) return false;
    if (version) *version = ParseVersion(j);
    return true;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring OcClient::FindOpencodeExe() {
    // PATH 上找 opencode.cmd / opencode.exe / opencode（npm 全局壳是 .cmd）
    static const wchar_t* names[] = { L"opencode.cmd", L"opencode.exe", L"opencode" };
    wchar_t pathBuf[MAX_PATH * 4];
    for (const wchar_t* n : names) {
        DWORD len = ::SearchPathW(nullptr, n, nullptr,
                                  (DWORD)ARRAYSIZE(pathBuf), pathBuf, nullptr);
        if (len > 0 && len < ARRAYSIZE(pathBuf)) {
            std::wstring hit = pathBuf;
            // npm 的 .cmd 壳只是跳板（%~dp0\node_modules\...\opencode.exe）；
            // spawn 链条上 cmd.exe 在 GUI 父进程 + CREATE_NO_WINDOW 下会立即
            // 退出（实测），直接解析出真实 exe 优先使用。
            if (hit.size() > 4 && hit.rfind(L".cmd", hit.size() - 4) == hit.size() - 4) {
                std::wstring dir = hit.substr(0, hit.find_last_of(L"\\/"));
                std::wstring real = dir +
                    L"\\node_modules\\opencode-ai\\bin\\opencode.exe";
                if (FileExists(real)) {
                    Logger::Info("OcClient: FindOpencodeExe PATH hit " +
                                 WideToUtf8(hit) + " -> real " + WideToUtf8(real));
                    return real;
                }
            }
            Logger::Info("OcClient: FindOpencodeExe PATH hit " + WideToUtf8(hit));
            return hit;
        }
    }
    // PATH 没有时的补充查找（桌面版 opencode GUI 不往 PATH 装 CLI；
    // 官方 install.ps1 装 %USERPROFILE%\.opencode\bin，npm 全局在
    // %APPDATA%\npm；xfsWinPad 安装器把 CLI 下到 <exe 目录>\bin）
    std::wstring exeDir(MAX_PATH * 4, L'\0');
    DWORD n = ::GetModuleFileNameW(nullptr, &exeDir[0], (DWORD)exeDir.size());
    if (n > 0 && n < exeDir.size()) {
        exeDir.resize(n);
        size_t slash = exeDir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::wstring base = exeDir.substr(0, slash);
            for (const wchar_t* nm : names) {
                std::wstring cand = base + L"\\" + nm;
                if (FileExists(cand)) return cand;
            }
            // 安装器下载位：<exe 目录>\bin\opencode.exe
            for (const wchar_t* nm : names) {
                std::wstring cand = base + L"\\bin\\" + nm;
                if (FileExists(cand)) return cand;
            }
        }
    }
    if (const wchar_t* up = ::_wgetenv(L"USERPROFILE")) {
        std::wstring cand = std::wstring(up) + L"\\.opencode\\bin\\opencode.exe";
        if (FileExists(cand)) return cand;
    }
    if (const wchar_t* ad = ::_wgetenv(L"APPDATA")) {
        std::wstring cand = std::wstring(ad) + L"\\npm\\opencode.cmd";
        if (FileExists(cand)) return cand;
    }
    return L"";
}

bool OcClient::SpawnServeDetached(const std::wstring& workDir) {
    std::wstring exe = FindOpencodeExe();
    if (exe.empty()) return false;
    // 隐藏窗口启动：opencode.exe 是 console 子系统程序，直接 CreateProcess
    // （或经 cmd.exe /c / WMI Win32_Process.Create 间接起）都会分配一个
    // 新控制台窗口（任务栏可见）——学生用户容易把它当无关程序关掉导致
    // AI 断连。CREATE_NO_WINDOW = 无窗口无任务栏项（实测 MainWindowHandle=0）。
    //
    // 存活性：Windows 进程模型里父退出不影响子进程（本项目无 Job 对象
    // 约束），serve 在编辑器退出后继续运行，下次启动 ProbeHealth 直连。
    // 曾试过 WMI Win32_Process.Create（真父=WmiPrvSE）+ cmd.exe /c，
    // 能脱离但控制台窗口藏不掉；wscript.exe 壳链路在 WMI 启动环境下
    // 静默不执行（手动跑同 js 同参数正常）——放弃。
    std::wstring app;   // lpApplicationName（必须全路径，非空）
    std::wstring cmd;   // lpCommandLine
    if (exe.size() > 4 && (exe.rfind(L".cmd", exe.size() - 4) == exe.size() - 4 ||
                           exe.rfind(L".bat", exe.size() - 4) == exe.size() - 4)) {
        // 兜底：仍是 .cmd/.bat（npm 布局不符）——经 cmd.exe /c（全路径；
        // NO_WINDOW 下无窗口；GUI 父下此链路可能即退，属最后手段）。
        wchar_t sysDir[MAX_PATH];
        UINT sysLen = ::GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring cmdExe = (sysLen > 0 && sysLen < MAX_PATH)
                                  ? std::wstring(sysDir) + L"\\cmd.exe"
                                  : std::wstring(L"cmd.exe");
        app = cmdExe;
        cmd = L"\"" + cmdExe + L"\" /c \"" + exe + L"\" serve --port " +
              std::to_wstring((unsigned)kPort) + L" --hostname " + kHost;
    } else {
        app = exe;
        cmd = L"\"" + exe + L"\" serve --port " +
              std::to_wstring((unsigned)kPort) + L" --hostname " + kHost;
    }

    STARTUPINFOW si;
    ::ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ::ZeroMemory(&pi, sizeof(pi));
    // CREATE_NO_WINDOW：为 console 子系统进程不分配可见控制台；
    // CREATE_UNICODE_ENVIRONMENT：自定义环境块是 wchar（lpEnvironment 为
    // nullptr 时系统默认 Unicode，一旦传入必须显式声明，否则 err=87）
    constexpr DWORD kNoWindow = 0x08000000;
    constexpr DWORD kUnicodeEnv = 0x00000400;
    std::wstring dir = workDir.empty() ? std::wstring(L"C:\\") : workDir;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    // 环境净化：opencode 桌面版会设置用户级 OPENCODE_SERVER_USERNAME/
    // OPENCODE_SERVER_PASSWORD，serve 读到即开启 HTTP Basic 认证，
    // 而我们（本机 127.0.0.1 直连）不带鉴权头 → 全部 401。构造去掉
    // 这两个变量的环境块，保证 serve 免鉴权（仅监听本机回环，无风险）。
    std::vector<wchar_t> envBuf;
    {
        LPWCH blk = ::GetEnvironmentStringsW();
        if (blk) {
            LPWCH p = blk;
            while (*p) {
                std::wstring entry(p);
                bool skip = entry.rfind(L"OPENCODE_SERVER_USERNAME=", 0) == 0 ||
                            entry.rfind(L"OPENCODE_SERVER_PASSWORD=", 0) == 0;
                // 只保留的条目才写入（含结尾 \0）——跳过的条目若也 push
                // \0 会产生空条目，等于环境块提前双 \0 终止，子进程丢
                // PATH/SystemRoot 直接 fail-fast（0xC0000409）。
                if (!skip) {
                    envBuf.insert(envBuf.end(), entry.begin(), entry.end());
                    envBuf.push_back(L'\0');
                }
                p += entry.size() + 1;
            }
            ::FreeEnvironmentStringsW(blk);
        }
        envBuf.push_back(L'\0');
    }
    LPWCH envPtr = envBuf.empty() ? nullptr : envBuf.data();
    std::vector<wchar_t> appBuf(app.begin(), app.end());
    appBuf.push_back(L'\0');
    // serve 的 stdout/stderr 追加到 exe 同目录 debug.log——zen 网关不可达、
    // provider 初始化失败等真实报错都在 serve 侧输出里，现场排查的钥匙。
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE logFile = ::CreateFileW(Logger::DebugLogPath().c_str(),
                                   FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si2 = si;   // 保持 si 结构不动，下面按需加标志
    if (logFile != INVALID_HANDLE_VALUE) {
        si2.dwFlags |= STARTF_USESTDHANDLES;
        si2.hStdInput = nullptr;
        si2.hStdOutput = logFile;
        si2.hStdError = logFile;
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        char banner[160];
        int bn = _snprintf_s(banner, sizeof(banner), _TRUNCATE,
                             "\r\n---- opencode serve spawn %04u-%02u-%02u "
                             "%02u:%02u:%02u ----\r\n",
                             st.wYear, st.wMonth, st.wDay, st.wHour,
                             st.wMinute, st.wSecond);
        if (bn > 0) {
            DWORD w = 0;
            ::WriteFile(logFile, banner, (DWORD)bn, &w, nullptr);
        }
    }
    BOOL created = ::CreateProcessW(appBuf.data(), cmdBuf.data(), nullptr,
                                    nullptr, logFile != INVALID_HANDLE_VALUE,
                                    kNoWindow | kUnicodeEnv,
                                    envPtr, dir.c_str(), &si2, &pi);
    if (logFile != INVALID_HANDLE_VALUE) ::CloseHandle(logFile);
    bool ok = created != FALSE;
    if (ok) {
        Logger::Info("OcClient: spawned opencode serve (CREATE_NO_WINDOW), cwd=" +
                     WideToUtf8(workDir) + " app=" + WideToUtf8(app));
        // pidfile：复用卫生（EnsureServer 校验 exe 漂移 → 杀旧拉新）。
        // 注意 pi.dwProcessId 是 opencode.exe 自身；.cmd 兜底链路里它是
        // cmd.exe 的 pid——cmd /c 会 exec 成 opencode？实测 cmd /c 起
        // console 程序不替换自身，pid 会随 cmd 退出失效 → pidfile 过期
        // → 复用保持现状（安全侧：不杀不误伤）。
        WritePidFile(pi.dwProcessId, app);
        if (logFile == INVALID_HANDLE_VALUE)
            Logger::Warn("OcClient: serve stderr redirect unavailable err=" +
                         std::to_string(::GetLastError()));
    } else
        Logger::Error("OcClient: CreateProcessW failed err=" +
                      std::to_string(::GetLastError()));
    if (pi.hThread) ::CloseHandle(pi.hThread);
    if (pi.hProcess) {
        // 诊断（保留）：若 serve 秒退，抓退出码进日志——常见 3221225786
        // (0xC000013A=CTRL+C) 或 config 错误 1。
        if (::WaitForSingleObject(pi.hProcess, 2500) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            ::GetExitCodeProcess(pi.hProcess, &ec);
            Logger::Error("OcClient: serve exited immediately, code=" +
                          std::to_string((unsigned long long)ec));
        }
        ::CloseHandle(pi.hProcess);
    }
    return ok;
}

// auth.json 污染预检（批次 18 教训）：条目键为 "opencode" 的值若是
// OpenRouter 格式（sk-or- 前缀）等异通道 key，serve 会对所有 opencode/*
// 免费模型带坏 key 请求 → 网关 401 → 消息静默失败永挂"生成中"。
// 只 WARN 不阻止（也可能是合法的 zen 登录 key——无法本地判定有效性）。
// 返回 true = 检出可疑条目（调用方决定是否提示用户）。
bool OcClient::DetectPoisonedZenAuth() {
    const wchar_t* up = ::_wgetenv(L"USERPROFILE");
    if (!up) return false;
    std::wstring path = std::wstring(up) +
        L"\\.local\\share\\opencode\\auth.json";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;   // 没登录过 = 匿名免费通道
    char buf[8192] = {};
    DWORD got = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    ::CloseHandle(h);
    // 轻量启发：找 "opencode" 键 + 邻近的 sk-or- 值前缀（OpenRouter key）。
    // JSON 全解析过重；两个特征都命中才报，避免误伤正常 zen key。
    std::string body(buf, got);
    size_t kpos = body.find("\"opencode\"");
    if (kpos == std::string::npos) return false;
    if (body.find("sk-or-", kpos) == std::string::npos &&
        body.find("sk-or-", 0) == std::string::npos)
        return false;
    Logger::Warn("OcClient: auth.json has 'opencode' entry with OpenRouter-"
                 "style key - zen free models will 401");
    return true;
}

bool OcClient::EnsureServer(const std::wstring& workDir, bool* notInstalled) {
    if (notInstalled) *notInstalled = false;
    if (ProbeHealth()) {
        // 复用卫生：pidfile 记录的是我们 spawn 的 serve（pid+exe 路径）。
        // exe 与当前解析结果不一致 = 编辑器升级/换安装位后旧 serve 仍挂着
        //（批次 20 教训：坏状态孤儿会被无限复用）→ 杀掉重拉。
        // pidfile 读不出/pid 死（手动起的 serve、跨机场景）→ 保持现状复用，
        // 不冒进（ProbeHealth 已确认 4096 上是活的 opencode）。
        std::wstring curExe = FindOpencodeExe();
        DWORD pid = 0;
        std::wstring exePath;
        if (ReadPidFile(&pid, &exePath) && !exePath.empty() &&
            !curExe.empty() && _wcsicmp(exePath.c_str(), curExe.c_str()) != 0) {
            Logger::Warn(
                "OcClient: stale serve from different install (pid=" +
                std::to_string(pid) + " exe=" + WideToUtf8(exePath) +
                " current=" + WideToUtf8(curExe) + ") - killing for respawn");
            if (IsProcessAlive(pid, L"opencode")) {
                HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (h) {
                    ::TerminateProcess(h, 1);
                    ::CloseHandle(h);
                    ::Sleep(500);   // 让端口释放
                    Logger::Info("OcClient: stale serve killed");
                }
            }
            if (FindOpencodeExe().empty()) {
                if (notInstalled) *notInstalled = true;
                return false;
            }
            if (!SpawnServeDetached(workDir)) {
                Logger::Error("OcClient: failed to respawn serve after kill");
                return false;
            }
            for (int i = 0; i < 24; ++i) {
                ::Sleep(500);
                if (ProbeHealth()) {
                    Logger::Info("OcClient: opencode server (respawned) ready");
                    return true;
                }
            }
            Logger::Error("OcClient: respawned server not ready within 12s");
            return false;
        }
        return true;
    }

    if (FindOpencodeExe().empty()) {
        if (notInstalled) *notInstalled = true;
        Logger::Warn("OcClient: opencode not found on PATH");
        return false;
    }
    if (!SpawnServeDetached(workDir)) {
        Logger::Error("OcClient: failed to spawn opencode serve");
        return false;
    }
    // 轮询就绪（serve 冷启 ~1-2s；上限 12s）
    for (int i = 0; i < 24; ++i) {
        ::Sleep(500);
        if (ProbeHealth()) {
            Logger::Info("OcClient: opencode server ready after " +
                         std::to_string((i + 1) * 500) + "ms");
            return true;
        }
    }
    Logger::Error("OcClient: opencode server not ready within 12s");
    return false;
}

std::wstring OcClient::ServePidPath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                         &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    return base + L"\\xfsWinPad\\serve.pid";
}

bool OcClient::ReadPidFile(DWORD* pid, std::wstring* exePath) {
    HANDLE h = ::CreateFileW(ServePidPath().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[1024] = {};
    DWORD got = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    ::CloseHandle(h);
    // "pid|exe路径"；exePath 容忍缺失（老格式只有 pid）
    std::string body(buf, got);
    size_t bar = body.find('|');
    try {
        if (pid)
            *pid = (DWORD)std::stoul(body.substr(0, bar));
    } catch (...) {
        return false;
    }
    if (exePath) {
        if (bar != std::string::npos) {
            std::string tail = body.substr(bar + 1);
            // 去掉行尾 \r\n（写文件时补的）
            while (!tail.empty() &&
                   (tail.back() == '\r' || tail.back() == '\n'))
                tail.pop_back();
            *exePath = Utf8ToWide(tail);
        }
    }
    return true;
}

void OcClient::WritePidFile(DWORD pid, const std::wstring& exePath) {
    std::wstring dir = ServePidPath();
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        ::CreateDirectoryW(dir.substr(0, slash).c_str(), nullptr);
    HANDLE h = ::CreateFileW(dir.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string body = std::to_string(pid) + "|" + WideToUtf8(exePath);
    DWORD w = 0;
    ::WriteFile(h, body.c_str(), (DWORD)body.size(), &w, nullptr);
    ::CloseHandle(h);
}

bool OcClient::IsProcessAlive(DWORD pid, const wchar_t* expectName) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t name[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = ::QueryFullProcessImageNameW(h, 0, name, &size);
    ::CloseHandle(h);
    if (!ok) return false;
    // 只比对文件名（不区分大小写）：expectName="opencode" 匹配
    // ...\opencode.exe，不误伤同名不同物（路径级异同交给 exePath 比对）
    std::wstring full(name);
    size_t slash = full.find_last_of(L"\\/");
    std::wstring fname =
        (slash != std::wstring::npos) ? full.substr(slash + 1) : full;
    size_t dot = fname.rfind(L'.');
    if (dot != std::wstring::npos) fname = fname.substr(0, dot);
    return _wcsicmp(fname.c_str(), expectName) == 0;
}

std::wstring OcClient::CreateSession(const std::wstring& title) {
    std::wstring bodyJson = L"{\"title\":\"" + title + L"\"}";
    std::string resp;
    if (!Request(L"POST", L"/session", WideToUtf8(bodyJson), &resp, 10000))
        return L"";
    return ParseSessionId(Utf8ToWide(resp));
}

bool OcClient::FetchProviderDefaults(
    std::vector<std::pair<std::wstring, std::wstring>>* out) {
    std::string resp;
    if (!Request(L"GET", L"/config/providers", "", &resp, 10000))
        return false;
    return ParseProviderDefaults(Utf8ToWide(resp), out);
}

bool OcClient::FetchModelList(std::vector<OcModelEntry>* out) {
    std::string resp;
    if (!Request(L"GET", L"/config/providers", "", &resp, 10000))
        return false;
    return ParseProviderList(Utf8ToWide(resp), out);
}

bool OcClient::PromptAsync(const std::wstring& sessionId, const std::wstring& text,
                           const std::wstring& providerID,
                           const std::wstring& modelID) {
    std::wstring path = L"/session/" + sessionId + L"/prompt_async";
    return Request(L"POST", path, BuildPromptBody(text, providerID, modelID),
                   nullptr, 10000);
}

// 带错误回报版本：HTTP 状态码/连接失败翻译成可读文本（UI 直接展示）
bool OcClient::PromptAsync(const std::wstring& sessionId, const std::wstring& text,
                           const std::wstring& providerID,
                           const std::wstring& modelID, std::wstring* err) {
    std::wstring path = L"/session/" + sessionId + L"/prompt_async";
    std::string resp;
    Logger::Info("OcClient: prompt_async submit sid=" + WideToUtf8(sessionId) +
                 " model=" + WideToUtf8(providerID) + "/" + WideToUtf8(modelID) +
                 " bytes=" + std::to_string(text.size()));
    bool ok = Request(L"POST", path, BuildPromptBody(text, providerID, modelID),
                      &resp, 10000);
    if (ok) {
        Logger::Info("OcClient: prompt_async accepted");
        return true;
    }
    if (err) {
        // Request 失败时区分：拿到了状态码（resp 仍空/带体）vs 连接级失败。
        // Request 没吐状态码——用 ProbeHealth 判定服务端是否还活着来分类。
        if (ProbeHealth()) {
            *err = L"HTTP " + Utf8ToWide(resp.substr(0, 120)) +
                   L" (server refused)";
            Logger::Error("OcClient: prompt_async refused: " +
                          WideToUtf8(*err));
        } else {
            *err = L"server unreachable";
            Logger::Error("OcClient: prompt_async failed: server unreachable");
        }
    }
    return false;
}

bool OcClient::ListMessages(const std::wstring& sessionId, std::vector<OcMessage>* out) {
    std::wstring path = L"/session/" + sessionId + L"/message";
    std::string resp;
    if (!Request(L"GET", path, "", &resp, 5000)) return false;
    return ParseMessagesJson(Utf8ToWide(resp), out);
}

// 请求失败按 busy 返回（true）：生成判定宁可拖后不能提前
bool OcClient::SessionBusy(const std::wstring& sessionId) {
    std::string resp;
    if (!Request(L"GET", L"/session/status", "", &resp, 5000))
        return true;
    return ParseSessionBusy(Utf8ToWide(resp), sessionId);
}

bool OcClient::ListPendingPermissions(
    const std::wstring& sessionId,
    std::vector<std::pair<std::wstring, std::wstring>>* out) {
    std::string resp;
    if (!Request(L"GET", L"/permission", "", &resp, 5000)) return false;
    return ParsePendingPermissions(Utf8ToWide(resp), sessionId, out);
}

// always+remember：实测放行后同目录后续操作（含跨 session）不再询问
bool OcClient::AnswerPermission(const std::wstring& sessionId,
                                const std::wstring& permissionId) {
    std::wstring path = L"/session/" + sessionId +
                        L"/permissions/" + permissionId;
    return Request(L"POST", path, R"({"response":"always","remember":true})",
                   nullptr, 5000);
}

bool OcClient::ListSessions(std::vector<OcSessionEntry>* out) {
    std::string resp;
    if (!Request(L"GET", L"/session", "", &resp, 10000)) return false;
    return ParseSessionList(Utf8ToWide(resp), out);
}

bool OcClient::GetSession(const std::wstring& sessionId,
                          OcSessionEntry* out) {
    if (sessionId.empty() || !out) return false;
    std::wstring path = L"/session/" + sessionId;
    std::string resp;
    if (!Request(L"GET", path, "", &resp, 10000)) return false;
    return ParseSessionDetail(Utf8ToWide(resp), out);
}

void OcClient::Abort(const std::wstring& sessionId) {
    std::wstring path = L"/session/" + sessionId + L"/abort";
    Request(L"POST", path, "", nullptr, 5000);
}

void OcClient::SetPortOverride(unsigned port) {
    g_portOverride.store(port);
}

bool OcClient::EventSubscribe(OcEventFn onEvent, void* user,
                              std::atomic<bool>* cancelFlag) {
    HINTERNET ses = WinHttpOpen(L"xfsWinPad-AI/1.0",
                                WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    unsigned port = g_portOverride.load(std::memory_order_relaxed);
    HINTERNET con = WinHttpConnect(ses, kHost,
                                   port ? (INTERNET_PORT)port : kPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/event",
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                        : nullptr;
    bool ok = false;
    do {
        if (!req) break;
        // 长连接：接收超时 10s（真机/mock 心跳 ≤1s 间隔，远小于此；
        // 断流 10s 内返回 false → 调用方重连。cancelFlag 置位后最多
        // 等 10s——阻塞中的 ReadData 无法立即唤醒，面板 Destroy 走
        // detach 不等它）。
        DWORD to = 10000;
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(req, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(req, WINHTTP_OPTION_SEND_TIMEOUT, &to, sizeof(to));
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            break;
        if (!WinHttpReceiveResponse(req, nullptr)) break;
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) break;
        // SSE 行缓冲（跨 TCP 包）：event 帧与 data 帧。"data: " 前缀 5 字符
        //（批次 24 的坑：compare 长度必须与前缀一致）。
        // 读循环必须先 QueryDataAvailable 再按实际量 ReadData：同步
        // ReadData(4096) 会凑满请求数才返回，对永不关连接的 SSE 流
        // 等于死等（批次 26 e2e 0 字节根因）；QueryDataAvailable 只等
        // "已有" 到达的字节，心跳/事件一到立刻交付。
        std::string pending, lineBytes;
        char buf[4096];
        DWORD rd = 0, avail = 0;
        ok = true;
        while (!(cancelFlag && cancelFlag->load()) &&
               WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            DWORD want =
                avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (!WinHttpReadData(req, buf, want, &rd) || rd == 0) break;
            pending.append(buf, rd);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                lineBytes.append(pending, 0, nl);
                pending.erase(0, nl + 1);
                if (!lineBytes.empty() && lineBytes.back() == '\r')
                    lineBytes.pop_back();
                std::wstring line = Utf8ToWide(lineBytes);
                lineBytes.clear();
                if (line.compare(0, 5, L"data:") != 0) continue;
                std::wstring payload = line.substr(5);
                while (!payload.empty() &&
                       (payload.front() == L' ' || payload.front() == L'\t'))
                    payload.erase(payload.begin());
                std::wstring type, sid;
                if (!ParseEventFrame(payload, &type, &sid)) continue;
                if (onEvent &&
                    (type.rfind(L"message.", 0) == 0 ||
                     type.rfind(L"session.", 0) == 0))
                    onEvent(user, type, sid);
            }
        }
    } while (false);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

} // namespace ai
} // namespace xfs
