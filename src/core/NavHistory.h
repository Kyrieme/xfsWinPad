#pragma once
// xfsWinPad - 导航历史（"上一处 / 下一处"的后端）
//
// 这是一个**纯逻辑**模块：不碰 Win32、不碰 Scintilla、不做 IO。宿主负责把
// "编辑器里的一个位置"翻译成 NavPoint，也负责把它还原回去。这样它就能被单测
// 直接钉住，而不必起窗口。
//
// 【模型：列表末尾永远是"我现在在哪"】与浏览器 / IDE 的 Back-Forward 同构。
//   宿主在**每次跳转前**记一次、**跳转后**再记一次。因为 Push 对"与当前项
//   相同"的点是空操作，"跳转前"那一次在常见情形下自动退化成 no-op；而用户在
//   跳转之前手动移动过的那一段不会丢（它被"跳转前"那一次记下来了）。
//
// 【为什么不监听光标变化】打字、按方向键都是光标变化，逐次记录会把历史淹掉。
//   只记**显式导航**：跳行、转到定义、诊断面板双击跳转。
//
// 【为什么路径存字符串而不是 Document*】历史要能活过文档关闭 —— 用户可能
//   后退到一个已经关掉的文档，那时应当把它重新打开（宿主用既有的
//   ActivateOrOpenDocument，与转到定义同一条路径）。

#include <cstddef>
#include <string>
#include <vector>

namespace xfs {

// 一个导航点：哪份文档（完整路径）+ 文档内的字节位置。
//   pos 用**字节位置**而不是行列：行列在文件被外部改动后会失准，而字节位置
//   可以夹到合法区间后仍然落在附近；口径与 SCI_GETCURRENTPOS / SCI_GOTOPOS 一致。
struct NavPoint {
    std::wstring path;
    long long pos = 0;

    bool SameAs(const NavPoint& o) const {
        return pos == o.pos && path == o.path;
    }
};

class NavHistory {
public:
    // 上限。够用且不会无界增长；超出后丢**最早**的。
    static constexpr std::size_t kMax = 64;

    // 记一个点。
    //   · 与"当前项"相同 ⇒ 什么都不做。**这条正是"跳转前后各记一次"能成立的
    //     原因**：不这样的话每次跳转都会多塞一条重复项，Back 一次只退半步。
    //   · 否则先丢掉当前项之后的全部（新的分支取代旧的前进链），再追加。
    void Push(const NavPoint& p);

    bool CanBack() const;
    bool CanForward() const;

    // 后退 / 前进一格。成功时把目标写进 *out，并把"当前项"移到那一格上。
    // out 允许为 nullptr（只想挪游标时）。
    bool Back(NavPoint* out);
    bool Forward(NavPoint* out);

    std::size_t Size() const { return points_.size(); }
    std::size_t Cursor() const { return cursor_; }   // Size()==0 时无意义
    void Clear();

private:
    std::vector<NavPoint> points_;
    std::size_t cursor_ = 0;   // "当前项"下标；points_ 非空时恒 < Size()
};

} // namespace xfs
