// test_shortcut.cpp - ShortcutTable unit tests: defaults, combo-string codec,
// override/remove semantics, conflict detection, JSON round trip, corruption.
#include "../src/shortcut/ShortcutTable.h"
#include "../src/core/CommandIds.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

static void WriteFileRaw(const std::wstring& path, const char* data) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f) { fwrite(data, 1, strlen(data), f); fclose(f); }
}

int main() {
    // --- 默认表与历史 BuildAccelerators 对齐 --------------------------------
    {
        ShortcutTable t;
        ShortcutInfo n = t.Get(Cmd::FileNew);
        CHECK(n.Valid() && n.vk == 'N' && n.ctrl && !n.alt && !n.shift);
        ShortcutInfo pal = t.Get(Cmd::PaletteShow);
        CHECK(pal.Valid() && pal.vk == 'P' && pal.ctrl && pal.shift);
        ShortcutInfo tab = t.Get(6001);
        CHECK(tab.Valid() && tab.vk == VK_TAB && tab.ctrl);
        // Scintilla 自处理键：表内有条目（菜单尾注/tooltip 单一来源），
        // 但 DisplayOnly 标记使其不进 ACCEL 构建列表
        CHECK(t.Get(Cmd::EditUndo).vk == 'Z');
        CHECK(ShortcutTable::DisplayOnly(Cmd::EditUndo));
        bool undoInAccel = false;
        for (const auto& kv : t.All())
            if (kv.first == (unsigned)Cmd::EditUndo) { undoInAccel = true; break; }
        CHECK(!undoInAccel);
        CHECK(t.Get(Cmd::Preferences).vk == 0);       // 未登记 = 无快捷键
        // 批次 107：查找所有引用 = Shift+F12。
        // 【为什么这条断言在单测里】真机探针验不了它：本沙箱里桌面被别的进程的
        //   全屏窗口占着，SetForegroundWindow 抢不回前台（实测前台是
        //   Chrome_WidgetWin_1），SendInput 送去的按键进了那个窗口。而"表里有没
        //   有这一条"是确定性的事实 —— 宁可把它钉在每次 CI 都跑的地方，也不写
        //   一条没人跑过的真机断言（硬约束 21）。
        ShortcutInfo refs = t.Get(Cmd::FindAllReferences);
        CHECK(refs.Valid() && refs.vk == VK_F12 && refs.shift &&
              !refs.ctrl && !refs.alt);
        // 与 F12（转到定义）必须**不同**：两条占同一组合时，TranslateAccelerator
        // 只会命中其中一个，另一个从此永远打不到 —— 编译、链接、运行都不报错。
        ShortcutInfo gd = t.Get(Cmd::GotoDefinition);
        CHECK(gd.Valid() && gd.vk == VK_F12 && !gd.shift);
        CHECK(!refs.SameAs(gd));
    }
    // --- 组合串编解码 round trip ---------------------------------------------
    {
        CHECK(ShortcutInfo::FromString(L"Ctrl+Shift+F3").SameAs(
            [] { ShortcutInfo i; i.vk = VK_F3; i.ctrl = i.shift = true; return i; }()));
        CHECK(ShortcutInfo::FromString(L"F5").vk == VK_F5);
        CHECK(ShortcutInfo::FromString(L"Ctrl+Num0").vk == VK_NUMPAD0);
        CHECK(ShortcutInfo::FromString(L"ctrl+alt+t").vk == 'T');   // 大小写宽容
        CHECK(ShortcutInfo::FromString(L"Shift+8").vk == '8');
        // 非法输入 → 无效
        CHECK(!ShortcutInfo::FromString(L"Ctrl+").Valid());
        CHECK(!ShortcutInfo::FromString(L"Ctrl+Shift").Valid());    // 只有修饰键
        CHECK(!ShortcutInfo::FromString(L"F1+F2").Valid());         // 双主键
        CHECK(!ShortcutInfo::FromString(L"NotAKey").Valid());
        // ToString
        ShortcutInfo i; i.vk = VK_F3; i.ctrl = i.shift = true;
        CHECK(i.ToString() == L"Ctrl+Shift+F3");
        i = {}; i.vk = VK_NUMPAD0; i.ctrl = true;
        CHECK(i.ToString() == L"Ctrl+Num0");
        CHECK(ShortcutInfo{}.ToString().empty());
    }
    // --- 覆盖 / 删除 / 还原语义 ------------------------------------------------
    {
        ShortcutTable t;
        // 批次 29 起 Ctrl+Q 默认绑 EditToggleComment——换用仍空闲的 Ctrl+K
        ShortcutInfo n; n.vk = 'K'; n.ctrl = true;
        unsigned conflict = 0;
        CHECK(t.SetShortcut(Cmd::FileNew, n, &conflict));
        CHECK(t.Get(Cmd::FileNew).vk == 'K');
        // 冲突检测：FileSave 已占 Ctrl+S
        ShortcutInfo s; s.vk = 'S'; s.ctrl = true;
        CHECK(!t.SetShortcut(Cmd::FileSaveAs, s, &conflict));
        CHECK(conflict == (unsigned)Cmd::FileSave);
        // 同命令重设不冲突
        CHECK(t.SetShortcut(Cmd::FileSave, s, &conflict));
        // 显式删除：vk=0 → Get 无效，All() 不含
        ShortcutInfo del;   // vk=0
        CHECK(t.SetShortcut(Cmd::FileClose, del));
        CHECK(!t.Get(Cmd::FileClose).Valid());
        bool hasClose = false;
        for (const auto& [cmd, info] : t.All())
            if (cmd == (unsigned)Cmd::FileClose) hasClose = true;
        CHECK(!hasClose);
        // ClearOverride → 回到默认
        CHECK(t.ClearOverride(Cmd::FileClose));
        CHECK(t.Get(Cmd::FileClose).vk == 'W');
        CHECK(!t.ClearOverride(Cmd::FileClose));   // 已无覆盖
        // 显式解绑的命令仍必须出现在 Everything（映射器列表可见，便于恢复默认）
        CHECK(t.SetShortcut(Cmd::FileNew, del));
        bool fileNewVisible = false;
        for (const auto& [cmd, info] : t.Everything())
            if (cmd == (unsigned)Cmd::FileNew) fileNewVisible = true;
        CHECK(fileNewVisible);
        CHECK(t.ClearOverride(Cmd::FileNew));
    }
    // --- JSON round trip（仅覆盖项持久化） -------------------------------------
    {
        ShortcutTable t;
        ShortcutInfo n; n.vk = 'Q'; n.ctrl = true; n.alt = true;
        t.SetShortcut(Cmd::FileNew, n);
        ShortcutInfo del;
        t.SetShortcut(Cmd::FileClose, del);        // 删除项也要持久化
        std::wstring path = TempPath(L"xfs_shortcuts_rt.json");
        CHECK(t.Save(path));

        ShortcutTable r;
        CHECK(r.Load(path));
        CHECK(r.Get(Cmd::FileNew).SameAs(n));
        CHECK(!r.Get(Cmd::FileClose).Valid());     // 删除语义存活
        CHECK(r.Get(Cmd::FileSave).vk == 'S');     // 默认项未被覆盖时原样生效
        CHECK(r.OverrideCount() == 2);
        DeleteFileW(path.c_str());
    }
    // --- 缺失/损坏文件 → 默认表不受影响 -----------------------------------------
    {
        ShortcutTable t;
        CHECK(!t.Load(TempPath(L"xfs_shortcuts_nonexistent.json")));
        CHECK(t.Get(Cmd::FileNew).vk == 'N');
        CHECK(t.OverrideCount() == 0);

        std::wstring path = TempPath(L"xfs_shortcuts_bad.json");
        WriteFileRaw(path.c_str(), "{{{ not json !!!");
        CHECK(!t.Load(path));   // 无有效键值对 → false
        CHECK(t.Get(Cmd::FileNew).vk == 'N');
        DeleteFileW(path.c_str());
    }
    // --- 混合内容容错（非数字键 / futureKey 跳过） -------------------------------
    {
        std::wstring path = TempPath(L"xfs_shortcuts_mixed.json");
        WriteFileRaw(path.c_str(),
                     "{ \"100\": \"Ctrl+Q\", \"future\": {\"x\":1}, \"6001\": \"\" }");
        ShortcutTable t;
        CHECK(t.Load(path));
        CHECK(t.Get(Cmd::FileNew).vk == 'Q');
        CHECK(!t.Get(6001).Valid());               // 空串 = 删除
        DeleteFileW(path.c_str());
    }

    if (g_fail == 0) { printf("ALL SHORTCUT TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
