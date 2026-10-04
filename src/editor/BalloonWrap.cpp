// BalloonWrap.cpp - 悬停气泡折行算法（批次 118 从 Editor.cpp 逐字搬出）
//
// 这里**只**做折行，不碰窗口、不碰 GDI、不碰 Scintilla。算法本体与批次 103 完全
// 一致（包括"吃掉词间空格"、"只认半角空格"、"行装箱用严格大于"这些口径），
// 唯一的变化是宽度来源从 HDC 换成了调用方给的 measure 回调 —— 正是这一点让它
// 从"没法测"变成"能测"，也是本批次存在的唯一理由。
//
// 搬家后靠两件事证明"没搬坏"：
//   · tests/test_balloon_wrap.cpp 里留了一份批次 103 算法的**冻结副本**（LegacyWrap），
//     用同一把量尺对撞（重构等价性）；
//   · 同一测试里另有一组**从需求推出**的判据（不变量 + 手算期望值），
//     不依赖任何一份实现 —— 因为"两份实现一致"不等于"对"。

#include "BalloonWrap.h"

namespace xfs {
namespace balloon {

std::wstring WrapByWidth(const std::wstring& text, int maxPx,
                         MeasureFn measure, void* ctx)
{
    if (maxPx <= 0 || !measure) return text;

    // 统一的量宽入口：拿"指针 + 长度"而不是 std::wstring，省掉每次探测的子串分配。
    auto width = [&](const wchar_t* s, std::size_t n) -> int {
        return measure(ctx, s, n);
    };

    std::wstring out, line;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i;
        while (j < text.size() && text[j] != L' ') ++j;
        std::wstring word = text.substr(i, j - i);
        i = j;
        while (i < text.size() && text[i] == L' ') ++i;   // 吃掉词间空格

        // 整段没有空格的超长"词"（例如一串参数）：按像素硬切，别让它撑破屏幕。
        // ★ 这条分支对当前手册语料走不到（见头文件说明），但它是"手册改版"的保险，
        //   因此必须由测试真的走一遍 —— 不要因为它没被真实数据触发就当它是对的。
        while (width(word.data(), word.size()) > maxPx && word.size() > 1) {
            std::size_t k = word.size();
            while (k > 1 && width(word.data(), k) > maxPx) --k;
            if (!line.empty()) { out += line; out += L"\r\n"; line.clear(); }
            out += word.substr(0, k);
            out += L"\r\n";
            word = word.substr(k);
        }
        if (word.empty()) continue;

        if (line.empty()) {
            line = word;
        } else {
            std::wstring probe = line + L" " + word;
            if (width(probe.data(), probe.size()) > maxPx) {
                out += line;
                out += L"\r\n";
                line = word;
            } else {
                line += L" ";
                line += word;
            }
        }
    }
    out += line;
    return out;
}

} // namespace balloon
} // namespace xfs
