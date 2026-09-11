// xfsWinPad - test_ai_json: OcClient 纯 JSON 助手单测
//（解析 session id / version / messages、prompt body 构造、转义回环）
#include "../src/ai/OcClient.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace xfs::ai;

static int g_failed = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        ++g_failed; \
        std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

int main() {
    // ---- ParseSessionId ----
    CHECK(ParseSessionId(LR"({"id":"ses_f9477d821ffeytHWwqxOWYu7uy","title":"x"})") ==
          L"ses_f9477d821ffeytHWwqxOWYu7uy");
    CHECK(ParseSessionId(LR"({"title":"first","id":"ses_abc"})") == L"ses_abc");
    CHECK(ParseSessionId(L"{}").empty());
    CHECK(ParseSessionId(L"not json").empty());
    CHECK(ParseSessionId(LR"({"id":123})").empty());   // 非字符串 id → 空

    // ---- ParseVersion ----
    CHECK(ParseVersion(LR"({"healthy":true,"version":"1.18.27"})") == L"1.18.27");
    CHECK(ParseVersion(LR"({"healthy":true})").empty());

    // ---- ParseMessagesJson：真实形态（info+parts、未知字段、乱序） ----
    {
        const wchar_t* j =
            LR"([{"info":{"id":"msg_1","role":"user","time":{"created":1}},"parts":[{"type":"step-start"},{"type":"text","text":"你好 <世界> \"引号\""}]},{"info":{"role":"assistant","finish":"stop","tokens":123},"parts":[{"type":"reasoning","text":"think..."},{"type":"text","text":"2"},{"type":"tool","state":{"name":"read"}}]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 2);
        CHECK(out[0].role == L"user");
        CHECK(out[0].parts.size() == 2);
        CHECK(out[0].parts[1].text == LR"(你好 <世界> "引号")");
        CHECK(out[1].role == L"assistant");
        CHECK(out[1].finish == L"stop");
        // reasoning + text + tool 三段
        CHECK(out[1].parts.size() == 3);
        CHECK(out[1].parts[0].type == L"reasoning");
        CHECK(out[1].parts[1].text == L"2");
        CHECK(out[1].parts[2].type == L"tool");
    }
    // 空数组 / 损坏输入
    {
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(L"[]", &out) && out.empty());
        CHECK(!ParseMessagesJson(L"{}", &out));
        CHECK(!ParseMessagesJson(L"broken", &out));
    }
    // 嵌套字符串中的花括号/引号不破坏扫描
    {
        const wchar_t* j =
            LR"([{"info":{"role":"assistant","finish":"stop"},"parts":[{"type":"text","text":"} { \" [ ] 混合"}]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 1);
        CHECK(out[0].parts.size() == 1);
        CHECK(out[0].parts[0].text == LR"(} { " [ ] 混合)");
    }
    // \uXXXX 转义（含中文 BMP + 代理对 🎉）
    {
        const wchar_t* j =
            LR"([{"info":{"role":"assistant"},"parts":[{"type":"text","text":"\u4f60\u597d \ud83c\udf89"}]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 1 && !out[0].parts.empty());
        CHECK(out[0].parts[0].text == L"你好 🎉");
    }
    // info 内嵌套对象（time:{created:..}）不破坏后续字段扫描（JSkipValue 回归）
    {
        const wchar_t* j =
            LR"([{"info":{"id":"m1","role":"user","time":{"created":1,"updated":2}},"parts":[{"type":"text","text":"q"}]},{"info":{"role":"assistant","finish":"stop","time":{"created":3}},"parts":[{"type":"text","text":"a"}]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 2);
        CHECK(out[0].role == L"user" && out[0].parts.size() == 1);
        CHECK(out[1].role == L"assistant");
        CHECK(out[1].finish == L"stop");   // 嵌套对象后的 finish 仍能读到
        CHECK(out[1].parts.size() == 1 && out[1].parts[0].text == L"a");
    }
    // 流式中间态：最后一条 assistant 无 finish
    {
        const wchar_t* j =
            LR"([{"info":{"role":"user"},"parts":[{"type":"text","text":"q"}]},{"info":{"role":"assistant"},"parts":[{"type":"text","text":"partial"}]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 2);
        CHECK(out[1].finish.empty());   // 流式进行中
    }
    // tool part：tool 名 + callID + state.status 嵌套对象解析
    {
        const wchar_t* j =
            LR"([{"info":{"role":"assistant"},"parts":[{"type":"step-start"},{"type":"reasoning","text":"think"},{"type":"tool","callID":"call_1","tool":"read","state":{"status":"running","input":{"filePath":"D:\\x.txt"},"time":{"start":1}}},{"type":"tool","tool":"grep","state":{"status":"completed"}},{"type":"text","text":"answer"}]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 1);
        CHECK(out[0].parts.size() == 5);
        const OcPart& t1 = out[0].parts[2];
        CHECK(t1.type == L"tool" && t1.tool == L"read");
        CHECK(t1.callId == L"call_1");
        CHECK(t1.toolState == L"running");
        const OcPart& t2 = out[0].parts[3];
        CHECK(t2.tool == L"grep" && t2.toolState == L"completed");
        // text part 仍正常
        CHECK(out[0].parts[4].type == L"text" &&
              out[0].parts[4].text == L"answer");
    }

    // ---- BuildPromptBody：转义回环 ----
    {
        std::string body = BuildPromptBody(L"行1\n行2 \"q\" \\end\\");
        CHECK(body.find("\\n") != std::string::npos);
        CHECK(body.find("\\\"q\\\"") != std::string::npos);
        CHECK(body.find("\\\\end\\\\") != std::string::npos);
        CHECK(body.substr(0, 28) == "{\"parts\":[{\"type\":\"text\",\"te");
    }
    // BuildPromptBody：带 model 字段
    {
        std::string body = BuildPromptBody(L"hi", L"opencode", L"big-pickle");
        CHECK(body.find("\"model\":{\"providerID\":\"opencode\",\"modelID\":\"big-pickle\"}")
              != std::string::npos);
        CHECK(body.find("\"parts\":") != std::string::npos);
    }
    // BuildPromptBody：非 ASCII 不截断（wchar_t += 到 string 的历史截断回归）
    {
        std::string body = BuildPromptBody(L"你好\"q\"", L"", L"");
        CHECK(body.find("你好") != std::string::npos);
        CHECK(body.find("\\\"q\\\"") != std::string::npos);
    }

    // ---- ParseProviderDefaults ----
    {
        std::vector<std::pair<std::wstring, std::wstring>> defs;
        CHECK(ParseProviderDefaults(
            LR"({"providers":[{"id":"a"},{"id":"b"}],"default":{"opencode":"big-pickle","bankofai":"qwen3.8-flash"}})",
            &defs));
        CHECK(defs.size() == 2);
        CHECK(defs[0].first == L"opencode" && defs[0].second == L"big-pickle");
        CHECK(defs[1].first == L"bankofai" && defs[1].second == L"qwen3.8-flash");
    }
    {   // 无 default 键
        std::vector<std::pair<std::wstring, std::wstring>> defs;
        CHECK(ParseProviderDefaults(LR"({"providers":[]})", &defs));
        CHECK(defs.empty());
    }

    // ---- ParseProviderList：真实 /config/providers 形态 ----
    {
        const wchar_t* j =
            LR"({"providers":[{"id":"p1","name":"P-One","models":{"m1":{"name":"Model One"},"m2":{}}},{"id":"p2","models":{"m3":{"name":"M Three"}}},{"name":"no-id","models":{"m4":{}}}],"default":{"p1":"m1"}})";
        std::vector<OcModelEntry> out;
        CHECK(ParseProviderList(j, &out));
        CHECK(out.size() == 3);   // 无 id 的 provider 跳过
        CHECK(out[0].provider == L"p1" && out[0].providerName == L"P-One");
        CHECK(out[0].modelId == L"m1" && out[0].modelName == L"Model One");
        CHECK(out[1].modelId == L"m2" && out[1].modelName.empty());  // 空 name 回退
        CHECK(out[2].provider == L"p2" && out[2].providerName.empty());
        CHECK(out[2].modelId == L"m3" && out[2].modelName == L"M Three");
    }
    {   // 非 providers 键 / 损坏输入
        std::vector<OcModelEntry> out;
        CHECK(ParseProviderList(LR"({"default":{}})", &out) && out.empty());
        CHECK(!ParseProviderList(L"broken", &out));
    }

    // ---- ParseSessionBusy：/session/status 形态 ----
    {
        CHECK(ParseSessionBusy(LR"({"ses_a":{"type":"busy"}})", L"ses_a"));
        CHECK(ParseSessionBusy(LR"({"ses_a":{"type":"idle"},"ses_b":{"type":"busy"}})",
                               L"ses_b"));
        CHECK(!ParseSessionBusy(L"{}", L"ses_a"));            // 空对象 = 空闲
        CHECK(!ParseSessionBusy(LR"({"ses_b":{"type":"busy"}})", L"ses_a"));
        CHECK(!ParseSessionBusy(L"broken", L"ses_a"));        // 损坏 → 空闲
        CHECK(!ParseSessionBusy(LR"({"ses_a":{"type":"busy"}})", L""));  // 空 id
    }

    // ---- ParsePendingPermissions：/permission 形态（会话过滤 + 缺字段容错）----
    {
        const wchar_t* j =
            LR"([{"id":"per_1","sessionID":"ses_a","permission":"external_directory","patterns":["x"]},"
               "{"id":"per_2","sessionID":"ses_b","permission":"bash"},"
               "{"permission":"no-id-no-sid"},"
               "{"id":"per_3","permission":"no-sid"}])";
        std::vector<std::pair<std::wstring, std::wstring>> out;
        CHECK(ParsePendingPermissions(j, L"ses_a", &out));
        CHECK(out.size() == 1);   // 只留本会话且 id 齐全的
        CHECK(out[0].first == L"per_1");
        CHECK(out[0].second == L"external_directory");
        CHECK(ParsePendingPermissions(L"[]", L"ses_a", &out) && out.empty());
        CHECK(!ParsePendingPermissions(L"broken", L"ses_a", &out));
    }

    // ---- ParseMessagesJson：info.error（ProviderAuthError，批次 18 实锤形态）----
    {
        const wchar_t* j =
            LR"([{"info":{"role":"assistant","finish":"error","error":{"name":"ProviderAuthError","data":{"providerID":"opencode","message":"Invalid API key"}},"tokens":{"input":0}},"parts":[{"type":"step-start"}]},
                  {"info":{"role":"assistant","finish":"error","error":{"name":"APIError","data":{"message":"gateway 500","statusCode":500,"isRetryable":true}}},"parts":[]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 2);
        CHECK(out[0].errText == L"ProviderAuthError: Invalid API key");
        CHECK(out[0].finish == L"error");
        CHECK(out[1].errText == L"APIError: gateway 500");
        // 正常消息 errText 为空
        CHECK(out[0].parts.size() == 1 && out[0].parts[0].type == L"step-start");
    }
    // error 无 data.message / error 为字符串等畸形形态不崩、不误报
    {
        const wchar_t* j =
            LR"([{"info":{"role":"assistant","error":{"name":"UnknownError"}},"parts":[]},
                  {"info":{"role":"assistant","error":"plain"},"parts":[]}])";
        std::vector<OcMessage> out;
        CHECK(ParseMessagesJson(j, &out));
        CHECK(out.size() == 2);
        CHECK(out[0].errText == L"UnknownError");
        CHECK(out[1].errText.empty());
    }

    // ---- ParseMessagesJson：tool part 的 state.input.filePath 提取 ----
    {
        const wchar_t* j =
            LR"([{"info":{"role":"assistant","finish":"stop"},"parts":[
                  {"type":"tool","tool":"edit","callID":"c1",
                   "state":{"status":"completed","input":{"filePath":"D:\\a\\f1.txt","oldString":"x"}}},
                  {"type":"tool","tool":"read","callID":"c2",
                   "state":{"status":"completed","input":{"filePath":"D:\\a\\f2.txt"}}},
                  {"type":"tool","tool":"write","callID":"c3",
                   "state":{"status":"running"}}]}])";
        std::vector<OcMessage> msgs;
        CHECK(ParseMessagesJson(j, &msgs));
        CHECK(msgs.size() == 1);
        const auto& ps = msgs[0].parts;
        CHECK(ps.size() == 3);
        CHECK(ps[0].filePath == LR"(D:\a\f1.txt)");   // edit 带路径
        CHECK(ps[1].filePath == LR"(D:\a\f2.txt)");   // read 也有 input.filePath
        CHECK(ps[2].filePath.empty());                // 无 input → 空
    }

    // ---- ParseMessagesJson：state.metadata.diff 提取（edit 完成态）----
    {
        const wchar_t* j =
            LR"([{"info":{"role":"assistant"},"parts":[
                  {"type":"tool","tool":"edit","callID":"e1",
                   "state":{"status":"completed",
                            "input":{"filePath":"D:\\x.cpp"},
                            "metadata":{"diagnostics":[],"diff":"Index: D:\\x.cpp\n@@ -1 +1 @@\n-old\n+new\n","truncated":false}}},
                  {"type":"tool","tool":"edit","callID":"e2",
                   "state":{"status":"running","metadata":{}}},
                  {"type":"tool","tool":"write","callID":"e3",
                   "state":{"status":"completed","metadata":{"filepath":"D:\\y.txt"}}}]}])";
        std::vector<OcMessage> msgs;
        CHECK(ParseMessagesJson(j, &msgs));
        const auto& ps = msgs[0].parts;
        CHECK(ps.size() == 3);
        CHECK(ps[0].diff.find(L"+new") != std::wstring::npos);
        CHECK(ps[0].diff.find(L"-old") != std::wstring::npos);
        CHECK(ps[1].diff.empty());   // metadata 空 → 无 diff
        CHECK(ps[2].diff.empty());   // write 的 metadata 无 diff 键
    }

    // ---- ParseSessionList：/session 形态（id/title/directory/time.updated）----
    {
        const wchar_t* j =
            LR"([{"id":"ses_1","slug":"a","directory":"D:\\ws","title":"first task","time":{"created":100,"updated":1234567890123},"
                 "{"id":"ses_2","title":"","time":{}},"
                 "{"title":"no id"},"notanobject","
                 "{"id":"ses_3","directory":"D:\\other","time":{"updated":5}}])";
        std::vector<OcSessionEntry> out;
        CHECK(ParseSessionList(j, &out));
        CHECK(out.size() == 3);                       // 无 id 条目丢弃
        CHECK(out[0].id == L"ses_1");
        CHECK(out[0].title == L"first task");
        CHECK(out[0].directory == LR"(D:\ws)");
        CHECK(out[0].updated == 1234567890123LL);
        CHECK(out[1].id == L"ses_2");
        CHECK(out[1].updated == 0);                   // time 空 → 0
        CHECK(out[2].updated == 5);
        CHECK(ParseSessionList(L"[]", &out) && out.empty());
        CHECK(!ParseSessionList(L"broken", &out));
    }

    // ---- ParseSessionList cost/tokens（批次 27）----
    {
        // 真实形态（探针记录：cost 浮点 + tokens 四键 + cache.read）
        const wchar_t* j =
            LR"([{"id":"ses_c","cost":0.7994560000000006,"tokens":{"input":144263372,"output":2497122,"reasoning":1594600,"cache":{"read":830063971,"write":0}}},"
               "{"id":"ses_plain","title":"no cost"},"
               "{"id":"ses_part","tokens":{"output":42}}])";
        std::vector<OcSessionEntry> out;
        CHECK(ParseSessionList(j, &out));
        CHECK(out.size() == 3);
        CHECK(out[0].cost > 0.7994 && out[0].cost < 0.7995);
        CHECK(out[0].tokIn == 144263372LL);
        CHECK(out[0].tokOut == 2497122LL);
        CHECK(out[0].tokReason == 1594600LL);
        CHECK(out[0].tokCacheRead == 830063971LL);
        CHECK(out[1].cost == 0 && out[1].tokOut == 0);   // 无字段 → 0
        CHECK(out[2].tokOut == 42 && out[2].tokIn == 0);  // 部分字段
    }

    // ---- ParseSessionDetail（批次 27 /session/:id）----
    {
        OcSessionEntry d;
        CHECK(ParseSessionDetail(
            LR"({"id":"ses_d","slug":"x","cost":12.5,"tokens":{"input":1000,"output":2000,"reasoning":500,"cache":{"read":7000}},"title":"t"})",
            &d));
        CHECK(d.id == L"ses_d");
        CHECK(d.cost == 12.5);
        CHECK(d.tokIn == 1000 && d.tokOut == 2000 &&
              d.tokReason == 500 && d.tokCacheRead == 7000);
        // 畸形：非对象 / 缺 id
        CHECK(!ParseSessionDetail(L"[1,2]", &d));
        CHECK(!ParseSessionDetail(LR"({"cost":1.0})", &d));
        CHECK(!ParseSessionDetail(L"broken", &d));
    }

    // ---- FormatTokens / FormatCost（批次 27 显示格式化）----
    {
        CHECK(FormatTokens(0) == L"0");
        CHECK(FormatTokens(-5) == L"0");
        CHECK(FormatTokens(999) == L"999");
        CHECK(FormatTokens(1000) == L"1k");
        CHECK(FormatTokens(1500) == L"1.5k");
        CHECK(FormatTokens(123456) == L"123k");
        CHECK(FormatTokens(2497122) == L"2.5M");
        CHECK(FormatTokens(123456789LL) == L"123M");
        CHECK(FormatCost(0) == L"");
        CHECK(FormatCost(-1) == L"");
        CHECK(FormatCost(0.005) == L"<$0.01");
        CHECK(FormatCost(0.799456) == L"$0.80");
        CHECK(FormatCost(12.5) == L"$12.50");
    }

    // ---- ParseEventFrame（批次 26 /event SSE data 帧）----
    {
        std::wstring type, sid;
        // 真机形态：message.part.updated（sessionID 在 properties 首位）
        CHECK(ParseEventFrame(
            LR"({"id":"evt_1","type":"message.part.updated","properties":{"sessionID":"ses_abc","part":{"type":"text","text":"hi"}}})",
            &type, &sid));
        CHECK(type == L"message.part.updated");
        CHECK(sid == L"ses_abc");
        // message.updated：sessionID 后有 info 内层同名键（不应串位）
        CHECK(ParseEventFrame(
            LR"({"id":"evt_2","type":"message.updated","properties":{"sessionID":"ses_x","info":{"id":"msg_1","sessionID":"ses_inner","role":"assistant"}}})",
            &type, &sid));
        CHECK(type == L"message.updated" && sid == L"ses_x");
        // server.* 事件无 sessionID
        CHECK(ParseEventFrame(
            LR"({"id":"evt_3","type":"server.connected","properties":{}})",
            &type, &sid));
        CHECK(type == L"server.connected" && sid.empty());
        // 畸形
        CHECK(!ParseEventFrame(L"garbage", &type, &sid));
        CHECK(!ParseEventFrame(LR"({"noType":1})", &type, &sid));
    }

    // ---- EventSubscribe 真网络链路（需本地 mock ocserver :14209）----
    {
        OcClient::SetPortOverride(14209);
        struct Ctx { int n = 0; std::wstring lastType, lastSid; };
        Ctx ctx;
        auto onEvt = [](void* user, const std::wstring& type,
                        const std::wstring& sid) {
            auto* c = (Ctx*)user;
            c->n++;
            c->lastType = type;
            c->lastSid = sid;
            if (c->n >= 3) {   // connected + 若干心跳即认为通
                // 置位取消由外层 atomic 指针完成——这里只计数
            }
        };
        std::atomic<bool> cancel{false};
        // 限时 3s：EventSubscribe 是阻塞长连接，到点取消返回
        auto t0 = GetTickCount();
        auto killer = std::thread([&cancel]() {
            Sleep(3000);
            cancel.store(true);
        });
        bool connected = OcClient::EventSubscribe(onEvt, &ctx, &cancel);
        killer.join();
        DWORD elapsed = GetTickCount() - t0;
        if (connected && ctx.n > 0) {
            std::printf("EventSubscribe live: %d events, last=%S sid=%S (%lums)\n",
                        ctx.n, ctx.lastType.c_str(), ctx.lastSid.c_str(),
                        elapsed);
        } else {
            std::printf("EventSubscribe live: mock server unavailable "
                        "(connected=%d events=%d)\n", connected ? 1 : 0,
                        ctx.n);
        }
        OcClient::SetPortOverride(0);
    }

    if (g_failed == 0) { std::printf("ai_json: ALL PASS\n"); return 0; }
    std::printf("ai_json: %d failed\n", g_failed);
    return 1;
}
// probe
