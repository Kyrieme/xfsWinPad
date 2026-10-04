#pragma once
// NppPointerGuard.h — NPPM_* 垫片的"外来指针"防护判据。
//
// 为什么需要：NPPM_* 的编号全部落在 WM_USER 之上，而 Windows **只对
// WM_USER 以下的部分消息、WM_COPYDATA、WM_GETTEXT/WM_SETTEXT 等做参数
// 封送**。超过 WM_USER 的消息，wp/lp 按位原样传给目标窗口过程。
//
// 于是进程外插件（OOP 代理进程）把 NPPM_* 直接发给宿主主窗口时，
// wp/lp 里的指针是**代理进程地址空间**的地址。宿主一旦解引用，读写的
// 是"同一个数值地址在自己地址空间里的内容"——那个地址大概率未映射，
// 结果是宿主进程当场访问违例（这些分支外面没有 SEH）。
//
// 本模块提供"这个指针在我自己的地址空间里能不能安全读/写这么多字节"。
// 判据是**保守**的（只认 MEM_COMMIT 且保护位允许、且不是 PAGE_GUARD）：
//   * 合法调用方（进程内插件）传的指针必然满足 ⇒ 不产生误报；
//   * 外来指针、未映射地址、只读页、保留区必然不满足 ⇒ 拒答而不是崩。
//
// 拒答语义由调用方决定（NPPM 的失败值是 FALSE/0），本模块只回答"能不能"。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>

namespace xfs {
namespace npp {

// p 起连续 bytes 字节是否可安全读取（committed + 允许读 + 非 guard 页）。
// p == nullptr 返回 false；bytes == 0 返回 true（无需触碰内存）。
bool LocalReadable(const void* p, std::size_t bytes);

// 同上，但要求可写。
bool LocalWritable(void* p, std::size_t bytes);

// 有界宽字符串扫描：返回 p 处**含结尾 NUL 在内**可安全读取的 wchar_t 数。
// 在 maxChars 个 wchar_t 之内没找到 NUL、或中途遇到不可读区域 ⇒ 返回 0
// （调用方必须按"指针不可用"拒答）。
// 存在的意义：直接对 lp 做 std::wstring 构造会**无界扫描**——外来指针
// 会在第一页就崩，而合法但未终止的缓冲会读穿。这里把两者都变成 0。
std::size_t LocalWideStrChars(const wchar_t* p, std::size_t maxChars);

// 有界窄字符串扫描（与 LocalWideStrChars 同语义，按**字节**计）。
// 返回 p 处**含结尾 NUL 在内**可安全读取的字节数；界内未见 NUL、或中途遇到
// 不可读区域 ⇒ 返回 0（调用方必须按"指针不可用"拒答）。
// 存在的意义与宽串版相同，但多一层：SCI_* 的入参串是 UTF-8 字节串，而
// Scintilla 对 kStrInLp / kStrInWp 形态是按 C 串读的 —— 一个未终止的缓冲
// 交给 memcpy 就是越界读（读穿到下一页 = 代理进程当场死）。
std::size_t LocalNarrowStrBytes(const char* p, std::size_t maxBytes);

} // namespace npp
} // namespace xfs
