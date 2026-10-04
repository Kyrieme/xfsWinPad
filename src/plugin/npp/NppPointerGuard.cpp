#include "NppPointerGuard.h"

#include <cstdint>
#include <cstring>

namespace xfs {
namespace npp {

namespace {

// 读/写允许位（PAGE_EXECUTE 单独出现时不可读，故不入表）。
constexpr DWORD kReadMask =
    PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kWriteMask =
    PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

// 查询 cur 所在区域；可用则回填区域末尾（独占）与保护位。
bool RegionAt(uintptr_t cur, uintptr_t& regionEnd, DWORD& prot) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) !=
        sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT) return false;          // 保留/空闲
    prot = mbi.Protect;
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;   // 触碰会炸
    if ((prot & kReadMask) == 0) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    regionEnd = base + mbi.RegionSize;
    // 区域末尾不前进 = 除零/死循环风险；宁可判不可用。
    return regionEnd > cur;
}

bool RangeOk(const void* p, std::size_t bytes, bool needWrite) {
    if (!p) return false;
    if (bytes == 0) return true;
    uintptr_t cur = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = cur + bytes;
    if (end < cur) return false;                        // 长度溢出
    while (cur < end) {
        uintptr_t regionEnd = 0;
        DWORD prot = 0;
        if (!RegionAt(cur, regionEnd, prot)) return false;
        if (needWrite && (prot & kWriteMask) == 0) return false;
        cur = regionEnd;
    }
    return true;
}

} // namespace

bool LocalReadable(const void* p, std::size_t bytes) {
    return RangeOk(p, bytes, /*needWrite=*/false);
}

bool LocalWritable(void* p, std::size_t bytes) {
    return RangeOk(p, bytes, /*needWrite=*/true);
}

std::size_t LocalWideStrChars(const wchar_t* p, std::size_t maxChars) {
    if (!p || maxChars == 0) return 0;
    uintptr_t cur = reinterpret_cast<uintptr_t>(p);
    std::size_t seen = 0;
    while (seen < maxChars) {
        uintptr_t regionEnd = 0;
        DWORD prot = 0;
        if (!RegionAt(cur, regionEnd, prot)) return 0;
        // 本区域内还能安全读的 wchar_t 数（不跨区取半字符）
        const std::size_t avail = (regionEnd - cur) / sizeof(wchar_t);
        if (avail == 0) { cur = regionEnd; continue; }
        const std::size_t room = maxChars - seen;
        const std::size_t limit = room < avail ? room : avail;
        const wchar_t* q = reinterpret_cast<const wchar_t*>(cur);
        for (std::size_t i = 0; i < limit; ++i)
            if (q[i] == L'\0') return seen + i + 1;
        seen += limit;
        cur += limit * sizeof(wchar_t);
    }
    return 0;   // 界内未见 NUL：按不可用处理
}

std::size_t LocalNarrowStrBytes(const char* p, std::size_t maxBytes) {
    if (!p || maxBytes == 0) return 0;
    uintptr_t cur = reinterpret_cast<uintptr_t>(p);
    std::size_t seen = 0;
    while (seen < maxBytes) {
        uintptr_t regionEnd = 0;
        DWORD prot = 0;
        if (!RegionAt(cur, regionEnd, prot)) return 0;
        const std::size_t avail = regionEnd - cur;      // 本区域内可安全读的字节
        const std::size_t room = maxBytes - seen;
        const std::size_t limit = room < avail ? room : avail;
        // 用 memchr 而不是手写"逐字符 + 条件返回"循环：后者是 MSVC v145 /O2
        // 的已知误编译形态（只在 Release 下静默出错），标准库实现没有这个风险。
        const char* q = reinterpret_cast<const char*>(cur);
        const void* hit = std::memchr(q, 0, limit);
        if (hit)
            return seen + static_cast<std::size_t>(
                              static_cast<const char*>(hit) - q) + 1;
        seen += limit;
        cur += limit;
    }
    return 0;   // 界内未见 NUL：按不可用处理
}

} // namespace npp
} // namespace xfs
