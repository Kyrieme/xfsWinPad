// xfsWinPad - test_oai_json: OaiClient 纯 JSON 助手单测
//（chat body 构造 + 转义回环、choices 嵌套解析、models 列表、错误提取）
#include "../src/ai/OaiClient.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
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
    // ---- BuildChatBody：模型名/消息数组/stream:false ----
    {
        std::vector<OaiMsg> ms;
        ms.push_back({L"system", L"You are helpful."});
        ms.push_back({L"user", L"你好 \"引号\" <世界>\n第二行"});
        std::string body = OaiClient::BuildChatBody(L"qwen2.5-coder:7b", ms);
        CHECK(body.find("\"model\":\"qwen2.5-coder:7b\"") != std::string::npos);
        CHECK(body.find("\"stream\":false}") != std::string::npos);
        CHECK(body.find("\"role\":\"system\"") != std::string::npos);
        CHECK(body.find("\"role\":\"user\"") != std::string::npos);
        // UTF-8 转义回环：原文里的引号/换行必须转义，中文应转为 UTF-8 字节
        //（源文件是 UTF-8， Narrow 后中文为多字节序列，这里验证它不在宽字符形态）
        CHECK(body.find("\xe7\xac\xac") != std::string::npos);  // UTF-8 第
        CHECK(body.find("\\n") != std::string::npos);
        CHECK(body.find("\\\"") != std::string::npos);
        // 反斜杠转义
        std::vector<OaiMsg> bs;
        bs.push_back({L"user", L"a\\b"});
        std::string b2 = OaiClient::BuildChatBody(L"m", bs);
        CHECK(b2.find("a\\\\b") != std::string::npos);
    }

    // ---- ParseChatReply：标准 OpenAI 形态 ----
    {
        const wchar_t* j =
            LR"({"id":"cmpl-1","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,"message":{"role":"assistant","content":"你好，答案是 42。"},"finish_reason":"stop"}],"usage":{"total_tokens":10}})";
        CHECK(OaiClient::ParseChatReply(j) == L"你好，答案是 42。");
    }
    // 转义还原
    {
        const wchar_t* j =
            LR"({"choices":[{"message":{"content":"line1\nline2 \"q\" a\\b"}}]})";
        CHECK(OaiClient::ParseChatReply(j) == L"line1\nline2 \"q\" a\\b");
    }
    // 畸形输入 → 空
    CHECK(OaiClient::ParseChatReply(L"not json").empty());
    CHECK(OaiClient::ParseChatReply(L"{}").empty());
    CHECK(OaiClient::ParseChatReply(LR"({"choices":[]})").empty());
    // 多 choices 只取首个
    {
        const wchar_t* j =
            LR"({"choices":[{"message":{"content":"first"}},{"message":{"content":"second"}}]})";
        CHECK(OaiClient::ParseChatReply(j) == L"first");
    }

    // ---- ParseModelsList ----
    {
        const wchar_t* j =
            LR"({"object":"list","data":[{"id":"qwen2.5-coder:7b","object":"model"},{"id":"llama3.1:8b"},{"id":""},{"object":"model"}]})";
        std::vector<std::wstring> out;
        CHECK(OaiClient::ParseModelsList(j, &out));
        CHECK(out.size() == 2);
        CHECK(out[0] == L"qwen2.5-coder:7b");
        CHECK(out[1] == L"llama3.1:8b");
    }
    {
        std::vector<std::wstring> out;
        CHECK(!OaiClient::ParseModelsList(L"not json", &out));
        CHECK(OaiClient::ParseModelsList(LR"({"data":[]})", &out));
        CHECK(out.empty());
        CHECK(!OaiClient::ParseModelsList(LR"({"data":{}})", &out));
    }

    // ---- ParseErrorMessage ----
    CHECK(OaiClient::ParseErrorMessage(
              LR"({"error":{"message":"model not found","type":"invalid"}})") ==
          L"model not found");
    CHECK(OaiClient::ParseErrorMessage(LR"({"error":"plain string"})").empty());
    CHECK(OaiClient::ParseErrorMessage(L"{}").empty());

    // ---- BuildChatBody stream 参数 ----
    {
        std::vector<OaiMsg> ms;
        ms.push_back({L"user", L"hi"});
        std::string sb = OaiClient::BuildChatBody(L"m", ms, /*stream=*/true);
        CHECK(sb.find("\"stream\":true}") != std::string::npos);
    }

    // ---- ParseStreamDelta：chat.completion.chunk ----
    // 标准 delta 帧
    CHECK(OaiClient::ParseStreamDelta(
              LR"({"id":"c1","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"片段A"},"finish_reason":null}]})") ==
          L"片段A");
    // delta 里带换行/引号转义还原
    CHECK(OaiClient::ParseStreamDelta(
              LR"({"choices":[{"delta":{"content":"line1\nline2 \"q\""}}]})") ==
          L"line1\nline2 \"q\"");
    // role-only 帧（无 content）→ 空
    CHECK(OaiClient::ParseStreamDelta(
              LR"({"choices":[{"delta":{"role":"assistant","content":""}}]})")
              .empty());
    // content:null（末帧）→ 空
    CHECK(OaiClient::ParseStreamDelta(
              LR"({"choices":[{"delta":{},"finish_reason":"stop"}]})").empty());
    // delta 外还有兄弟字段（usage 块）不影响
    CHECK(OaiClient::ParseStreamDelta(
              LR"({"choices":[{"delta":{"content":"ok"}}],"usage":{"total_tokens":1}})") ==
          L"ok");
    // 畸形输入 → 空
    CHECK(OaiClient::ParseStreamDelta(L"not json").empty());
    CHECK(OaiClient::ParseStreamDelta(L"{}").empty());
    CHECK(OaiClient::ParseStreamDelta(LR"({"choices":[]})").empty());
    // delta 对象嵌套对象字段（如 function_call）不串位
    CHECK(OaiClient::ParseStreamDelta(
              LR"({"choices":[{"delta":{"tool_calls":[{"function":{"name":"f"}}],"content":"after"}}]})") ==
          L"after");

    // ---- 会话历史落盘：HistoryToJson / ParseHistory 回环（批次 25）----
    {
        std::vector<OaiMsg> src{
            {L"system", L"You are helpful."},
            {L"user", L"你好 \"引号\" <世界>\n第二行"},
            {L"assistant", L"回答：OK\\done"},
            {L"user", L"emoji \U0001F600 tail"}};
        std::string j = OaiClient::HistoryToJson(src);
        CHECK(j.find("\"version\":1") != std::string::npos);
        CHECK(j.find("\"role\":\"user\"") != std::string::npos);
        std::vector<OaiMsg> out;
        CHECK(OaiClient::ParseHistory(j, &out));
        CHECK(out.size() == src.size());
        bool same = out.size() == src.size();
        for (size_t i = 0; same && i < src.size(); ++i)
            same = out[i].role == src[i].role &&
                   out[i].content == src[i].content;
        CHECK(same);
        // 畸形输入 → false + 清空
        std::vector<OaiMsg> bad{{L"user", L"x"}};
        CHECK(!OaiClient::ParseHistory("garbage", &bad));
        CHECK(bad.empty());
        CHECK(!OaiClient::ParseHistory("{\"nokey\":[]}", &bad));
        CHECK(!OaiClient::ParseHistory("{\"history\":\"str\"}", &bad));
        CHECK(!OaiClient::ParseHistory(
            "{\"history\":[{\"role\":\"user\"}]}", &bad));
        // 空 history 数组 = 合法
        std::vector<OaiMsg> empty;
        CHECK(OaiClient::ParseHistory("{\"history\":[]}", &empty));
        CHECK(empty.empty());
    }

    // ---- 复刻 ChatStream 的 SSE 行切分（完全相同代码路径，静态喂入）----
    {
        // 9 帧与 mock5 相同结构：content 帧 + finish 帧 + [DONE]
        std::string sse;
        const char* words[] = {"Hello", " from", " SSE", " stream!"};
        for (const char* w : words) {
            sse += "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\","
                   "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"";
            sse += w;
            sse += "\"},\"finish_reason\":null}]}\n\n";
        }
        sse += "data: [DONE]\n\n";
        std::string pending, lineBytes;
        std::wstring full;
        bool seenDone = false;
        size_t nl;
        pending = sse;
        while ((nl = pending.find('\n')) != std::string::npos) {
            lineBytes.append(pending, 0, nl);
            pending.erase(0, nl + 1);
            if (!lineBytes.empty() && lineBytes.back() == '\r')
                lineBytes.pop_back();
            std::wstring line = xfs::Utf8ToWide(lineBytes);
            lineBytes.clear();
            if (line.compare(0, 5, L"data:") == 0) {
                std::wstring payload = line.substr(6);
                while (!payload.empty() &&
                       (payload.front() == L' ' || payload.front() == L'\t'))
                    payload.erase(payload.begin());
                if (payload == L"[DONE]") { seenDone = true; break; }
                std::wstring delta = OaiClient::ParseStreamDelta(payload);
                full += delta;
            }
        }
        CHECK(seenDone);
        CHECK(full == L"Hello from SSE stream!");
        if (g_failed) std::printf("replicated split: full=%.40S\n", full.c_str());
        else std::printf("replicated split OK: %.40S\n", full.c_str());
    }

    // ---- ChatStream 真网络链路（需本地 mock SSE :41139；无服务则跳过）----
    {
        std::vector<OaiMsg> msgs{{L"user", L"hi"}};
        std::wstring acc;
        int deltas = 0;
        auto onDelta = [](void* user, const std::wstring& d) {
            (*static_cast<std::pair<std::wstring, int>*>(user)).first += d;
            ++(*static_cast<std::pair<std::wstring, int>*>(user)).second;
        };
        auto onDone = [](void* user, const std::wstring& full) {
            static_cast<std::pair<std::wstring, int>*>(user)->first = full;
        };
        std::pair<std::wstring, int> st{L"", 0};
        std::wstring liveErr;
        bool ok = OaiClient::ChatStream(
            L"http://127.0.0.1:41139/v1", L"", L"mock-stream-7b", msgs,
            onDelta, &st, onDone, &liveErr, 15000);
        if (ok) {
            std::printf("ChatStream live: %d deltas, %zu chars, full head=%.40S\n",
                        st.second, st.first.size(), st.first.c_str());
        } else {
            std::printf("ChatStream live: mock server unavailable (err=%.60S)\n",
                        liveErr.c_str());
        }
    }

    if (g_failed) {
        std::printf("%d checks FAILED\n", g_failed);
        return 1;
    }
    std::printf("test_oai_json: all OK\n");
    return 0;
}
