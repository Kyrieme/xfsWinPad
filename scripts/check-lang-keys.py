#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""语言键守卫：确认代码里引用的每个语言键，在语言文件里真的存在。

为什么需要它：
    Tr() 解析不到 id 时会**原样返回 id**（见 src/core/I18n.h，这是故意的 ——
    缺键在开发期一眼可见）。但到了用户手里，那就变成界面上直接显示
    "panel.compile.title" 这样的字符串。键名打错、忘了加、或者只加进了别的
    语言文件而漏了 en.json —— 编译器对这些都不会有任何意见。

和 tests/test_i18n.cpp 第 7 节的分工（刻意不重叠）：
    * test_i18n 第 7 节 —— 查**五份语言文件之间**键集合是否一致，走的是应用
      自己的 JsonLite 解析器，验证"运行时真的读得进来"。
    * 本脚本 —— 查**代码引用的 id 与语言文件**是否对得上。静态扫源码，不需要
      构建，报错时能直接给出是哪个文件、哪一行引用的。

规则：
    R1  src/ 下 Tr("id") / Fmt("id") 引用的 id，以及数据表里的字面量 `{L"id", ...}`，
        都必须存在于 en.json。

        ⚠️ 覆盖面边界（诚实说明，别当成"全查过了"）：
           * 只覆盖**字面量**。形如 `Tr(entry.key)` / `Tr(("theme.f." + name).c_str())`
             的**动态拼接**静态判不出来，不在覆盖范围内（这类键有 theme.f.* / theme.d.*）。
           * 表项那一路靠"表项形态 + 全小写点分形态 + 语言键前缀"三重过滤收窄，
             是为了不误报（只用前两条会误报 137 处）。代价是**更宽松的写法会漏**，
             例如 `{ L"Key.With.Uppercase", ... }` 不会被扫到。
    R2  resources/lang/ 下每个 *.json 都必须能解析，且顶层是对象。
    R3  语言文件集合必须与语言下拉列表（PreferencesDialog.cpp 的 kUiLangs[]）一致。
    R4  kUiLangCount 必须等于 kUiLangs[] 的实际项数（它是手工维护的计数）。
    R5  每个语言文件都必须在 src/CMakeLists.txt 里有"拷贝到 exe 旁"的规则。
    （仅提示）en.json 里存在、但没有任何代码引用的键。可能是历史遗留，也可能是
      "键加了但忘了用"，两种情况都不构成缺陷，列出来供人工判断，不影响退出码。

    R3/R4/R5 的共同点：漏掉任何一处的后果都是**静默**的 ——
      有文件没进下拉 ⇒ 用户看不到该语言；
      有文件没拷贝规则 ⇒ exe 旁没有该 json，I18n::Load 失败后悄悄沿用旧语言；
      加了项没改计数 ⇒ 下拉里可见，选中后不生效。

用法：
    python scripts/check-lang-keys.py            # 检查（0 = 干净 / 1 = 有违规）
    python scripts/check-lang-keys.py -v         # 同时打印未被引用的键

退出码：0 = 干净；1 = 有违规；2 = 环境问题（不在 git 仓库 / 语言文件读不到）。
"""

import bisect
import json
import os
import re
import subprocess
import sys

# Tr(L"id") / Fmt(L"id")，以及 I18n::Instance().Fmt(L"id", ...)。
# 只匹配字面量：形如 Tr(var) / Tr(ids[i]) 的动态调用匹配不到，这是刻意的 ——
# 它们本来就无法静态判定，报出来只会变成噪声。
CALL_RE = re.compile(r'(?:Tr|Fmt)\s*\(\s*L"([A-Za-z0-9_.]+)"')

# 数据表里的语言键，形如 `{L"pal.file.new", Cmd::FileNew}`（CommandPalette.cpp 的
# kCommands[]）。**只扫 Tr()/Fmt() 会漏掉它们** —— 而键名写错同样会静默显示原始 id。
#
# 三重过滤，缺一不可。两种朴素做法实测都会误报：
#   * 只用"表项形态" ⇒ 137 处误报（Settings 的配置键、语言 id、主题字段名
#     全都是 `{L"xxx", ...}` 的形状）
#   * 只用"语言键前缀" ⇒ 误报 cmd.exe（程序名）与 theme.f.（拼接用前缀）
# 三者叠加后：34 个候选、0 误报。
RE_TABLE_KEY = re.compile(r'\{\s*L"([A-Za-z0-9_.]+)"\s*,')
RE_KEY_SHAPE = re.compile(r'^[a-z][a-z0-9]*(\.[a-z0-9]+)+$')

LANG_DIR = os.path.join("resources", "lang")
BASE_LANG = "en"
SCAN_ROOT = "src"
SRC_EXT = (".cpp", ".h")

# 语言下拉列表（PreferencesDialog.cpp）
UI_LANGS_CPP = os.path.join("src", "app", "PreferencesDialog.cpp")
RE_UILANGS_BLOCK = re.compile(r"kUiLangs\s*\[\s*\]\s*=\s*\{(.*?)\};", re.S)
RE_UILANG_ENTRY = re.compile(r'\{\s*L"([A-Za-z0-9_\-]+)"\s*,')
RE_UILANG_COUNT = re.compile(r"kUiLangCount\s*=\s*(\d+)")

# 语言文件拷贝规则（src/CMakeLists.txt）
CMAKE_LISTS = os.path.join("src", "CMakeLists.txt")


def _force_utf8_stdio() -> None:
    """把标准输出/错误切到 UTF-8。

    CI（windows-latest）上 Python 的 stdout 默认跟随本地代码页（cp936 /
    cp1252 …），而本脚本会打印中文。编码失败会抛 UnicodeEncodeError 并以非零
    码退出 —— 那会把"规则通过"误报成"守卫失败"（2026-09-18 在公开仓库的 ci 上
    实际踩到过）。老解释器不支持 reconfigure 时静默跳过。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def repo_root():
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], stderr=subprocess.PIPE
        )
    except Exception as exc:  # git 不可用 / 不在仓库里
        print("无法定位仓库根目录：%s" % exc)
        return None
    return out.decode("utf-8", "replace").strip()


def load_lang(path):
    """读一个语言文件；返回 (dict, 错误信息)。"""
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        return None, "读取失败：%s" % exc
    text = raw.decode("utf-8-sig")          # 容忍 BOM
    try:
        obj = json.loads(text)
    except ValueError as exc:
        return None, "JSON 解析失败：%s" % exc
    if not isinstance(obj, dict):
        return None, "顶层不是对象（是 %s）" % type(obj).__name__
    return obj, None


def ui_langs(root):
    """读语言下拉列表。返回 (codes, declared_count, error)。

    `kUiLangCount` 是**手工维护的计数** —— 加了第 6 项却忘了改它，第 6 项就永远
    选不到（下拉里看得见、选中后不生效）。所以计数也要一起查。
    """
    path = os.path.join(root, UI_LANGS_CPP)
    try:
        with open(path, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
    except OSError as exc:
        return None, None, "读取失败：%s" % exc

    m = RE_UILANGS_BLOCK.search(text)
    if not m:
        return None, None, "找不到 kUiLangs[] 定义（写法变了？）"
    codes = RE_UILANG_ENTRY.findall(m.group(1))
    if not codes:
        return None, None, "kUiLangs[] 里一个语言项都没解析到（写法变了？）"

    c = RE_UILANG_COUNT.search(text)
    if not c:
        return codes, None, "找不到 kUiLangCount（写法变了？）"
    return codes, int(c.group(1)), None


def scan_source(root, prefixes):
    """扫 src/，返回 {id: [(相对路径, 行号), ...]}。

    两路扫描（见 CALL_RE / RE_TABLE_KEY 的注释）：
      ① Tr()/Fmt() 的字面量调用 —— 主路径，覆盖绝大多数键；
      ② 数据表里的字面量 `{L"key", ...}` —— 只走 ① 会漏掉 kCommands[] 这类表。

    注意：**整个文件一起匹配，不能按行扫**。调用点经常写成跨行的：

        I18n::Instance().Fmt(
            L"panel.compile.running",
            {proj.plnName, ...});

    CALL_RE 里的 \\s* 能跨过换行，但按行匹配就跨不过 —— 第一版按行扫，
    静默漏掉了 8 个键（含 panel.compile.* 三个）。守卫少算比不算更危险，
    因为它会让人以为"查过了没问题"。

    `prefixes` 是 en.json 里出现过的语言键首段集合，用来给第 ② 路兜底。
    """
    used = {}
    for base, _dirs, files in os.walk(os.path.join(root, SCAN_ROOT)):
        for name in files:
            if not name.endswith(SRC_EXT):
                continue
            full = os.path.join(base, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            try:
                with open(full, "rb") as fh:
                    text = fh.read().decode("utf-8", "replace")
            except OSError:
                continue

            # 每行起始偏移，用来把 match 偏移换算回行号（比逐次 count 快得多）
            line_starts = [0]
            pos = text.find("\n")
            while pos != -1:
                line_starts.append(pos + 1)
                pos = text.find("\n", pos + 1)

            def record(key, at):
                lineno = bisect.bisect_right(line_starts, at)
                used.setdefault(key, []).append((rel, lineno))

            for match in CALL_RE.finditer(text):
                record(match.group(1), match.start())

            for match in RE_TABLE_KEY.finditer(text):
                key = match.group(1)
                if not RE_KEY_SHAPE.match(key):
                    continue
                if key.split(".")[0] not in prefixes:
                    continue
                record(key, match.start())

    return used


def main(argv):
    _force_utf8_stdio()
    verbose = "-v" in argv or "--verbose" in argv

    root = repo_root()
    if root is None:
        return 2
    os.chdir(root)

    lang_dir = os.path.join(root, LANG_DIR)
    if not os.path.isdir(lang_dir):
        print("找不到语言目录：%s" % lang_dir)
        return 2

    problems = []

    # ---- R2：每个语言文件都要能解析 ----
    dicts = {}
    for name in sorted(os.listdir(lang_dir)):
        if not name.endswith(".json"):
            continue
        code = name[:-5]
        obj, err = load_lang(os.path.join(lang_dir, name))
        if err is not None:
            problems.append("R2 %s/%s 无法使用：%s" % (LANG_DIR.replace("\\", "/"), name, err))
            continue
        dicts[code] = obj

    if BASE_LANG not in dicts:
        print("找不到基准语言文件 %s/%s.json" % (LANG_DIR.replace("\\", "/"), BASE_LANG))
        return 2

    base = dicts[BASE_LANG]

    # ---- R1：代码引用的 id 必须存在 ----
    # 语言键首段（`panel.` / `pal.` / `sb.` …）来自基准字典，给表项扫描兜底用。
    prefixes = {k.split(".")[0] for k in base if "." in k}
    used = scan_source(root, prefixes)
    for key in sorted(used):
        if key not in base:
            where = used[key]
            shown = ", ".join("%s:%d" % (p, n) for p, n in where[:4])
            if len(where) > 4:
                shown += " (+%d 处)" % (len(where) - 4)
            problems.append("R1 引用了 %s.json 里没有的键 \"%s\" —— %s" % (BASE_LANG, key, shown))

    # ---- R3/R4/R5：语言文件 ↔ 下拉列表 ↔ 拷贝规则，三者必须对齐 ----
    #
    # 这三种"漏一处"的后果都是**静默**的：
    #   有文件、没进 kUiLangs[]   ⇒ 用户在下拉里看不到这个语言
    #   有文件、没拷贝规则        ⇒ exe 旁没有该 json，I18n::Load 失败、悄悄沿用旧语言
    #   加了项、没改 kUiLangCount ⇒ 下拉里看得见，选中后不生效
    codes = sorted(dicts)
    ui, ui_count, ui_err = ui_langs(root)

    if ui_err is not None:
        problems.append("R3 无法核对语言下拉列表：%s" % ui_err)
    else:
        if sorted(ui) != codes:
            only_file = sorted(set(codes) - set(ui))
            only_ui = sorted(set(ui) - set(codes))
            detail = []
            if only_file:
                detail.append("有语言文件但不在 kUiLangs[]：%s" % ", ".join(only_file))
            if only_ui:
                detail.append("在 kUiLangs[] 但没有语言文件：%s" % ", ".join(only_ui))
            problems.append("R3 %s/%s 与语言文件不一致 —— %s"
                            % (UI_LANGS_CPP.replace("\\", "/"), "kUiLangs[]", "；".join(detail)))

        if ui_count != len(ui):
            problems.append("R4 kUiLangCount = %s，但 kUiLangs[] 里有 %d 项 —— "
                            "多出的项在下拉里可见却选不中" % (ui_count, len(ui)))

    try:
        with open(os.path.join(root, CMAKE_LISTS), "rb") as fh:
            cmake_text = fh.read().decode("utf-8", "replace")
    except OSError as exc:
        problems.append("R5 无法读取 %s：%s" % (CMAKE_LISTS.replace("\\", "/"), exc))
    else:
        for code in codes:
            if ('lang/%s.json' % code) not in cmake_text.replace("\\", "/"):
                problems.append("R5 %s/%s.json 没有拷贝到 exe 旁的规则 —— "
                                "该语言在安装包里不存在，I18n::Load 会静默失败"
                                % (LANG_DIR.replace("\\", "/"), code))

    # ---- 提示：en.json 里没被任何代码引用的键 ----
    unused = sorted(k for k in base if k not in used)

    if verbose:
        print("仓库：%s" % root)
        print("语言文件：%s" % ", ".join(codes))
        print("下拉列表：%s（kUiLangCount = %s）"
              % (", ".join(ui) if ui else "<解析失败>", ui_count))
        print("基准 %s.json 键数：%d" % (BASE_LANG, len(base)))
        print("src/ 引用的键数：%d" % len(used))
        if unused:
            print("未被引用（提示，不算违规）：%d 个" % len(unused))
            for k in unused:
                print("    " + k)

    if problems:
        print("RESULT: %d 处违规" % len(problems))
        for p in problems:
            print("  " + p)
        print("\n说明：Tr() 遇到缺键会把 id 原样显示给用户，"
              "所以每个被引用的键都必须真的存在于语言文件里。")
        return 1

    print("RESULT: CLEAN —— 代码引用的 %d 个语言键全部存在" % len(used))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
