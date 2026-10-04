// DmmMarshal.cpp — 停靠族载荷布局的组装/解析（见 DmmMarshal.h 的规则）。
//
// 本文件只做**字节层**的事：把结构体/串变成 wire 载荷，以及反过来。
// 它不认识 NPPM_* 编号、也不认识 DockManager —— 那两件事分别由
// PluginHostMain.cpp（代理侧中转窗）与 OopHost.cpp（宿主侧）负责。
// 这样切分的价值：e2e 夹具与单测可以**直接调用**这两对函数，
// 不必把编辑器侧或代理侧实现一遍（否则"两边一致"只能证明夹具自洽）。
//
// ★ 唯一例外是图标段（批次 137）：图标必须**解引用 HICON**（GetIconInfo /
//   GetDIBits），那是 GDI 不是字节。例外放在这里而不是另开文件，是因为
//   "布局在哪、编解码就在哪"比"为纯度拆成两处"更不容易漂移 —— 一对编解码
//   函数分居两文件，正是 DmmMarshal.h 开头那段"各写一份必然漂移"说的情形。

#include "DmmMarshal.h"

#include "OopProtocol.h"                // kNppmPayloadMax（图标段的第二道闸）
#include "../npp/NppPointerGuard.h"

#include <cstring>

namespace xfs {
namespace oop {

namespace {

// 有界读一个宽串的字符数（含 NUL），并把它拷进 out 的尾部。
//   * s == nullptr      ⇒ 写 0 到 charsOut，不拷任何字节
//   * 不可读 / 无 NUL / 超过 kDmmStrCharsMax ⇒ 返回 false（调用方拒答）
// 注意 LocalWideStrChars 是**页粒度**的判据（见 NppPointerGuard.cpp），
// 所以"插件给了个指向不可读页的串"不会把代理拖死。
bool AppendWideStr(const wchar_t* s, std::vector<unsigned char>& out,
                   unsigned& charsOut) {
    if (!s) {
        charsOut = 0;
        return true;
    }
    const std::size_t n = npp::LocalWideStrChars(s, kDmmStrCharsMax);
    if (n == 0 || n > kDmmStrCharsMax) return false;   // 不可读 / 无 NUL / 超长
    charsOut = static_cast<unsigned>(n);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(s);
    out.insert(out.end(), b, b + n * sizeof(wchar_t));
    return true;
}

// 解析侧：从 p 起读 chars 个 wchar_t，校验「在范围内 + 末字节是 NUL」。
// chars == 0 ⇒ 返回 nullptr（NULL 语义）且 ok 保持 true。
bool ReadWideStr(const unsigned char* base, unsigned total, unsigned& off,
                 unsigned chars, const wchar_t*& out) {
    if (chars == 0) {
        out = nullptr;
        return true;
    }
    if (chars > kDmmStrCharsMax) return false;
    const unsigned bytes = chars * static_cast<unsigned>(sizeof(wchar_t));
    if (off > total || bytes > total - off) return false;   // 越界（不溢出）
    const unsigned char* seg = base + off;
    // 末字节必须是 NUL —— 否则宿主会把一个"没有结尾的串"当串用
    // （越界读，正是本模块存在的理由）。
    const wchar_t* w = reinterpret_cast<const wchar_t*>(seg);
    if (w[chars - 1] != L'\0') return false;
    out = w;
    off += bytes;
    return true;
}

// ---- 图标的字节算术与 DIB 头（编解码共用同一份，防两侧漂移）-----------------
// 任一为 0 或超出安全上界 ⇒ false。上界取 4096：一个停靠标签图标不可能更大，
// 而它同时保证了下面 `4ull*w*h` 之类的算式不会溢出 64 位（4×2^32×2^32 = 2^66）。
bool IconDims(unsigned w, unsigned h) {
    return w >= 1 && h >= 1 && w <= 4096u && h <= 4096u;
}
unsigned long long IconColorBytes(unsigned w, unsigned h) { return 4ull * w * h; }
unsigned long long IconMaskBytes(unsigned w, unsigned h) {
    return ((static_cast<unsigned long long>(w) + 31) / 32 * 4) * h;
}

// top-down（biHeight 取负）是两侧的**统一约定**：正向 GetDIBits 拿到的行序与
// 反向 CreateDIBSection 写入的行序必须一致，否则图标会上下颠倒（静默错 ——
// 一张图看着"像"但不等于原图，只有逐像素断言抓得到）。
void FillColorDib(BITMAPINFO& bi, int w, int h) {
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
}
// 1bpp 掩码：DIB 的 1bpp 需要调色板，否则 GetDIBits / CreateDIBSection 都可能
// 失败（掩码本身不关心颜色，只要两格合法即可）。
struct MaskDib {
    BITMAPINFOHEADER h;
    RGBQUAD          pal[2];
};
void FillMaskDib(MaskDib& b, int w, int h) {
    std::memset(&b, 0, sizeof(b));
    b.h.biSize = sizeof(BITMAPINFOHEADER);
    b.h.biWidth = w;
    b.h.biHeight = -h;
    b.h.biPlanes = 1;
    b.h.biBitCount = 1;
    b.h.biCompression = BI_RGB;
    b.pal[0].rgbBlue = b.pal[0].rgbGreen = b.pal[0].rgbRed = 0;
    b.pal[1].rgbBlue = b.pal[1].rgbGreen = b.pal[1].rgbRed = 255;
}

// 代理侧：把插件给的 HICON 转码成图标段追加到 out（**不失败**）。
// 无图标 / 单色 / 过大 / 顶穿载荷上限 / GDI 不认 ⇒ 只追加一个 width==0 的头。
// 失败路径就地留零（进函数时已 resize 出 16 字节的头），调用方无需区分。
void AppendIcon(HICON hIcon, std::vector<unsigned char>& out, std::size_t payloadMax) {
    const std::size_t headAt = out.size();
    out.resize(headAt + sizeof(DmmIconHead));      // 占位；失败就是 width==0
    if (!hIcon) return;

    ICONINFO ii{};
    if (!::GetIconInfo(hIcon, &ii)) return;
    // ★ GetIconInfo 交出的两个位图**归调用方**，成败都要删（否则每注册一次
    //   面板漏两个 GDI 对象 ⇒ 长时间运行必然耗尽 GDI 句柄）。
    HBITMAP hColor = ii.hbmColor;
    HBITMAP hMask = ii.hbmMask;
    if (!hColor || !hMask) {                       // 单色/无掩码图标：本版不搬
        if (hColor) ::DeleteObject(hColor);
        if (hMask) ::DeleteObject(hMask);
        return;
    }

    BITMAP bm{};
    if (!::GetObjectW(hColor, sizeof(bm), &bm)) {
        ::DeleteObject(hColor); ::DeleteObject(hMask);
        return;
    }
    const unsigned w = (unsigned)bm.bmWidth;
    const unsigned h = (unsigned)bm.bmHeight;
    if (!IconDims(w, h)) {
        ::DeleteObject(hColor); ::DeleteObject(hMask);
        return;
    }
    const unsigned long long colorBytes = IconColorBytes(w, h);
    const unsigned long long maskBytes = IconMaskBytes(w, h);
    // 过大 ⇒ 退化；且**加上后**不能顶穿整单载荷上限（否则代理侧的总量闸会拒掉
    // 整个注册请求 —— 丢图标保注册是这里的取舍）。
    if (colorBytes + maskBytes > kDmmIconBytesMax ||
        headAt + sizeof(DmmIconHead) + colorBytes + maskBytes > payloadMax) {
        ::DeleteObject(hColor); ::DeleteObject(hMask);
        return;
    }

    HDC dc = ::GetDC(nullptr);
    if (!dc) { ::DeleteObject(hColor); ::DeleteObject(hMask); return; }
    std::vector<unsigned char> color(static_cast<std::size_t>(colorBytes));
    std::vector<unsigned char> mask(static_cast<std::size_t>(maskBytes));
    BITMAPINFO bi{};
    FillColorDib(bi, (int)w, (int)h);
    const int gotColor = ::GetDIBits(dc, hColor, 0, (UINT)h, color.data(), &bi,
                                     DIB_RGB_COLORS);
    MaskDib mb{};
    FillMaskDib(mb, (int)w, (int)h);
    const int gotMask = ::GetDIBits(dc, hMask, 0, (UINT)h, mask.data(),
                                    reinterpret_cast<BITMAPINFO*>(&mb), DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    ::DeleteObject(hColor); ::DeleteObject(hMask);
    if (gotColor == 0 || gotMask == 0) return;     // GDI 不认 ⇒ 退化

    DmmIconHead ih{};
    ih.width = w;
    ih.height = h;
    ih.colorBytes = (unsigned)colorBytes;
    ih.maskBytes = (unsigned)maskBytes;
    std::memcpy(out.data() + headAt, &ih, sizeof(ih));
    out.insert(out.end(), color.begin(), color.end());
    out.insert(out.end(), mask.begin(), mask.end());
}

} // namespace

bool DmmBuildReg(const npp::DockedWidgetData& d, std::vector<unsigned char>& out) {
    DmmRegHead h{};
    h.hClient = reinterpret_cast<UINT_PTR>(d.hClient);
    h.uMask = static_cast<UINT_PTR>(d.uMask);
    h.dlgID = d.dlgID;

    out.clear();
    out.resize(sizeof(DmmRegHead));
    if (!AppendWideStr(d.pszName, out, h.nameChars)) return false;
    if (!AppendWideStr(d.pszAddInfo, out, h.addInfoChars)) return false;
    if (!AppendWideStr(d.pszModuleName, out, h.moduleChars)) return false;
    memcpy(out.data(), &h, sizeof(h));
    // 图标段恒在（width==0 = 无图标）。放在三串**之后**：串读不出来就拒答，
    // 不必白做一次 GDI 转码。
    AppendIcon(d.hIconTab, out, kNppmPayloadMax);
    return true;
}

bool DmmBuildTwoStr(const wchar_t* wp, const wchar_t* lp,
                    std::vector<unsigned char>& out) {
    DmmTwoStrHead h{};
    out.clear();
    out.resize(sizeof(DmmTwoStrHead));
    if (!AppendWideStr(wp, out, h.wpChars)) return false;
    if (!AppendWideStr(lp, out, h.lpChars)) return false;
    memcpy(out.data(), &h, sizeof(h));
    return true;
}

DmmRegView DmmParseReg(const unsigned char* p, unsigned bytes) {
    DmmRegView v;
    if (!p || bytes < sizeof(DmmRegHead)) return v;
    DmmRegHead h{};
    memcpy(&h, p, sizeof(h));
    unsigned off = static_cast<unsigned>(sizeof(DmmRegHead));
    if (!ReadWideStr(p, bytes, off, h.nameChars, v.name)) return v;
    if (!ReadWideStr(p, bytes, off, h.addInfoChars, v.addInfo)) return v;
    if (!ReadWideStr(p, bytes, off, h.moduleChars, v.module)) return v;

    // 图标段（恒在）：16 字节头 + 可选两段，且**总字节必须恰好用完**。
    // "恰好用完"是判据而不是形式主义：允许尾随字节 ⇒ 截断/拼接的载荷与合法
    // 载荷同形，而这里的每一段后面都要被 memcpy/交给 GDI。
    if (bytes - off < sizeof(DmmIconHead)) return v;
    DmmIconHead ih{};
    memcpy(&ih, p + off, sizeof(ih));
    off += static_cast<unsigned>(sizeof(DmmIconHead));
    if (ih.width == 0) {
        // 无图标：其余三格必须是 0（否则头与体自相矛盾），且不得有尾随字节
        if (ih.height != 0 || ih.colorBytes != 0 || ih.maskBytes != 0) return v;
    } else {
        if (!IconDims(ih.width, ih.height)) return v;
        const unsigned long long cb = IconColorBytes(ih.width, ih.height);
        const unsigned long long mb = IconMaskBytes(ih.width, ih.height);
        // 重算并逐格比对：不信任头里自报的字节数（那是外来数据）
        if (ih.colorBytes != cb || ih.maskBytes != mb) return v;
        if (cb + mb > kDmmIconBytesMax) return v;
        const unsigned long long need =
            static_cast<unsigned long long>(off) + cb + mb;
        if (need != bytes) return v;               // 多一字节 / 少一字节都拒
        v.iconColor = p + off;
        v.iconMask = p + off + ih.colorBytes;
        v.iconWidth = ih.width;
        v.iconHeight = ih.height;
        v.iconColorBytes = ih.colorBytes;
        v.iconMaskBytes = ih.maskBytes;
        off = static_cast<unsigned>(need);
    }
    if (off != bytes) return v;                    // 无图标时同样不许有尾随字节

    v.hClient = h.hClient;
    v.uMask = h.uMask;
    v.dlgID = h.dlgID;
    v.ok = true;
    return v;
}

HICON DmmIconFromView(const DmmRegView& v) {
    if (!v.ok || v.iconWidth == 0 || !v.iconColor || !v.iconMask) return nullptr;
    const int w = (int)v.iconWidth;
    const int h = (int)v.iconHeight;
    HDC dc = ::GetDC(nullptr);
    if (!dc) return nullptr;
    BITMAPINFO bi{};
    FillColorDib(bi, w, h);
    void* cbits = nullptr;
    HBITMAP color = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &cbits, nullptr, 0);
    MaskDib mb{};
    FillMaskDib(mb, w, h);
    void* mbits = nullptr;
    HBITMAP mask = ::CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&mb),
                                      DIB_RGB_COLORS, &mbits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (!color || !mask || !cbits || !mbits) {
        if (color) ::DeleteObject(color);
        if (mask) ::DeleteObject(mask);
        return nullptr;
    }
    std::memcpy(cbits, v.iconColor, v.iconColorBytes);
    std::memcpy(mbits, v.iconMask, v.iconMaskBytes);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = ::CreateIconIndirect(&ii);       // 会**复制**两个位图
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return ic;                                  // 失败 ⇒ nullptr（面板照常注册）
}

DmmTwoStrView DmmParseTwoStr(const unsigned char* p, unsigned bytes) {
    DmmTwoStrView v;
    if (!p || bytes < sizeof(DmmTwoStrHead)) return v;
    DmmTwoStrHead h{};
    memcpy(&h, p, sizeof(h));
    unsigned off = static_cast<unsigned>(sizeof(DmmTwoStrHead));
    if (!ReadWideStr(p, bytes, off, h.wpChars, v.wp)) return v;
    if (!ReadWideStr(p, bytes, off, h.lpChars, v.lp)) return v;
    v.ok = true;
    return v;
}

} // namespace oop
} // namespace xfs
