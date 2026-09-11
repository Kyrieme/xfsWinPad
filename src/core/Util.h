#pragma once
// xfsWinPad - UTF-8 / UTF-16 / ANSI conversion helpers

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdint>

namespace xfs {

std::wstring Utf8ToWide(const std::string& utf8, unsigned int codePage = 65001);
std::string WideToUtf8(const std::wstring& wide, unsigned int codePage = 65001);

// Path formatting helpers
std::wstring NormalizePath(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileBytes(const std::wstring& path, const void* data, size_t size);

// Open the containing folder in Explorer. For a file, reveals (selects) it;
// for a directory, opens that directory. No-op (false) on failure.
bool OpenInExplorer(const std::wstring& path);

bool SetClipboardText(HWND owner, const std::wstring& text);

// Zero-copy memory-mapped read-only view of a file. The mapping stays valid
// for the lifetime of the object; call Close()/destructor to unmap. Used by
// the large-file loader to stream bytes straight into Scintilla without
// building a second full-file std::string copy.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { Close(); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool Open(const std::wstring& path);
    void Close();

    const char* Data() const { return reinterpret_cast<const char*>(view_); }
    size_t Size() const { return size_; }
    bool IsValid() const { return view_ != nullptr && size_ > 0; }

private:
    void* fileH_ = nullptr;      // HANDLE
    void* mapping_ = nullptr;    // HANDLE
    const unsigned char* view_ = nullptr;
    size_t size_ = 0;
};

} // namespace xfs
