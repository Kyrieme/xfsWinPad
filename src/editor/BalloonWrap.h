#pragma once
// xfsWinPad - 悬停气泡的按像素折行（批次 103 落地，批次 118 抽成纯函数）
//
// 【为什么必须抽出来】
//   批次 103 把折行写在 Editor.cpp 的 WrapBalloonText 里，量宽直接抓 GDI 的 HDC。
//   后果是「整段没有空格的超长词按像素硬切」那条**防御分支没法单测**——而对当前
//   手册语料它又恰好走不到（最长 token 64 字符 < 阈值 ~87），于是成了"没人验证过、
//   也没人会发现的代码"。本项目纪律：没有实测覆盖的防御分支 = 未验证的断言，
//   而"手册改版"正是它存在的理由，一旦那时它坏了，用户看到的是气泡撑破屏幕。
//   折行算法本身与 GDI 无关：宽度只是一个回调。抽出来之后
//     · Editor 侧只剩"取 HDC + 字体，喂一个 measure"；
//     · 算法侧可以喂等宽量尺，把边界值、超长词、以及下面的不变量全部钉进测试。
//
// 【不变量】（tests/test_balloon_wrap.cpp 逐条断言）
//   1. 任何一行的像素宽都不超过 maxPx —— **除非单个字符本身就宽于 maxPx**（无解，
//      那种情况下也只能让它单独成行）。
//   2. 不丢字符、不重排：把结果里的换行与空格全部去掉，与原文去掉空格后逐字符相等。
//      （硬切会在词**内部**断行，所以不能拿"按空格拼回"当判据——那是错的。）
//   3. 结果里没有空行（行尾不会多出一个 "\r\n"）。
//
// 【口径】只把半角空格 L' ' 当词分隔符，且**吃掉**词间的连续空格（与批次 103 一致；
//   制表符/全角空格不拆）。所以前导/尾随/重复空格都不会出现在结果里。

#include <cstddef>
#include <string>

namespace xfs {
namespace balloon {

// 量宽回调：返回 s[0,n) 在气泡字体下的像素宽；量不出来返回 0（调用方口径）。
// 用函数指针 + ctx 而不是 std::function：本函数会被热路径反复调用，且纯函数模块
// 不该引入任何分配/虚派发的可能。
using MeasureFn = int (*)(void* ctx, const wchar_t* s, std::size_t n);

// 按像素折行；行边界用显式 L"\r\n"（气泡**不**依赖控件二次折行，见 Editor.cpp）。
// maxPx <= 0 或 measure 为空时原样返回 text。
std::wstring WrapByWidth(const std::wstring& text, int maxPx,
                         MeasureFn measure, void* ctx);

} // namespace balloon
} // namespace xfs
