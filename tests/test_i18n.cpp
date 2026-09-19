// test_i18n.cpp - I18n 字典加载/回退/格式化/切换回调单测
// 通过 SetLangDir 指向 fixture 目录，不依赖 exe 旁真实 lang\。
//
// 另有一节（第 7 节）反过来读**仓库里真实的** resources/lang/*.json，
// 校验五份语言文件的键集合完全一致 —— 见文件末尾的说明。
#include "../src/core/I18n.h"
#include "../src/core/JsonLite.h"
#include "../src/core/Log.h"
#include "../src/core/Util.h"
#include "lang_test_dir.h"   // CMake configure_file 生成：XFS_LANG_DIR

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xfs;

namespace {

int g_failed = 0;

void Check(bool cond, const char* what) {
    if (!cond) {
        ++g_failed;
        Logger::Error(std::string("FAIL: ") + what);
    }
}

// 宽字符版：键名列表只有宽字符才装得下。
void CheckW(bool cond, const std::wstring& what) {
    if (!cond) {
        ++g_failed;
        Logger::Error(WideToUtf8(what));
    }
}

// 把差异键名拼成一行可读消息（最多列 12 个，其余只报个数 —— 否则一个
// 语言整体没翻译时会把日志刷爆，反而看不见重点）。
std::wstring ListKeys(const std::vector<std::wstring>& keys) {
    std::wstring s;
    const size_t show = keys.size() < 12 ? keys.size() : 12;
    for (size_t i = 0; i < show; ++i) {
        if (i) s += L", ";
        s += keys[i];
    }
    if (keys.size() > show)
        s += L", ... (+" + std::to_wstring(keys.size() - show) + L" more)";
    return s;
}

// 提取 "{N}" 占位符下标（去重、升序）。没有任何占位符时返回空。
//
// 只认"{数字}"这一种形态 —— I18n::Fmt 与直接 swprintf 的调用点都只依赖它。
// 上限 100 是为了把"{2026}"这种正文里的花括号排除掉。
std::vector<int> Placeholders(const std::wstring& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'{') continue;
        size_t j = i + 1;
        if (j >= s.size() || s[j] < L'0' || s[j] > L'9') continue;
        int v = 0;
        while (j < s.size() && s[j] >= L'0' && s[j] <= L'9') {
            v = v * 10 + (s[j] - L'0');
            ++j;
        }
        if (j < s.size() && s[j] == L'}' && v < 100) out.push_back(v);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::wstring ShowIdx(const std::vector<int>& v) {
    std::wstring s = L"{";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += L",";
        s += std::to_wstring(v[i]);
    }
    return s + L"}";
}

// 写一个最小字典文件（UTF-8 无 BOM）
void WriteDict(const std::wstring& dir, const std::wstring& code,
               const std::string& content) {
    std::filesystem::create_directories(dir + L"lang");
    std::ofstream f(dir + L"lang\\" + code + L".json", std::ios::binary);
    f.write(content.data(), (std::streamsize)content.size());
}

} // namespace

int main() {
    Logger::Init();
    const std::wstring dir =
        std::filesystem::temp_directory_path().wstring() + L"xfs_i18n_test\\";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    WriteDict(dir, L"xx-XX",
              "{\n"
              "  \"greet\": \"Hej {0}\",\n"
              "  \"only.here\": \"local\"\n"
              "}\n");
    WriteDict(dir, L"en",
              "{\n"
              "  \"greet\": \"Hello {0}\",\n"
              "  \"en.only\": \"english\"\n"
              "}\n");
    WriteDict(dir, L"zh-CN",
              "{\n"
              "  \"greet\": \"Ni Hao {0}\"\n"
              "}\n");

    auto& i18n = I18n::Instance();
    i18n.SetLangDir(dir);

    // 1) 加载失败时保留旧字典并返回 false
    i18n.SetLangDir(dir + L"nonexistent\\");
    Check(!i18n.Load(L"xx-XX"), "load failure must return false");

    // 2) 正常加载 + 主动语言解析（Tr 返回原文，不做占位符替换）
    i18n.SetLangDir(dir);
    Check(i18n.Load(L"xx-XX"), "load xx-XX");
    Check(i18n.Code() == L"xx-XX", "code recorded");
    Check(std::wstring(i18n.Tr(L"greet")) == L"Hej {0}", "active dict wins");
    Check(std::wstring(i18n.Tr(L"en.only")) == L"english", "fallback to en");
    Check(std::wstring(i18n.Tr(L"missing.key")) == L"missing.key",
          "unknown id returns id");

    // 3) Fmt 占位符（含越界索引 = 空串，不崩溃）
    Check(i18n.Fmt(L"greet", {L"World"}) == L"Hej World", "fmt {0}");
    Check(i18n.Fmt(L"missing.key", {L"a"}) == L"missing.key", "fmt unknown id");
    Check(i18n.Fmt(L"greet", {}) == L"Hej ", "fmt no args");

    // 4) 切换到 zh-CN：en 兜底仍然生效
    Check(i18n.Load(L"zh-CN"), "load zh-CN");
    Check(std::wstring(i18n.Tr(L"greet")) == L"Ni Hao {0}",
          "zh-CN active");
    Check(std::wstring(i18n.Tr(L"en.only")) == L"english", "en fallback lives");

    // 5) 切换回调触发
    int fired = 0;
    i18n.AddCallback([&](const std::wstring& code) {
        if (code == L"en") ++fired;
    });
    i18n.Load(L"en");
    Check(fired == 1, "callback fired on switch");

    // 6) en → xx-XX → en 的往返切换（回退链固定 en→zh-CN，
    //    自定义语言仅在其为活动语言时参与解析）
    Check(i18n.Load(L"xx-XX") && i18n.Load(L"en"), "round trip reload");
    Check(std::wstring(i18n.Tr(L"greet")) == L"Hello {0}", "en dict after trip");

    // 7) 真实语言文件的键完整性（五份文件的键集合必须完全一致）
    //
    // 背景：批次 87 加了诊断面板，键只补进了 en / zh-CN —— ja / ko / zh-TW
    // 各缺 18 个键，日/韩/繁用户看到的诊断面板整块回退成英文，而且**没有
    // 任何测试会红**，靠人眼比对根本抓不住。所以在这里钉死。
    //
    // 两个注意点：
    //   * 路径来自 CMake 生成的头文件，不依赖 CWD（ctest 的 CWD 不一定是仓库根）。
    //   * 读不到文件必须**判失败**，不能当成"跳过" —— 否则守卫会静默退化成
    //     空操作，而"守卫生效"这件事本身也就没人验证了。
    {
        const std::wstring dir = XFS_LANG_DIR L"\\";
        auto parseFile = [&dir](const std::wstring& code, json::Value* out) {
            std::string raw;
            if (!ReadFileBytes(dir + code + L".json", raw)) return false;
            std::wstring t = Utf8ToWide(raw);
            if (!t.empty() && t[0] == 0xFEFF) t.erase(0, 1);
            return json::Parse(t, out) && out->kind == json::Value::Obj;
        };

        json::Value en;
        const bool enOk = parseFile(L"en", &en);
        CheckW(enOk, L"cannot parse " + dir + L"en.json as a JSON object");
        if (enOk) {
            // 7a) en 自己的占位符必须从 {0} **连续**。
            //     Fmt 是按位置取 initializer_list 的，写成 {0} 与 {2} 并存时
            //     {2} 对应的那个参数要靠调用点额外塞一个占位值才填得上 ——
            //     现实里没人这么写，所以一旦出现基本就是笔误。
            //     （译文与 en 一致这一条由下面的 7b 覆盖，所以这里只查 en。）
            std::vector<std::wstring> gap;
            for (const auto& [k, v] : en.obj) {
                const std::vector<int> idx = Placeholders(v.str);
                for (size_t i = 0; i < idx.size(); ++i)
                    if (idx[i] != (int)i) {
                        gap.push_back(k + L" " + ShowIdx(idx));
                        break;
                    }
            }
            if (!gap.empty())
                CheckW(false, L"en.json has " + std::to_wstring(gap.size()) +
                                 L" key(s) whose {N} placeholders are not 0..n-1: " +
                                 ListKeys(gap));

            int compared = 0;
            for (const wchar_t* code : {L"zh-CN", L"ja", L"ko", L"zh-TW"}) {
                json::Value other;
                if (!parseFile(code, &other)) {
                    CheckW(false, std::wstring(L"cannot parse ") + dir + code +
                                     L".json as a JSON object");
                    continue;
                }
                ++compared;
                std::vector<std::wstring> missing, extra;
                for (const auto& [k, v] : en.obj)
                    if (other.obj.find(k) == other.obj.end()) missing.push_back(k);
                for (const auto& [k, v] : other.obj)
                    if (en.obj.find(k) == en.obj.end()) extra.push_back(k);

                if (!missing.empty())
                    CheckW(false, std::wstring(code) + L".json is missing " +
                                     std::to_wstring(missing.size()) +
                                     L" key(s) present in en.json: " + ListKeys(missing));
                if (!extra.empty())
                    CheckW(false, std::wstring(code) + L".json has " +
                                     std::to_wstring(extra.size()) +
                                     L" key(s) absent from en.json: " + ListKeys(extra));

                // 7b) 占位符必须与 en 完全一致。
                //     译文多一个 {1} ⇒ 渲染成空串（Fmt 对越界索引给空）；少一个 ⇒
                //     参数被丢掉。两种情况都**不报错**，只是界面上少字或空一段。
                std::vector<std::wstring> badPh;
                for (const auto& [k, v] : en.obj) {
                    auto it = other.obj.find(k);
                    if (it == other.obj.end()) continue;   // 缺键已在上面报过
                    const std::vector<int> a = Placeholders(v.str);
                    const std::vector<int> b = Placeholders(it->second.str);
                    if (a != b)
                        badPh.push_back(k + L" en=" + ShowIdx(a) + L" " + code + L"=" +
                                        ShowIdx(b));
                }
                if (!badPh.empty())
                    CheckW(false, std::wstring(code) + L".json has " +
                                     std::to_wstring(badPh.size()) +
                                     L" key(s) whose {N} placeholders differ from en.json: " +
                                     ListKeys(badPh));
            }
            // 正控：确认这个守卫**真的**逐份比对过 4 个语言文件。
            // 没有它，将来若有人把路径宏改坏，上面的循环会一次都不跑，
            // 测试照样全绿 —— 那就成了"描述现象"而不是"验证机制"。
            CheckW(compared == 4,
                   L"language parity guard compared " + std::to_wstring(compared) +
                       L" of 4 non-English language file(s)");
        }
    }

    Logger::Info("test_i18n done, failures=" + std::to_string(g_failed));
    return g_failed == 0 ? 0 : 1;
}
