#pragma once
// SciMarshal.h — 进程外插件的 SCI_* 通道形状表（v2.3，批次 112）。
//
// 为什么需要它
// ------------
// SCI_* 的编号全在 WM_USER 之上（Scintilla 的 SCI_START = 2000，实测区间
// 2001..4033），与 NPPM_* 受同一条规律支配：**Windows 不封送参数**。但 SCI_*
// 与 NPPM_* 有一个决定性差别 —— 826 条里有 **716 条根本不带指针**（参数与返回
// 值都是整数/枚举），这些跨进程直发是**完全安全**的；只有 110 条带指针
// （入参串 / 出参缓冲 / 结构体指针 / 裸指针 / 返回指针）才会写坏宿主内存。
//
// 所以本通道的策略不是"全部过桥"，而是**按形状分流**：
//   * kValue —— 无指针参数且无指针返回值 ⇒ 中转窗**直发**宿主真 Scintilla。
//     这一步不需要任何封送：没有指针可封送。
//   * 其余（含表外编号）—— 一律**拒答**（返回契约失败值 0）+ 记账，由后续批次
//     逐族桥接（kInStr / kOutStr / kInOutStr / kStruct / kInBytes 都是可桥接的；
//     kRawPtr / kPtrRet 语义上不可桥接）。
//
// 表的完备性
// ----------
// 表由 scripts/gen-sci-marshal.py 从 Scintilla **自己的**机器可读接口定义
// third_party/scintilla/include/Scintilla.iface 生成（SciMarshalTable.inc），
// 并用两条独立判据对撞：ScintillaCall.h 的 C++ 签名（'*' 判据）、
// ScintillaMessages.h 的 id 集合。生成器的覆盖面边界写在它自己的文件头里。
//
// 关键安全性质：**表外编号 = 拒答**。所以 Scintilla 升级后新增的消息只会
// "安全地不可用"，不会"危险地可用" —— 这正是我们要的失败方向。
//
// 已知边界（别把 0 当成功）
// ------------------------
//   * 本表只回答"这条 SCI_* 的参数/返回里有没有指针"，**不**回答"宿主的
//     Scintilla 是否实现了它"。未实现的消息也返回 0，与"被拒答"**同形**。
//   * 值类型消息里有一族**跨进程语义本来就不同**的（如 SetFocus / GrabFocus /
//     GetFocus / SetCursor），直发不会崩，但作用于宿主窗口时行为可能与进程内
//     调用不同。本表不做区分（它们没有指针，不是安全问题）。
//   * 本批**不**桥接任何带指针的 SCI_*，包括最常用的 SCI_GETTEXT。这是有意的
//     取舍：先保证"不可能写坏宿主"，再逐族放开。

#include <cstddef>

namespace xfs {
namespace oop {

// 形状。前 8 个由生成器写入表；kUnknown 表示"不在表里"。
enum class SciShape : unsigned char {
    kValue = 0,   // 无指针 ⇒ 中转窗直发宿主真 Scintilla
    kInStr,       // string        入参 NUL 结尾串
    kOutStr,      // stringresult  调用方缓冲（容量由别的参数给，见文件头）
    kInOutStr,    // string + stringresult 同时出现
    kStruct,      // textrange / textrangefull / findtext / findtextfull /
                  // formatrange / formatrangefull —— 结构体里还有指针
    kInBytes,     // cells         裸字节缓冲
    kRawPtr,      // pointer       不透明裸指针
    kPtrRet,      // 返回 pointer（文档内部指针，语义上不可复制）
    kUnknown,     // 不在表里（含 Scintilla 升级后新增的消息）
};

struct SciEntry {
    unsigned msg;
    SciShape shape;
};

#include "SciMarshalTable.inc"

// 表按 msg 严格升序 ⇒ 二分。表外返回 kUnknown（调用方必须当拒答处理）。
inline SciShape SciShapeOf(unsigned msg) {
    std::size_t lo = 0;
    std::size_t hi = kSciCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kSciTable[mid].msg < msg)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < kSciCount && kSciTable[lo].msg == msg)
        return kSciTable[lo].shape;
    return SciShape::kUnknown;
}

// 直发条件：**只有** kValue。其余（含表外）一律拒答。
inline bool SciForwardable(unsigned msg) {
    return SciShapeOf(msg) == SciShape::kValue;
}

// 本窗口负责的编号区间（含端点）。区间外交给转发清单 / DefWindowProc。
inline bool SciInRange(unsigned msg) {
    return msg >= kSciMsgMin && msg <= kSciMsgMax;
}

inline const char* SciShapeName(SciShape sh) {
    switch (sh) {
    case SciShape::kValue:    return "value";
    case SciShape::kInStr:    return "in-string";
    case SciShape::kOutStr:   return "out-string";
    case SciShape::kInOutStr: return "in+out-string";
    case SciShape::kStruct:   return "struct-with-pointer";
    case SciShape::kInBytes:  return "in-bytes";
    case SciShape::kRawPtr:   return "raw-pointer";
    case SciShape::kPtrRet:   return "pointer-result";
    case SciShape::kUnknown:  return "unknown";
    }
    return "?";
}

// 拒答原因（ASCII，直接进日志/诊断）。kValue 没有原因 ⇒ 返回 nullptr。
// 每一句都写清"为什么现在不行 + 该走哪条路"，免得下一个人从 0 反推。
inline const char* SciRefusalWhy(SciShape sh) {
    switch (sh) {
    case SciShape::kValue:
        return nullptr;
    case SciShape::kInStr:
        return "input string pointer: bridgeable (copy the bytes, host rebuilds)";
    case SciShape::kOutStr:
        return "output string buffer: bridgeable but needs the caller's capacity, "
               "which is not in the message for every member of this family";
    case SciShape::kInOutStr:
        return "input string plus output buffer: bridgeable in the same pass as "
               "out-string once capacity is resolved";
    case SciShape::kStruct:
        return "struct containing further pointers: bridgeable by flattening the "
               "struct and inlining the strings";
    case SciShape::kInBytes:
        return "raw byte buffer: bridgeable by copying the bytes";
    case SciShape::kRawPtr:
        return "opaque pointer argument: not bridgeable";
    case SciShape::kPtrRet:
        return "returns a pointer into the document: not bridgeable by copying";
    case SciShape::kUnknown:
        return "not in the table (unknown or newer than the vendored Scintilla): "
               "refused by default";
    }
    return "?";
}

} // namespace oop
} // namespace xfs
