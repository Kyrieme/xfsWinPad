// Profile.cpp — 单文件配置 profile 实现（字节级保真内嵌）。
//
// .xfprofile 格式（UTF-8 JSON）：
// {
//   "xfsProfile": 1,            // 格式版本（整数）
//   "app": "xfsWinPad",
//   "files": {                  // 相对 configDir 的原样文本
//     "settings.json": "<原始字节经 JSON 转义>",
//     "shortcuts.json": "...",
//     "stylers.json": "...",
//     "session.json": "..."
//   },
//   "themes": {                 // themes\*.json，键 = 文件名
//     "mytheme.json": "..."
//   }
// }
//
// JSON 转义：全量以 \u00XX 逐字节转义非 ASCII 可打印字符之外的字节
// （简版 escaping，仅 \" \\ \b \f \n \r \t + \u00XX），读回时逆向。
// 不使用 JsonLite::Parse 的转义假设（它不处理 \u），本文件自带
// Escape/Unescape 一对，且 Unescape 后再做整体 UTF-8 校验由写回字节
// 的原始性保证（导出什么字节、导入就写回什么字节）。
#define WIN32_LEAN_AND_MEAN
#include "Profile.h"
#include "../core/Util.h"
#include "../core/Log.h"

#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>

namespace xfs {

namespace fs = std::filesystem;

// ---- 配置根目录 --------------------------------------------------------------
// 各模块（Settings/Theme/Session/ShortcutTable/Styler）各自拼接同一目录；
// 这里保持一致口径（FOLDERID_RoamingAppData\xfsWinPad）。
std::wstring ProfileConfigDir() {
    std::wstring base = L".";
    PWSTR appData = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                         &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    std::wstring dir = base + L"\\xfsWinPad";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

namespace {

// ---- JSON 字符串转义（字节级，ASCII 安全）------------------------------------
std::string JsonEscape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 16);
    for (unsigned char c : raw) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

bool JsonUnescape(const std::string& in, std::string* out) {
    out->clear();
    out->reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c != '\\') { *out += c; continue; }
        if (++i >= in.size()) return false;
        char e = in[i];
        switch (e) {
            case '"':  *out += '"';  break;
            case '\\': *out += '\\'; break;
            case '/':  *out += '/';  break;
            case 'b':  *out += '\b'; break;
            case 'f':  *out += '\f'; break;
            case 'n':  *out += '\n'; break;
            case 'r':  *out += '\r'; break;
            case 't':  *out += '\t'; break;
            case 'u': {
                if (i + 4 >= in.size()) return false;
                unsigned v = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = in[++i];
                    v <<= 4;
                    if (h >= '0' && h <= '9')      v |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                if (v > 0xFF) return false;   // 本格式只产生 \u00XX
                *out += static_cast<char>(v);
                break;
            }
            default: return false;
        }
    }
    return true;
}

// 读取 JSON 字符串字面量（in[*pos] 应指向开引号），推进 pos 越过闭引号。
// 注意：内部转义对（如 \"）必须原样保留进 raw 并整体跳过 —— 否则值里的
// 第一个 \" 会被误判为字符串结束（settings.json 全是引号，必炸）。
bool ReadJsonString(const std::string& text, size_t* pos, std::string* out) {
    if (*pos >= text.size() || text[*pos] != '"') return false;
    ++*pos;
    std::string raw;
    while (*pos < text.size()) {
        char c = text[*pos];
        if (c == '"') { ++*pos; return JsonUnescape(raw, out); }
        raw += c;
        ++*pos;
        if (c == '\\' && *pos < text.size()) {   // 转义对整体进 raw
            raw += text[*pos];
            ++*pos;
        }
    }
    return false;
}

void SkipWs(const std::string& text, size_t* pos) {
    while (*pos < text.size() &&
           (text[*pos] == ' ' || text[*pos] == '\t' ||
            text[*pos] == '\r' || text[*pos] == '\n')) ++*pos;
}

bool MatchKey(const std::string& text, size_t* pos, const char* key) {
    const size_t n = strlen(key);
    if (text.compare(*pos, n, key) != 0) return false;
    *pos += n;
    return true;
}

// ---- 文件清单 ----------------------------------------------------------------
const wchar_t* kFiles[] = {
    L"settings.json", L"shortcuts.json", L"stylers.json", L"session.json",
};

// ---- 备份 --------------------------------------------------------------------
std::wstring NowStamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t buf[40];
    swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool BackupFile(const fs::path& file, const fs::path& backupDir) {
    std::error_code ec;
    if (!fs::exists(file)) return true;   // 本来就没有，无需备份
    fs::create_directories(backupDir, ec);
    fs::copy_file(file, backupDir / file.filename(),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        Logger::Error("Profile: backup failed for " + WideToUtf8(file.wstring()) +
                      " gle=" + std::to_string(ec.value()));
        return false;
    }
    return true;
}

// 主题文件名安全检查：拒绝路径穿越/分隔符
bool SafeThemeName(const std::wstring& name) {
    if (name.empty() || name.size() > 128) return false;
    if (name.find(L"..") != std::wstring::npos) return false;
    if (name.find(L'\\') != std::wstring::npos ||
        name.find(L'/') != std::wstring::npos) return false;
    if (name.front() == L'.') return false;
    return true;
}

} // namespace

// ---- 导出 --------------------------------------------------------------------
bool ProfileExportTo(const std::wstring& configDir, const std::wstring& path,
                     std::wstring* err) {
    auto fail = [&](const char* why) {
        if (err) *err = Utf8ToWide(why);
        Logger::Error(std::string("Profile: export failed: ") + why);
        return false;
    };

    std::ostringstream out;
    out << "{\r\n  \"xfsProfile\": 1,\r\n  \"app\": \"xfsWinPad\",\r\n  \"files\": {";
    bool first = true;
    for (const wchar_t* rel : kFiles) {
        std::string raw;
        std::wstring full = configDir + L"\\" + rel;
        // 缺省配置自动跳过（从没改过 stylers/shortcuts 的机器上没有该文件）
        if (!ReadFileBytes(full, raw)) continue;
        out << (first ? "\r\n" : ",\r\n");
        first = false;
        out << "    \"" << WideToUtf8(rel) << "\": \"" << JsonEscape(raw) << "\"";
    }
    if (first) out << "}";   // files 空（几乎不可能，settings.json 总在）
    else       out << "\r\n  }";
    out << ",\r\n  \"themes\": {";

    // themes\*.json 逐个内嵌（文件名已由目录枚举保证，无需再校验）
    std::wstring themesDir = configDir + L"\\themes";
    first = true;
    if (fs::exists(fs::path(themesDir))) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(themesDir), ec)) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != L".json") continue;
            std::string raw;
            if (!ReadFileBytes(e.path().wstring(), raw)) continue;
            out << (first ? "\r\n" : ",\r\n");
            first = false;
            out << "    \"" << WideToUtf8(e.path().filename().wstring())
                << "\": \"" << JsonEscape(raw) << "\"";
        }
    }
    if (first) out << "}";
    else       out << "\r\n  }";
    out << "\r\n}\r\n";

    std::string text = out.str();
    if (!WriteFileBytes(path, text.data(), text.size()))
        return fail("cannot write destination file");
    if (err) err->clear();
    Logger::Info("Profile: exported " + std::to_string(text.size()) +
                 " bytes -> " + WideToUtf8(path));
    return true;
}

// ---- 导入 --------------------------------------------------------------------
bool ProfileImportTo(const std::wstring& configDir, const std::wstring& path,
                     std::wstring* err) {
    auto fail = [&](const std::wstring& why) {
        if (err) *err = why;
        Logger::Error("Profile: import failed: " + WideToUtf8(why));
        return false;
    };

    std::string text;
    if (!ReadFileBytes(path, text))
        return fail(L"cannot read profile file");

    // ---- 极简手写解析（字节级保真需要原文转义层，JsonLite 只到 wstring）----
    size_t pos = 0;
    auto ws = [&](size_t* p) { SkipWs(text, p); };

    ws(&pos);
    if (pos >= text.size() || text[pos] != '{')
        return fail(L"not a JSON object");
    ++pos;

    bool sawProfile = false;
    bool sawApp = false;
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> themes;
    bool any = false;

    ws(&pos);
    if (pos < text.size() && text[pos] == '}') ++pos;
    else {
        for (;;) {
            ws(&pos);
            std::string key;
            if (!ReadJsonString(text, &pos, &key))
                return fail(L"bad object key");
            ws(&pos);
            if (pos >= text.size() || text[pos] != ':')
                return fail(L"expected ':'");
            ++pos;
            ws(&pos);

            if (key == "xfsProfile") {
                // 版本号：目前只认 1
                if (pos >= text.size() || text[pos] < '0' || text[pos] > '9')
                    return fail(L"bad xfsProfile");
                sawProfile = true;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            } else if (key == "app") {
                std::string v;
                if (!ReadJsonString(text, &pos, &v)) return fail(L"bad app");
                sawApp = (v == "xfsWinPad");
            } else if (key == "files" || key == "themes") {
                auto& dst = (key == "files") ? files : themes;
                ws(&pos);
                if (pos >= text.size() || text[pos] != '{')
                    return fail(L"bad section object");
                ++pos;
                ws(&pos);
                if (pos < text.size() && text[pos] == '}') { ++pos; }
                else {
                    for (;;) {
                        std::string fname, val;
                        ws(&pos);   // 逗号/换行后需跳空白才能读下一个 key
                        if (!ReadJsonString(text, &pos, &fname))
                            return fail(L"bad file key @" + std::to_wstring(pos));
                        ws(&pos);
                        if (pos >= text.size() || text[pos] != ':')
                            return fail(L"expected ':'");
                        ++pos;
                        ws(&pos);
                        if (!ReadJsonString(text, &pos, &val))
                            return fail(L"bad file value @" + std::to_wstring(pos) +
                                        L" ctx: " + Utf8ToWide(text.substr(
                                            pos > 20 ? pos - 20 : 0, 40)));
                        dst.emplace(fname, std::move(val));
                        any = true;
                        ws(&pos);
                        if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                        if (pos < text.size() && text[pos] == '}') { ++pos; break; }
                        return fail(L"bad section syntax");
                    }
                }
            } else {
                return fail(L"unknown section: " + Utf8ToWide(key));
            }

            ws(&pos);
            if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
            if (pos < text.size() && text[pos] == '}') { ++pos; break; }
            return fail(L"bad object syntax");
        }
    }

    if (!sawProfile || !sawApp || !any)
        return fail(L"missing xfsProfile/app/files sections");

    // ---- 备份 + 写回 -----------------------------------------------------------
    fs::path backupDir = fs::path(configDir) /
        (L"profile-backup-" + NowStamp());
    auto backupAndWrite = [&](const std::wstring& full, const std::string& data)
        -> bool {
        if (!BackupFile(fs::path(full), backupDir)) return false;
        return WriteFileBytes(full, data.data(), data.size());
    };

    int written = 0;
    for (const auto& [fname, val] : files) {
        std::wstring wname = Utf8ToWide(fname);
        // 只接受白名单文件名（防未知文件写到任意位置）
        bool ok = false;
        for (const wchar_t* rel : kFiles)
            if (wname == rel) { ok = true; break; }
        if (!ok) continue;
        if (!backupAndWrite(configDir + L"\\" + wname, val)) {
            // 备份失败不中断整体导入（已写入的保留），只记日志
            Logger::Error("Profile: backup failed, skip " + fname);
            continue;
        }
        ++written;
    }
    for (const auto& [fname, val] : themes) {
        std::wstring wname = Utf8ToWide(fname);
        if (!SafeThemeName(wname)) continue;
        if (!backupAndWrite(configDir + L"\\themes\\" + wname, val)) continue;
        ++written;
    }
    if (written == 0)
        return fail(L"no known config file restored");

    if (err) err->clear();
    Logger::Info("Profile: imported " + std::to_string(written) +
                 " file(s) from " + WideToUtf8(path) +
                 " (backup: " + WideToUtf8(backupDir.wstring()) + ")");
    return true;
}

} // namespace xfs
