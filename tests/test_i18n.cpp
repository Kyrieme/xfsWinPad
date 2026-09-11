// test_i18n.cpp - I18n 字典加载/回退/格式化/切换回调单测
// 通过 SetLangDir 指向 fixture 目录，不依赖 exe 旁真实 lang\。
#include "../src/core/I18n.h"
#include "../src/core/Log.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace xfs;

namespace {

int g_failed = 0;

void Check(bool cond, const char* what) {
    if (!cond) {
        ++g_failed;
        Logger::Error(std::string("FAIL: ") + what);
    }
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

    Logger::Info("test_i18n done, failures=" + std::to_string(g_failed));
    return g_failed == 0 ? 0 : 1;
}
