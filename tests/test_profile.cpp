// test_profile.cpp — 单文件配置 profile（.xfprofile）导出/导入单测。
//
// 覆盖：字节级保真回环（含中文 UTF-8 内容）、缺文件跳过、坏 profile 拒绝、
// 主题文件名路径穿越防护、导入前备份目录生成。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "../src/settings/Profile.h"
#include "../src/core/Util.h"

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace fs = std::filesystem;

static fs::path MakeTempDir(const char* tag) {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    fs::path dir = fs::path(base) / (L"xfs_profile_test_" +
        std::wstring(tag, tag + strlen(tag)) + L"_" +
        std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static void WriteBytes(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
}

static std::string ReadAll(const fs::path& p) {
    std::string out;
    ReadFileBytes(p.wstring(), out);
    return out;
}

int main() {
    printf("== test_profile ==\n");

    // ---- 回环：导出 → 篡改 → 导入 → 字节一致 --------------------------------
    fs::path cfg = MakeTempDir("cfg");
    fs::create_directories(cfg / L"themes");
    // settings.json 含中文 + 引号 + 反斜杠 + 控制字符（覆盖转义矩阵）
    const std::string kSettings =
        "{\r\n  \"fontName\": \"Consolas\",\r\n"
        "  \"theme\": \"\\u81ea\\u5b9a\\u4e49-dark\",  // not json but raw\r\n"
        "  \"note\": \"quote\\\" back\\\\slash newline\\n tab\\t 中文原样字节\",\r\n"
        "  \"num\": 42\r\n}\r\n";
    WriteBytes(cfg / L"settings.json", kSettings);
    WriteBytes(cfg / L"shortcuts.json", "{\"100\":\"Ctrl+N\"}\n");
    WriteBytes(cfg / L"themes" / L"mytheme.json",
               "{\r\n  \"name\": \"mytheme\",\r\n  \"bg\": \"#1E1E1E\",\r\n"
               "  \"label\": \"\xe4\xb8\xad\xe6\x96\x87 bg\"\r\n}\r\n");

    fs::path profile = MakeTempDir("out") / L"p.xfprofile";
    std::wstring err;
    CHECK(ProfileExportTo(cfg.wstring(), profile.wstring(), &err));
    CHECK(fs::exists(profile));
    if (getenv("XFS_PROFILE_DUMP"))
        WriteBytes(fs::path(getenv("XFS_PROFILE_DUMP")) / L"dump.xfprofile",
                   ReadAll(profile));

    // 生成物是合法 JSON 且带版本标记
    std::string text = ReadAll(profile);
    CHECK(text.find("\"xfsProfile\": 1") != std::string::npos);
    CHECK(text.find("\"app\": \"xfsWinPad\"") != std::string::npos);
    // 中文以 UTF-8 原样字节存在（非 \\u 转义）
    CHECK(text.find("\xe4\xb8\xad\xe6\x96\x87") != std::string::npos);

    // 篡改现场：改 settings、删主题
    WriteBytes(cfg / L"settings.json", "{\"gone\": true}\r\n");
    fs::remove(cfg / L"themes" / L"mytheme.json");
    WriteBytes(cfg / L"themes" / L"other.json", "{}\r\n");

    CHECK(ProfileImportTo(cfg.wstring(), profile.wstring(), &err));
    CHECK(ReadAll(cfg / L"settings.json") == kSettings);   // 字节级还原
    CHECK(ReadAll(cfg / L"shortcuts.json") == "{\"100\":\"Ctrl+N\"}\n");
    CHECK(fs::exists(cfg / L"themes" / L"mytheme.json"));
    CHECK(ReadAll(cfg / L"themes" / L"mytheme.json").find("\xe4\xb8\xad\xe6\x96\x87")
          != std::string::npos);
    CHECK(fs::exists(cfg / L"themes" / L"other.json"));    // 导入不清除既有主题

    // 备份目录存在，且里面有导入前的 settings.json
    bool sawBackup = false;
    for (const auto& e : fs::directory_iterator(cfg)) {
        if (e.is_directory() &&
            e.path().filename().wstring().rfind(L"profile-backup-", 0) == 0) {
            sawBackup = fs::exists(e.path() / L"settings.json");
        }
    }
    CHECK(sawBackup);

    // ---- 拒绝：坏 profile -----------------------------------------------------
    fs::path bad = MakeTempDir("bad") / L"bad.xfprofile";
    WriteBytes(bad, "{\"random\": 1}\r\n");
    CHECK(!ProfileImportTo(cfg.wstring(), bad.wstring(), &err));
    CHECK(!err.empty());
    WriteBytes(bad, "not json at all");
    CHECK(!ProfileImportTo(cfg.wstring(), bad.wstring(), &err));

    // ---- 拒绝：主题文件名路径穿越 ---------------------------------------------
    fs::path evil = MakeTempDir("evil") / L"evil.xfprofile";
    WriteBytes(evil,
        "{\r\n  \"xfsProfile\": 1,\r\n"
        "  \"app\": \"xfsWinPad\",\r\n"
        "  \"files\": {\r\n"
        "    \"settings.json\": \"{\\\"ok\\\":true}\"\r\n"
        "  },\r\n"
        "  \"themes\": {\r\n"
        "    \"..\\\\evil_payload.json\": \"{\\\"evil\\\":true}\",\r\n"
        "    \"a/b.json\": \"{}\",\r\n"
        "    \".hidden.json\": \"{}\"\r\n"
        "  }\r\n"
        "}\r\n");
    fs::path cfg2 = MakeTempDir("cfg2");
    WriteBytes(cfg2 / L"settings.json", "{}\r\n");
    std::string payloadOutside = ReadAll(cfg2.parent_path() / L"evil_payload.json");
    CHECK(ProfileImportTo(cfg2.wstring(), evil.wstring(), &err));
    // settings 还原成功
    CHECK(ReadAll(cfg2 / L"settings.json").find("\"ok\":true") != std::string::npos);
    // 穿越名全部被拒绝：cfg2\themes 下没有任何文件，上级目录也没有 payload
    CHECK(!fs::exists(cfg2 / L"themes" / L"..\\evil_payload.json"));
    CHECK(!fs::exists(cfg2 / L"themes" / L"a"));
    CHECK(!fs::exists(cfg2 / L"themes" / L".hidden.json"));

    // ---- 缺文件跳过：空目录也能导出（空 files/themes 也算合法 profile）--------
    fs::path cfg3 = MakeTempDir("cfg3");
    fs::path p3 = MakeTempDir("out3") / L"p3.xfprofile";
    CHECK(ProfileExportTo(cfg3.wstring(), p3.wstring(), &err));
    CHECK(fs::exists(p3));

    // 清理
    fs::remove_all(cfg); fs::remove_all(profile.parent_path());
    fs::remove_all(bad.parent_path()); fs::remove_all(evil.parent_path());
    fs::remove_all(cfg2); fs::remove_all(cfg3); fs::remove_all(p3.parent_path());

    printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
