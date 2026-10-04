// UiFont.cpp — 见 UiFont.h 的说明（批次 145）。
//
// 单独一个编译单元（不折进 Util.cpp）：本函数依赖 user32 的
// SystemParametersInfoForDpi，而 core/Util.cpp 会被多个**不链 user32** 的测试
// 目标（test_encoding / test_settings / test_stdf …）直接编进去，折进去会在
// 链接期把这些测试一起拖红。
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "UiFont.h"

namespace xfs {

namespace {

// 取 dpi 对应的「消息字体」：优先 ForDpi 变体（Win10 1607+，按目标 dpi 缩放），
// 老系统退回当前系统 dpi 的版本。
bool MessageFontForDpi(int dpi, LOGFONTW& out) {
    if (dpi <= 0) dpi = 96;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                     static_cast<UINT>(dpi))) {
        out = ncm.lfMessageFont;
        return true;
    }

    ncm = NONCLIENTMETRICSW{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        out = ncm.lfMessageFont;
        return true;
    }
    return false;
}

} // namespace

HFONT CreateUiFont(int dpi) {
    LOGFONTW lf{};
    if (!MessageFontForDpi(dpi, lf)) return nullptr;
    return ::CreateFontIndirectW(&lf);
}

} // namespace xfs
