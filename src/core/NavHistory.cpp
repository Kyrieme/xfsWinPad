// NavHistory.cpp — 见头文件顶部的模型说明。这里只有游标算术，没有别的。
#include "NavHistory.h"

namespace xfs {

void NavHistory::Push(const NavPoint& p) {
    if (!points_.empty()) {
        // 与"当前项"相同 ⇒ 空操作（跳转前后各记一次时会走到这里）。
        if (points_[cursor_].SameAs(p)) return;
        // 新的分支取代旧的前进链：丢掉当前项之后的全部。
        points_.resize(cursor_ + 1);
    }
    points_.push_back(p);
    if (points_.size() > kMax) {
        points_.erase(points_.begin());   // 丢最早的
    }
    cursor_ = points_.size() - 1;
}

bool NavHistory::CanBack() const {
    return !points_.empty() && cursor_ > 0;
}

bool NavHistory::CanForward() const {
    return !points_.empty() && cursor_ + 1 < points_.size();
}

bool NavHistory::Back(NavPoint* out) {
    if (!CanBack()) return false;
    --cursor_;
    if (out) *out = points_[cursor_];
    return true;
}

bool NavHistory::Forward(NavPoint* out) {
    if (!CanForward()) return false;
    ++cursor_;
    if (out) *out = points_[cursor_];
    return true;
}

void NavHistory::Clear() {
    points_.clear();
    cursor_ = 0;
}

} // namespace xfs
