#include "Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>

namespace xfs {

std::wstring Utf8ToWide(const std::string& utf8, unsigned int codePage) {
    if (utf8.empty()) return {};
    int need = ::MultiByteToWideChar(codePage, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    ::MultiByteToWideChar(codePage, 0, utf8.data(), (int)utf8.size(), out.data(), need);
    return out;
}

std::string WideToUtf8(const std::wstring& wide, unsigned int codePage) {
    if (wide.empty()) return {};
    int need = ::WideCharToMultiByte(codePage, 0, wide.data(), (int)wide.size(),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    ::WideCharToMultiByte(codePage, 0, wide.data(), (int)wide.size(),
                          out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring NormalizePath(const std::wstring& path) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec) return path;
    return abs.wstring();
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = false;
    if (::GetFileSizeEx(h, &size)) {
        out.clear();
        out.resize((size_t)size.QuadPart);
        // Read in <=64MB chunks to keep peak memory low
        const DWORD kChunk = 64u * 1024u * 1024u;
        size_t total = 0;
        ok = true;
        while (total < out.size()) {
            DWORD toRead = (DWORD)((out.size() - total > kChunk) ? kChunk : (out.size() - total));
            DWORD got = 0;
            if (!::ReadFile(h, out.data() + total, toRead, &got, nullptr) || got != toRead) {
                ok = false;
                break;
            }
            total += got;
        }
    }
    ::CloseHandle(h);
    return ok;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
    // clear read-only attribute if set (common for ATE-generated pattern files)
    DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        // try to log the failure for diagnostics
        wchar_t msg[512];
        swprintf_s(msg, L"WriteFileBytes FAILED '%s' gle=%lu size=%zu\n",
                   path.c_str(), err, size);
        ::OutputDebugStringW(msg);
        return false;
    }
    bool ok = true;
    const char* p = (const char*)data;
    const DWORD kChunk = 64u * 1024u * 1024u;
    size_t total = 0;
    while (total < size) {
        DWORD toWrite = (DWORD)((size - total > kChunk) ? kChunk : (size - total));
        DWORD written = 0;
        if (!::WriteFile(h, p + total, toWrite, &written, nullptr) || written != toWrite) {
            ok = false;
            break;
        }
        total += written;
    }
    ::CloseHandle(h);
    return ok;
}

bool OpenInExplorer(const std::wstring& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::wstring cmd;
    if (std::filesystem::is_regular_file(path, ec)) {
        // reveal & select the file inside its folder
        cmd = L"/select,\"" + path + L"\"";
    } else {
        // open the directory itself
        cmd = L"\"" + path + L"\"";
    }
    HINSTANCE r = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", cmd.c_str(),
                                  nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

bool MappedFile::Open(const std::wstring& path) {
    Close();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz)) { ::CloseHandle(h); return false; }
    if (sz.QuadPart <= 0) { ::CloseHandle(h); size_ = 0; view_ = nullptr; return true; }

    // Map the whole file. MapViewOfFile with offset/size 0 maps to EOF; for
    // >2GB files Windows handles the high/low part split automatically.
    HANDLE map = ::CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map) { ::CloseHandle(h); return false; }
    const unsigned char* view = (const unsigned char*)
        ::MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!view) { ::CloseHandle(map); ::CloseHandle(h); return false; }

    fileH_ = (void*)h;
    mapping_ = (void*)map;
    view_ = view;
    size_ = (size_t)sz.QuadPart;
    return true;
}

void MappedFile::Close() {
    if (view_) { ::UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_) { ::CloseHandle((HANDLE)mapping_); mapping_ = nullptr; }
    if (fileH_) { ::CloseHandle((HANDLE)fileH_); fileH_ = nullptr; }
    size_ = 0;
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (!::OpenClipboard(owner)) return false;
    ::EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!g) { ::CloseClipboard(); return false; }
    void* p = ::GlobalLock(g);
    if (!p) { ::GlobalFree(g); ::CloseClipboard(); return false; }
    memcpy(p, text.c_str(), bytes);
    ::GlobalUnlock(g);
    const bool ok = ::SetClipboardData(CF_UNICODETEXT, g) != nullptr;
    ::CloseClipboard();
    return ok;
}

} // namespace xfs
