// test_findrefs.cpp - 批次 107：查找所有引用（FindSymbolReferences）
//
// 【为什么单开一个测试目标】按本项目的约定：**新增纯逻辑就新增一个目标**，让 ctest
//   的总数（33 → 34）本身成为一个可见信号 —— 折进 test_chromadiag 的话，"新测试没被
//   注册进 CMakeLists"这种失误会伪装成"100% 通过"。
//
// 这个函数要钉的是两件事，而且**都不能抄实现的结论**：
//   ① 位置成立：报出来的 (line, col, len) 拿去原文里取，必须正好是那个名字；
//   ② 该报的报、不该报的不报：整词边界 + 注释/字符串不算引用。
//
// 位置那条在测试侧**独立**再切一次行（不复用被测代码的 lines 表）：否则"位置与文本
// 同源"这条保证就退化成"两处抄了同一个错"，什么都验不出来。
#include "../src/language/Chroma3380Diagnostics.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace xfs;
using namespace xfs::chroma3380;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// 测试侧独立切行（按 \n 切、剥 \r），验证每个位置真的落在 name 上。
static bool PositionsHold(const std::string& text, const std::string& name,
                          const std::vector<SymbolRef>& v) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    lines.push_back(cur);
    for (const SymbolRef& r : v) {
        if (r.line < 1 || r.line > (int)lines.size()) return false;
        const std::string& L = lines[(std::size_t)r.line - 1];
        if (r.col < 0 || (std::size_t)r.len != name.size()) return false;
        if ((std::size_t)r.col + name.size() > L.size()) return false;
        if (L.compare((std::size_t)r.col, name.size(), name) != 0) return false;
    }
    return true;
}

// 只要行列，方便比对
struct LC { int line; int col; };
static std::vector<LC> ToLC(const std::vector<SymbolRef>& v) {
    std::vector<LC> o;
    for (const SymbolRef& r : v) o.push_back(LC{r.line, r.col});
    return o;
}

int main() {
    std::printf("== test_findrefs ==\n");

    // --- 1. 不开口的情形（宁可什么都不报）------------------------------------
    CHECK(FindSymbolReferences("MCLK;\n", "").empty());            // 空名
    CHECK(FindSymbolReferences("", "MCLK").empty());               // 空文本
    // 名字里含非标识符字符：宁可什么都不报，也不要拿一个怪串去比
    CHECK(FindSymbolReferences("M-1 = 2;\n", "M-1").empty());
    CHECK(FindSymbolReferences("a b c;\n", "a b").empty());

    // --- 2. 最基本：一处引用 --------------------------------------------------
    {
        const std::string t = "MCLK;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) {
            CHECK(v[0].line == 1);
            CHECK(v[0].col == 0);
            CHECK(v[0].len == 4);
        }
        CHECK(PositionsHold(t, "MCLK", v));
    }

    // --- 3. 整词：MCLK2 / XMCLK 都不算 MCLK -----------------------------------
    //    这条是"查找所有引用"最容易写坏的地方 —— 子串匹配会把一堆假引用塞给用户，
    //    而用户拿它判断"改这个 pin 要动多少地方"。
    {
        const std::string t = "MCLK2 MCLK XMCLK MCLK;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(PositionsHold(t, "MCLK", v));
        const std::vector<LC> got = ToLC(v);
        CHECK(got.size() == 2);
        if (got.size() == 2) {
            CHECK(got[0].line == 1 && got[0].col == 6);
            CHECK(got[1].line == 1 && got[1].col == 17);
        }
    }
    // 数字边界同样要整词：`40` 不该命中 `400`
    {
        const std::string t = "400 40 40;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "40");
        CHECK(v.size() == 2);
        if (v.size() == 2) {
            CHECK(v[0].col == 4);
            CHECK(v[1].col == 7);
        }
        CHECK(PositionsHold(t, "40", v));
    }

    // --- 4. 注释里的同名**不是**引用 ------------------------------------------
    {
        // 行注释：只有分号前那处算
        const std::string t = "MCLK;\n// MCLK\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) { CHECK(v[0].line == 1); CHECK(v[0].col == 0); }
        CHECK(PositionsHold(t, "MCLK", v));
    }
    {
        // 同一行：分号前算，`//` 之后不算
        const std::string t = "MCLK;   // use MCLK as master\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) CHECK(v[0].col == 0);
        CHECK(PositionsHold(t, "MCLK", v));
    }
    {
        // 块注释跨行：两行里的 MCLK 都不算；注释结束之后那处才算
        const std::string t = "/* MCLK\n   MCLK */\nMCLK;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) { CHECK(v[0].line == 3); CHECK(v[0].col == 0); }
        CHECK(PositionsHold(t, "MCLK", v));
    }
    {
        // 块注释在同一行内闭合：闭合之后那处要算
        const std::string t = "/* x */ MCLK;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) CHECK(v[0].col == 8);
        CHECK(PositionsHold(t, "MCLK", v));
    }
    {
        // 行首 `#` 是整行注释（.pat 的写法）；`#define` 那种在别处另有口径，
        // 这里只钉"行首 # 之后的不算引用"
        const std::string t = "  # MCLK\nMCLK;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) { CHECK(v[0].line == 2); CHECK(v[0].col == 0); }
        CHECK(PositionsHold(t, "MCLK", v));
    }

    // --- 5. 字符串里的同名**不是**引用 ----------------------------------------
    //    `"MCLK"` 是数据不是符号。抹平层把串内字符抹成空格、引号保留，
    //    所以整词匹配自然不会命中 —— 这条断言是把这个副作用钉下来。
    {
        const std::string t = "NAME = \"MCLK\";\nMCLK;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) { CHECK(v[0].line == 2); CHECK(v[0].col == 0); }
        CHECK(PositionsHold(t, "MCLK", v));
    }

    // --- 6. 大小写敏感（与 F12 / 状态栏提示 / 补全同口径）----------------------
    {
        const std::string t = "MCLK;\nmclk;\n";
        CHECK(FindSymbolReferences(t, "MCLK").size() == 1);
        CHECK(FindSymbolReferences(t, "mclk").size() == 1);
        CHECK(PositionsHold(t, "MCLK", FindSymbolReferences(t, "MCLK")));
    }

    // --- 7. 多行多命中 + 行首行尾边界 -----------------------------------------
    {
        const std::string t =
            "SET_DEC_FILE \"./pins.dec\"\n"
            "\n"
            "HEADER MCLK,WG0;\n"
            "\n"
            "FORCE_V_PPMU(MCLK, 1.0, 0.1);\n"
            "WAIT(MCLK);\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(PositionsHold(t, "MCLK", v));
        const std::vector<LC> got = ToLC(v);
        CHECK(got.size() == 3);
        if (got.size() == 3) {
            CHECK(got[0].line == 3);   // HEADER
            CHECK(got[1].line == 5);   // FORCE_V_PPMU(
            CHECK(got[2].line == 6);   // WAIT(
        }
    }
    // 行尾（名字就在 \n 之前，右边没有字符）
    {
        const std::string t = "WAIT(WG0)\nWG0";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "WG0");
        CHECK(v.size() == 2);
        CHECK(PositionsHold(t, "WG0", v));
    }
    // 下划线也是标识符字符：`UR_C0` 不该被 `UR_C` 命中，也不该被 `C0` 命中
    {
        const std::string t = "UR_C0 = 1;\n";
        CHECK(FindSymbolReferences(t, "UR_C").empty());
        CHECK(FindSymbolReferences(t, "C0").empty());
        CHECK(FindSymbolReferences(t, "UR_C0").size() == 1);
    }

    // --- 8. CRLF 与 \r 处理：列号不能在 \r 上偏一位 ---------------------------
    {
        const std::string t = "MCLK;\r\nWG0;\r\nMCLK;\r\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(PositionsHold(t, "MCLK", v));
        const std::vector<LC> got = ToLC(v);
        CHECK(got.size() == 2);
        if (got.size() == 2) {
            CHECK(got[0].line == 1 && got[0].col == 0);
            CHECK(got[1].line == 3 && got[1].col == 0);
        }
    }

    // --- 9. 一条完整扫描：任意切点下位置都必须成立（一般形式）------------------
    //    上面几条都是具体实例；这条保证"整词 + 抹平"没有漏掉的分支组合。
    {
        const std::string t =
            "PIN_LIST (B) {\n"
            "  MCLK = 40 = 1 = IO;  // MCLK 主时钟\n"
            "  WG0  = 39 = 2 = IO;\n"
            "}\n"
            "PIN_GROUP {\n"
            "  CTRL = MCLK+WG0;\n"
            "}\n";
        for (const char* nm : {"MCLK", "WG0", "CTRL", "IO", "B"}) {
            const std::string name(nm);
            const std::vector<SymbolRef> v = FindSymbolReferences(t, name);
            CHECK(PositionsHold(t, name, v));
        }
        // MCLK：声明一处 + 组里一处；注释里那处不算
        CHECK(FindSymbolReferences(t, "MCLK").size() == 2);
        CHECK(FindSymbolReferences(t, "WG0").size() == 2);
        CHECK(FindSymbolReferences(t, "CTRL").size() == 1);
    }

    // --- 10. 字节区间（结果面板双击选中的就是它）------------------------------
    //   这段换算错了是**静默**的：行号与行文本全对，只有选中的东西偏几个字节。
    //   真机探针验不到它（本沙箱窗口抢不到前台，模拟不出双击），所以钉在单测里。
    //   断言一律"拿区间回原文切片，必须正好是那个名字"—— 不抄实现的结论。
    {
        const std::string t =
            "SET_DEC_FILE \"./pins.dec\"\r\n"
            "\r\n"
            "HEADER MCLK,WG0;\r\n"
            "WAIT(MCLK);\r\n"
            "MCLK";                       // 最后一行没有行尾符
        for (const char* nm : {"MCLK", "WG0", "HEADER", "WAIT"}) {
            const std::string name(nm);
            const std::vector<SymbolRef> v = FindSymbolReferences(t, name);
            CHECK(!v.empty());
            for (const SymbolRef& r : v) {
                SymbolRefSpan sp;
                CHECK(LocateSymbolRef(t, r, sp));
                if (!LocateSymbolRef(t, r, sp)) continue;
                CHECK(sp.end > sp.start);
                CHECK(sp.end <= t.size());
                // ① 区间取出来必须正好是名字
                CHECK(sp.end - sp.start == name.size());
                CHECK(t.compare(sp.start, name.size(), name) == 0);
                // ② 行文本必须是那一行的**内容**（不含 \r\n）
                const std::string lt = t.substr(sp.lineBeg, sp.lineEnd - sp.lineBeg);
                CHECK(lt.find('\r') == std::string::npos);
                CHECK(lt.find('\n') == std::string::npos);
                CHECK(lt.find(name) != std::string::npos);
                // ③ 名字必须落在行内容之内（越界 = 两份文本不是同一份）
                CHECK(sp.start >= sp.lineBeg);
                CHECK(sp.end <= sp.lineEnd);
            }
        }
        // 最后一行（没有行尾符）也要成立：MCLK 在最后一个字节结束
        {
            const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
            CHECK(v.size() == 3);
            if (v.size() == 3) {
                SymbolRefSpan sp;
                CHECK(LocateSymbolRef(t, v[2], sp));
                CHECK(sp.end == t.size());
                CHECK(t.compare(sp.start, 4, "MCLK") == 0);
            }
        }
    }

    // --- 11. 越界必须失败，不许"夹一下" --------------------------------------
    //   名字跨到行尾之外 ⇒ text 与 SymbolRef 不是同一份文本（一个解码后、一个原始
    //   字节）。夹一下会给出一段选中别的东西的区间，比不报更糟。
    {
        const std::string t = "MCLK;\n";
        const std::vector<SymbolRef> v = FindSymbolReferences(t, "MCLK");
        CHECK(v.size() == 1);
        if (v.size() == 1) {
            SymbolRefSpan ok;
            CHECK(LocateSymbolRef(t, v[0], ok));       // 同一份文本：成立
            SymbolRef tooLong = v[0];
            tooLong.len = 99;                        // 跨出行尾
            SymbolRefSpan sp;
            CHECK(!LocateSymbolRef(t, tooLong, sp));
            SymbolRef noLine = v[0];
            noLine.line = 99;                        // 没有这一行
            CHECK(!LocateSymbolRef(t, noLine, sp));
            SymbolRef bad{v[0].line, -1, 4};         // 负列
            CHECK(!LocateSymbolRef(t, bad, sp));
            SymbolRef zero{v[0].line, 0, 0};         // 零长
            CHECK(!LocateSymbolRef(t, zero, sp));
        }
    }

    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
