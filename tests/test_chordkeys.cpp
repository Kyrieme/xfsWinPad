// test_chordkeys.cpp — 批次 154：Ctrl+K,C / Ctrl+K,U 和弦判定纯函数单测。
// 只依赖 header-only 的 src/core/ChordKeys.h（零 Windows API、零链接）。
#include <cstdio>

#include "../src/core/ChordKeys.h"

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main() {
    printf("== test_chordkeys ==\n");

    // --- 非前缀态（pending=false）---
    // 仅 Ctrl+K → Arm（进入前缀）
    CHECK(DecideChord('K', true, false, false, false) == ChordAction::Arm);
    // 前缀态未建立时按 Ctrl+C / Ctrl+U → 不是和弦，放行（None）
    CHECK(DecideChord('C', true, false, false, false) == ChordAction::None);
    CHECK(DecideChord('U', true, false, false, false) == ChordAction::None);
    // 无 Ctrl 的 K / 带 alt、shift 的 K → 非纯 Ctrl，放行
    CHECK(DecideChord('K', false, false, false, false) == ChordAction::None);
    CHECK(DecideChord('K', true, true, false, false) == ChordAction::None);
    CHECK(DecideChord('K', true, false, true, false) == ChordAction::None);

    // --- 前缀态（pending=true）---
    // 仅 Ctrl+C → Comment；仅 Ctrl+U → Uncomment
    CHECK(DecideChord('C', true, false, false, true) == ChordAction::Comment);
    CHECK(DecideChord('U', true, false, false, true) == ChordAction::Uncomment);
    // 带修饰的 C / U → 非纯 Ctrl，放行（不吞掉 Ctrl+Shift+U 等原生键）
    CHECK(DecideChord('C', true, true, false, true) == ChordAction::None);
    CHECK(DecideChord('C', true, false, true, true) == ChordAction::None);
    CHECK(DecideChord('U', true, true, false, true) == ChordAction::None);
    // 任意其它键 → 放弃前缀，放行
    CHECK(DecideChord('X', true, false, false, true) == ChordAction::None);
    CHECK(DecideChord('K', true, false, false, true) == ChordAction::None);
    // 无修饰的普通键 → 放弃前缀，放行
    CHECK(DecideChord('A', false, false, false, true) == ChordAction::None);

    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
