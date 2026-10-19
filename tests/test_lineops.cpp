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

// 批次 154：严格方向 —— 只加（Ctrl+K,C）
static void RunCommentLines() {
    // 基本：每行按自身缩进插入
    CHECK(CommentLines("int a;\n  int b;\n", "//", "\n")
          == "//int a;\n  //int b;\n");
    // 空行/全空白行跳过（用户要求：多行选中的空行跳过）
    CHECK(CommentLines("code\n\n  \n\t\nmore\n", "//", "\n")
          == "//code\n\n  \n\t\n//more\n");
    // 已是注释行不动（严格"只加"，不像 toggle 会整体取消）
    CHECK(CommentLines("//a\nb\n", "//", "\n") == "//a\n//b\n");
    // 全部已注释 → 原样不动
    CHECK(CommentLines("//a\n//b\n", "//", "\n") == "//a\n//b\n");
    // 全空行 → 原样不动
    CHECK(CommentLines("\n  \n", "//", "\n") == "\n  \n");
    // "# " 形式（python / sql）
    CHECK(CommentLines("x = 1\n", "# ", "\n") == "# x = 1\n");
    // 无尾 EOL 保持
    CHECK(CommentLines("a\nb", "//", "\n") == "//a\n//b");
}

// 批次 154：严格方向 —— 只删（Ctrl+K,U）
static void RunUncommentLines() {
    // 基本：去掉 prefix 及其后紧随的一个空格
    CHECK(UncommentLines("//int a;\n  //int b;\n", "//", "\n")
          == "int a;\n  int b;\n");
    // 无 prefix 行不动（严格"只删"，不像 toggle 会整体加）
    CHECK(UncommentLines("//a\nb\n", "//", "\n") == "a\nb\n");
    // 空行跳过
    CHECK(UncommentLines("//a\n\n  //b\n", "//", "\n") == "a\n\n  b\n");
    // "# " 形式
    CHECK(UncommentLines("# x = 1\n", "# ", "\n") == "x = 1\n");
    // 全部无 prefix → 原样不动
    CHECK(UncommentLines("a\nb\n", "//", "\n") == "a\nb\n");
    // 无尾 EOL 保持
    CHECK(UncommentLines("//a\n//b", "//", "\n") == "a\nb");
}

int main() {
    printf("== test_lineops ==\n");
    RunSplitJoin();
    RunRemoveEmpty();
    RunReverse();
    RunToggleComment();
    RunCommentLines();
    RunUncommentLines();
    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
