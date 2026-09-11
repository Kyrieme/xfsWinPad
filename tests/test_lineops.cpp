// test_lineops.cpp — 批次 29 行变换纯函数单测。
// RemoveEmptyLines / ReverseLines / ToggleComment / SplitLines+JoinLines。
#include <cstdio>
#include <string>

#include "../src/editor/LineOps.h"

using namespace xfs;
using namespace LineOps;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void RunSplitJoin() {
    std::vector<std::string> lines;
    bool ends = false;
    // \r\n / \n / \r 混合都拆
    SplitLines("a\r\nb\nc\rd", &lines, &ends);
    CHECK(lines.size() == 4);
    CHECK(lines[0] == "a" && lines[1] == "b" && lines[2] == "c" && lines[3] == "d");
    CHECK(!ends);
    CHECK(JoinLines(lines, "\r\n", false) == "a\r\nb\r\nc\r\nd");
    // 尾 EOL 保持
    SplitLines("x\r\n", &lines, &ends);
    CHECK(lines.size() == 1 && lines[0] == "x" && ends);
    CHECK(JoinLines(lines, "\r\n", true) == "x\r\n");
    // 空串
    SplitLines("", &lines, &ends);
    CHECK(lines.empty() && !ends);
    CHECK(JoinLines({}, "\n", true).empty());
    // 空行保留
    SplitLines("a\n\nb\n", &lines, &ends);
    CHECK(lines.size() == 3 && lines[1].empty() && ends);
}

static void RunRemoveEmpty() {
    // CRLF：空白行（空格/tab）也删
    CHECK(RemoveEmptyLines("a \r\n\r\n   \tb\r\n \r\nc\r\n", "\r\n")
          == "a \r\n   \tb\r\nc\r\n");
    // LF
    CHECK(RemoveEmptyLines("a\n\nb\n", "\n") == "a\nb\n");
    // 全空 → 空串
    CHECK(RemoveEmptyLines("\n \n\t\n", "\n").empty());
    // 无尾 EOL
    CHECK(RemoveEmptyLines("a\n\nb", "\n") == "a\nb");
    // 单行原文
    CHECK(RemoveEmptyLines("only", "\r\n") == "only");
}

static void RunReverse() {
    CHECK(ReverseLines("1\r\n2\r\n3\r\n", "\r\n") == "3\r\n2\r\n1\r\n");
    CHECK(ReverseLines("1\n2\n3", "\n") == "3\n2\n1");
    CHECK(ReverseLines("solo", "\r\n") == "solo");
    CHECK(ReverseLines("a\n\nb\n", "\n") == "b\n\na\n");
}

static void RunToggleComment() {
    bool commented = false;
    // 加注释：最小缩进对齐
    CHECK(ToggleComment("int a;\n  int b;\n", "//", "\n", &commented)
          == "//int a;\n  //int b;\n");
    CHECK(commented);
    // 取消：prefix + 紧随空格一起去掉
    CHECK(ToggleComment("//int a;\n  //int b;\n", "//", "\n", &commented)
          == "int a;\n  int b;\n");
    CHECK(!commented);
    // "// " 形式（python "# " / sql "-- "）
    CHECK(ToggleComment("x = 1\n", "# ", "\n", &commented) == "# x = 1\n");
    CHECK(commented);
    CHECK(ToggleComment("# x = 1\n", "# ", "\n", &commented) == "x = 1\n");
    CHECK(!commented);
    // 混合：一行已注释一行没 → 再加（判定为非全注释）
    CHECK(ToggleComment("//a\nb\n", "//", "\n", &commented) == "//a\n//b\n");
    CHECK(commented);
    // 空行不动
    CHECK(ToggleComment("\ncode\n", "//", "\n", &commented)
          == "\n//code\n");
    // 全空行 → 不动作（无目标）
    CHECK(ToggleComment("  \n\t\n", "//", "\n", &commented) == "  \n\t\n");
    CHECK(!commented);
    // 取消后空格误伤保护：代码本身以 // 开头但语义是 URL？——按规则去一个
    // prefix+一个空格，已验证上面。无尾 EOL 保持
    CHECK(ToggleComment("//a\n//b", "//", "\n", &commented) == "a\nb");
    CHECK(!commented);
}

int main() {
    printf("== test_lineops ==\n");
    RunSplitJoin();
    RunRemoveEmpty();
    RunReverse();
    RunToggleComment();
    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
