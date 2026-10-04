// SciBridge.cpp — SCI_* 指针参数族的过桥实现（v2.7，批次 113–116）。
//
// 这个文件同时被**两侧**编译进去：
//   * 编辑器（xfsWinPad.exe）：只用 SciBridgeCall() / SciBridgeOutCall()，
//     把 wire 里的字节还原成宿主本地指针（或反过来，把宿主缓冲的内容取出来）。
//   * 代理（xfsWinPadPluginHost.exe）：只用 SciReadInbound()，把插件给的
//     wp/lp 读成字节。
// 放在一起是**有意**的：两个方向共用同一张布局表（kSciInTable）与同一张容量表
// （kSciOutTable，都是生成物），分类只有一份 ⇒ 不会出现"代理以为串在 lp、
// 宿主以为串在 wp"或"代理按 need 拷、宿主按 need+1 写"这种漂移。
//
// 两侧的安全前提不同，注释里分开写清了：
//   * 代理侧面对的是**插件给的指针**（本进程内合法，但可能是垃圾/未终止）；
//   * 编辑器侧面对的是**wire 里的长度**（外来数据，一律先钳再用）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <vector>

#include "SciBridge.h"
#include "../npp/NppPointerGuard.h"

namespace xfs {
namespace oop {

bool SciReadInbound(SciInLayout lay, UINT_PTR wp, UINT_PTR lp,
                    std::vector<unsigned char>& b1,
                    std::vector<unsigned char>& b2,
                    UINT_PTR& argWp, UINT_PTR& argLp) {
    b1.clear();
    b2.clear();
    argWp = 0;
    argLp = 0;
    if (lay == SciInLayout::kNone) return false;

    bool wpPtr = false, lpPtr = false;
    SciInPtrSlots(lay, wpPtr, lpPtr);
    // 指针槽不随 wire 走（填 0），其余槽原样透传 —— 编辑器侧按同一张表还原。
    argWp = wpPtr ? 0u : wp;
    argLp = lpPtr ? 0u : lp;

    const std::size_t maxBytes = kSciPayloadMax;
    const char* p1 = reinterpret_cast<const char*>(wpPtr ? wp : lp);
    std::size_t n1 = 0;

    if (SciInLenFromWp(lay)) {
        // wp **就是**字节数（AddText / ReplaceTarget / SearchInTarget / cells…）。
        // 超限一律拒答：截断会静默改变语义（pattern 被砍一半，搜索结果"看起来
        // 正常"但是错的）。
        if (wp > maxBytes) return false;
        n1 = static_cast<std::size_t>(wp);
        // 长度 0 是合法的空串；此时不触碰指针（Scintilla 也不会解引用）。
        if (n1 && !xfs::npp::LocalReadable(p1, n1)) return false;
    } else {
        // 长度未知 ⇒ 有界 NUL 扫描。返回 0 有两种含义（指针不可读 / 界内没有
        // 终止符），两种都必须拒答：拿一个未终止的缓冲去 memcpy 是越界读。
        n1 = xfs::npp::LocalNarrowStrBytes(p1, maxBytes);
        if (n1 == 0) return false;
    }
    // 多留一个 NUL：宿主侧 Scintilla 对这些形态按 C 串读，而长度桶的调用方
    // 可能根本没给终止符。
    b1.assign(n1 + 1, 0);
    if (n1) std::memcpy(b1.data(), p1, n1);

    if (lay == SciInLayout::kStrBoth) {
        const char* p2 = reinterpret_cast<const char*>(lp);
        const std::size_t n2 = xfs::npp::LocalNarrowStrBytes(p2, maxBytes);
        if (n2 == 0) return false;
        b2.assign(n2 + 1, 0);
        std::memcpy(b2.data(), p2, n2);
    }
    return true;
}

bool SciBridgeCall(HWND sciTarget, const SciCallWire& cw, DWORD cbData,
                   LRESULT& out) {
    out = 0;
    const SciInLayout lay = SciInLayoutOf(cw.sciMsg);
    if (lay == SciInLayout::kNone) return false;   // 不在入参族：拒答
    if (!sciTarget) return false;
    if (cbData < sizeof(SciCallWire)) return false;

    // wire 是外来数据：两段长度都必须先按**实际载荷**钳，再按上限钳。
    const unsigned bodyMax =
        static_cast<unsigned>(cbData - sizeof(SciCallWire));
    unsigned n1 = cw.inBytes;
    if (n1 > bodyMax) n1 = bodyMax;
    unsigned n2 = cw.inBytes2;
    if (n2 > bodyMax - n1) n2 = bodyMax - n1;
    if (n1 > kSciPayloadMax || n2 > kSciPayloadMax) return false;

    const unsigned char* body =
        reinterpret_cast<const unsigned char*>(&cw) + sizeof(cw);
    // 各多留一个 NUL：空段也拿到一个合法的空串缓冲（空串是合法实参，
    // 不能因为 inBytes==0 就拒答）。
    std::vector<unsigned char> b1(static_cast<std::size_t>(n1) + 1, 0);
    std::vector<unsigned char> b2(static_cast<std::size_t>(n2) + 1, 0);
    if (n1) std::memcpy(b1.data(), body, n1);
    if (n2) std::memcpy(b2.data(), body + n1, n2);

    WPARAM hostWp = 0;
    LPARAM hostLp = 0;
    SciHostArgs(lay, cw.argWp, cw.argLp, b1.data(), b2.data(), hostWp, hostLp);
    out = ::SendMessageW(sciTarget, cw.sciMsg, hostWp, hostLp);
    return true;
}

// ---- 出参族（kOutStr 30 条）—— 批次 114 -------------------------------------
// 三步：探长度 → 真调用 → 哨兵校验。三步的顺序是必须的，理由各不同：
//   ① 探长度（lParam==0）：**容量唯一的来源**。不能拿 wParam 当容量 —— 出参族
//      的 wp 是"行号 / 样式号 / 标签号"这类东西（GetLine 的 wp 就是行号），
//      当容量用会算出一个与真实长度无关的数。
//   ② 真调用：缓冲比 copied 多留 kSciOutCanaryMargin 字节。多给不会害事 ——
//      Scintilla 只写 copied 个字节。
//   ③ 哨兵校验：把"写多了"（越界写）与"NUL 语义搞反"变成**可读的失败**，
//      而不是"结果看着还行"。
//
// ⚠ 回传字节数用 probe 的返回值算，**绝不用真调用的返回值**：GetCurLine 返回
//   的是光标列号、GetText 返回的是实际拷入量（被 wp 钳过）。本批负控②钉这条。
bool SciBridgeOutCall(HWND sciTarget, const SciOutCallWire& cw, DWORD cbData,
                      std::vector<unsigned char>& outBytes,
                      unsigned long& need, unsigned long& copied,
                      LRESULT& out) {
    outBytes.clear();
    need = 0;
    copied = 0;
    out = 0;

    const SciOutKind kind = SciOutKindOf(cw.sciMsg);
    if (kind == SciOutKind::kNone) return false;            // 不在出参族
    if (kind == SciOutKind::kRefuseRuntime) return false;   // 见 SciBridge.h
    if (!sciTarget) return false;
    if (cbData < sizeof(SciOutCallWire)) return false;

    // ---- 入参串（kInOutStr 族）：在**宿主本地**重建 wParam -------------------
    // ⚠ cw 必须指向 wire 缓冲的**起始处**（调用方传的是 COPYDATA 里的那个结构），
    //   因为载荷就紧随结构体之后 —— 与回包侧读 payload 的写法同源。
    //
    // wire 里的字节是外来数据，五道校验缺一不可：长度上限、载荷必须真的落在
    // cbData 之内、最后一个字节必须是 NUL（宿主侧 Scintilla 按 C 串读它）、
    // 带串时 argWp 必须为 0（否则同一个请求里会同时存在"值语义 wParam"与
    // "串指针 wParam"两个答案）、以及**反方向**的"不该带串的族带了串也拒答"
    // —— 少了最后两条，inBytes 就成了一个可以绕过 argWp 契约的旁路。
    std::vector<char> inStr;
    WPARAM hostWp = static_cast<WPARAM>(cw.argWp);
    if (SciOutNeedsInStr(cw.sciMsg)) {
        if (cw.inBytes == 0) return false;
        if (cw.inBytes > kSciPayloadMax) return false;
        if (cbData < sizeof(SciOutCallWire) + cw.inBytes) return false;
        const char* p = reinterpret_cast<const char*>(&cw) + sizeof(SciOutCallWire);
        if (p[cw.inBytes - 1] != '\0') return false;
        if (cw.argWp != 0) return false;
        inStr.assign(p, p + cw.inBytes);
        hostWp = reinterpret_cast<WPARAM>(inStr.data());
    } else if (cw.inBytes != 0) {
        return false;
    }

    // ① 探长度。返回值是"需要的字节数"，不会是负数；负数只可能来自未实现的
    //    消息（宿主返回 -1 之类）⇒ 一律当拒答，不猜。
    const LRESULT probe = ::SendMessageW(sciTarget, cw.sciMsg, hostWp, 0);
    if (probe < 0) return false;
    need = static_cast<unsigned long>(probe);
    if (need > kSciPayloadMax) return false;   // 超限拒答（**不截断**）

    if (!cw.writeBack) {
        out = probe;                           // 插件自己在查长度：到此为止
        return true;
    }

    copied = SciOutBytesToCopy(kind, need, static_cast<unsigned long>(cw.argWp));
    if (copied > kSciPayloadMax + 1) return false;
    if (copied == 0) {
        // 只有 kNoNul 且 need==0 会走到这里（空串）。Scintilla 一个字节都不写
        // （BytesResult 的 `if (lParam && !sv.empty())`）⇒ 不做第二次调用，
        // 返回值就是 probe 的结果（同一个表达式，等价）。
        out = probe;
        return true;
    }

    // ② 真调用：缓冲 = copied + 哨兵余量，先填哨兵。
    std::vector<unsigned char> buf(static_cast<std::size_t>(copied) +
                                   kSciOutCanaryMargin);
    std::memset(buf.data(), kSciOutCanaryByte, buf.size());
    out = ::SendMessageW(sciTarget, cw.sciMsg, hostWp,
                         reinterpret_cast<LPARAM>(buf.data()));

    // ③ 校验：越界写 / NUL 语义搞反都要在这里变成拒答。
    if (!SciOutWroteOnlyExpected(kind, static_cast<std::size_t>(copied),
                                 buf.data(), buf.size(), kSciOutCanaryByte)) {
        outBytes.clear();
        copied = 0;
        return false;
    }
    outBytes.assign(buf.begin(),
                    buf.begin() + static_cast<std::ptrdiff_t>(copied));
    return true;
}

// ---- 结构族（kStruct 8 条）—— 批次 116 --------------------------------------
// 三步：展平/重建 → 真调用 → 写后白名单（**复用出参族那套 canary**）。
// 与出参族的**关键差别**：容量不靠探长度，而是**算出来**（SciStructBytesToWrite）。
//
// 与出入参族的**关键差别**：本族有**条件回写**（FindText 的 chrgText 只在
// pos != -1 时被写）。这里用 canary 判定"到底写没写"，**不复刻 pos != -1**。
bool SciBridgeStructCall(HWND sciTarget, const SciStructCallWire& cw, DWORD cbData,
                         std::vector<unsigned char>& outBytes,
                         unsigned long& copied, unsigned& hasChrg,
                         INT_PTR& chrgMin, INT_PTR& chrgMax,
                         LRESULT& out) {
    outBytes.clear();
    copied = 0;
    hasChrg = 0;
    chrgMin = 0;
    chrgMax = 0;
    out = 0;

    SciStructKind kind = SciStructKind::kNone;
    bool full = false;
    if (!FindSciStructEntry(cw.sciMsg, kind, full)) return false;
    if (kind == SciStructKind::kRefuseHandle) return false;   // 进程私有 HDC
    if (!sciTarget) return false;
    if (cbData < sizeof(SciStructCallWire)) return false;

    // 非 Full 变体的结构体字段是 long（Windows 上 4 字节）：wire 里的 INT_PTR 必须
    // 先落在 long 的值域内才谈得上"重建"，否则截断会**静默换一个范围**。
    if (!full) {
        const INT_PTR lo = static_cast<INT_PTR>(std::numeric_limits<long>::min());
        const INT_PTR hi = static_cast<INT_PTR>(std::numeric_limits<long>::max());
        if (cw.cpMin < lo || cw.cpMin > hi || cw.cpMax < lo || cw.cpMax > hi)
            return false;
    }

    // needle（kFindInOut）：与出入参族同一套"载荷含结尾 NUL"的约定 + 同样五道校验。
    std::vector<char> needle;
    if (SciStructNeedsInStr(cw.sciMsg)) {
        if (cw.inBytes == 0) return false;
        if (cw.inBytes > kSciPayloadMax) return false;
        if (cbData < sizeof(SciStructCallWire) + cw.inBytes) return false;
        const char* p = reinterpret_cast<const char*>(&cw) + sizeof(SciStructCallWire);
        if (p[cw.inBytes - 1] != '\0') return false;
        needle.assign(p, p + cw.inBytes);
    } else if (cw.inBytes != 0) {
        return false;      // 反方向：不该带串的族带了串 ⇒ 拒答
    }

    if (kind == SciStructKind::kRangeOut || kind == SciStructKind::kStyledOut) {
        // 容量**算得出** ⇒ 不探长度。只有 kRangeOut 需要 docLen（cpMax == -1 的
        // "到文档末尾"语义 + 越界判定都要它）。
        INT_PTR docLen = 0;
        if (kind == SciStructKind::kRangeOut) {
            const LRESULT n = ::SendMessageW(sciTarget, kSciGetTextLength, 0, 0);
            if (n < 0) return false;
            docLen = static_cast<INT_PTR>(n);
        }
        const unsigned long cap =
            SciStructBytesToWrite(kind, cw.cpMin, cw.cpMax, docLen);
        if (cap == 0) return false;

        std::vector<unsigned char> buf(static_cast<std::size_t>(cap) +
                                       kSciOutCanaryMargin);
        std::memset(buf.data(), kSciOutCanaryByte, buf.size());

        if (full) {
            abi::TextRangeFull tr{ { cw.cpMin, cw.cpMax },
                                   reinterpret_cast<char*>(buf.data()) };
            out = ::SendMessageW(sciTarget, cw.sciMsg,
                                 static_cast<WPARAM>(cw.argWp),
                                 reinterpret_cast<LPARAM>(&tr));
        } else {
            abi::TextRange tr{ { static_cast<long>(cw.cpMin),
                                 static_cast<long>(cw.cpMax) },
                               reinterpret_cast<char*>(buf.data()) };
            out = ::SendMessageW(sciTarget, cw.sciMsg,
                                 static_cast<WPARAM>(cw.argWp),
                                 reinterpret_cast<LPARAM>(&tr));
        }
        // 写后白名单（**复用**出参族的判定）：越界写 / 末尾不是 NUL 都变成拒答。
        // 两条消息写的都是"内容字节 + 结尾 NUL"，与 kNul 桶同形 ⇒ 用同一个桶。
        if (!SciOutWroteOnlyExpected(SciOutKind::kNul, cap, buf.data(), buf.size(),
                                     kSciOutCanaryByte)) {
            outBytes.clear();
            return false;
        }
        outBytes.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cap));
        copied = cap;
        return true;
    }

    // kFindInOut：入串 + **条件**回写 chrgText。
    // ★ 用 canary 判定"到底写没写"，**不复刻 pos != -1** —— 复刻等于把产品逻辑
    //   抄一份到桥上，两份会漂（见 SciStructTable.inc 头）。
    if (full) {
        abi::TextToFindFull ft{ { cw.cpMin, cw.cpMax }, needle.data(),
                                { kSciStructChrgCanary, kSciStructChrgCanary } };
        out = ::SendMessageW(sciTarget, cw.sciMsg, static_cast<WPARAM>(cw.argWp),
                             reinterpret_cast<LPARAM>(&ft));
        if (ft.chrgText.cpMin != kSciStructChrgCanary) {
            if (ft.chrgText.cpMin < 0 || ft.chrgText.cpMax < ft.chrgText.cpMin)
                return false;      // 写了个不可能的范围 ⇒ 拒答，不猜
            hasChrg = 1;
            chrgMin = ft.chrgText.cpMin;
            chrgMax = ft.chrgText.cpMax;
        }
    } else {
        const long canary = kSciStructChrgCanaryCR;
        abi::TextToFind ft{ { static_cast<long>(cw.cpMin),
                              static_cast<long>(cw.cpMax) },
                            needle.data(), { canary, canary } };
        out = ::SendMessageW(sciTarget, cw.sciMsg, static_cast<WPARAM>(cw.argWp),
                             reinterpret_cast<LPARAM>(&ft));
        if (ft.chrgText.cpMin != canary) {
            if (ft.chrgText.cpMin < 0 || ft.chrgText.cpMax < ft.chrgText.cpMin)
                return false;
            hasChrg = 1;
            chrgMin = ft.chrgText.cpMin;
            chrgMax = ft.chrgText.cpMax;
        }
    }
    return true;
}

} // namespace oop
} // namespace xfs
