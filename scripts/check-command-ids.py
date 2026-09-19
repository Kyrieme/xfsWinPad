#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""命令 ID 守卫：确认 src/core/CommandIds.h 里没有两个命令占用同一个 ID。

为什么需要它：
    `Cmd` 是**手工分配**的 enum，里面既有单点值，也有"保留区间"
    （`FileRecentFirst = 200, // 200..219`、`EncConvertFirst = 700, // 700..710`
    之类）。代码里用 `Cmd::XxxFirst + i` 去取区间内的第 i 个，所以**区间撞上单点
    也是真碰撞** —— 症状是"点 A 菜单触发 B 动作"，编译器、链接器、运行时都不会报。

    这个 bug 类真实发生过：批次 39 把编码转换区扩到 700..710，正好撞上 reload 区的
    起点 710（`ISO-8859-1 转换 == UTF-8 重载`）。98 项 + 5 个区间铺在 155 行里，
    靠人眼 review 是抓不住的。

规则：
    R1  把保留区间展开成占用集合后，任何值不得被两项同时占用。
    R0（前置）**解析器必须覆盖整个 enum 体**。凡有无法归类的行，直接判失败并打印出来 ——
        因为"解析器跟不上文件格式"会表现为**静默少算**，而少算的守卫比没有守卫更危险
        （写这个脚本时先后两版都漏解析：先漏隐式递增项，再漏区间项，两次都安静地报 CLEAN）。

用法：
    python scripts/check-command-ids.py            # 检查（0 = 干净 / 1 = 有冲突）
    python scripts/check-command-ids.py -v         # 同时打印全部保留区间

退出码：0 = 干净；1 = 有冲突或解析失败；2 = 环境问题。
"""

import os
import re
import subprocess
import sys

HEADER = os.path.join("src", "core", "CommandIds.h")

# 一行 enum 项的两种形态：
#   Name = 123,        （显式）
#   Name,              （隐式递增，接上一项 +1）
RE_EXPLICIT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+|\d+)$")
RE_IMPLICIT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)$")
# 注释里的保留区间。**必须锚定在注释开头**（真实写法一律是
#   `Name = 200,  // 200..219 reserved for ...`
# ）。不锚定的话，像 `// 故意撞 EncConvertFirst 的 700..710 区间` 这种**正文里提到**
# 区间的注释会被误读成区间声明 —— 写这个脚本时就在负控里踩到过（多报了 5 个冲突）。
RE_RANGE = re.compile(r"^\s*(\d+)\s*\.\.\s*(\d+)")
# enum 体的起止
RE_ENUM_OPEN = re.compile(r"^enum\s+\w+\s*(:\s*[\w: ]+)?\s*\{$")
RE_ENUM_CLOSE = re.compile(r"^\};$")


def _force_utf8_stdio() -> None:
    """CI（windows-latest）上 stdout 默认跟随本地代码页，本脚本会打印中文。
    编码失败会抛 UnicodeEncodeError 并以非零码退出 —— 那会把"规则通过"误报成
    "守卫失败"。老解释器不支持 reconfigure 时静默跳过。"""
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
    except Exception as exc:
        print("无法定位仓库根目录：%s" % exc)
        return None
    return out.decode("utf-8", "replace").strip()


def parse(path):
    """返回 (entries, unclassified)。

    entries: [(name, value, line_no, range_hi_or_None)]
    unclassified: [(line_no, text)] —— enum 体内无法归类的行；非空即视为解析失败。
    """
    with open(path, "rb") as fh:
        lines = fh.read().decode("utf-8", "replace").splitlines()

    entries, unclassified = [], []
    cur = None
    in_enum = False
    for lineno, raw in enumerate(lines, 1):
        # 注释与正文分开：区间只写在注释里
        code = raw.split("//", 1)[0]
        comment = raw.split("//", 1)[1] if "//" in raw else ""
        stripped = code.strip()

        if not in_enum:
            if RE_ENUM_OPEN.match(stripped):
                in_enum = True
            continue
        if RE_ENUM_CLOSE.match(stripped):
            in_enum = False
            continue
        if not stripped:            # 空行
            continue
        if not code.strip(" \t\r"):  # 纯注释行
            continue

        body = stripped.rstrip(",").strip()
        m = RE_EXPLICIT.match(body)
        if m:
            cur = int(m.group(2), 0)
            name = m.group(1)
        else:
            m = RE_IMPLICIT.match(body)
            if m and cur is not None:
                cur += 1
                name = m.group(1)
            else:
                unclassified.append((lineno, raw.strip()))
                continue

        r = RE_RANGE.search(comment)
        entries.append((name, cur, lineno, int(r.group(2)) if r else None))

    return entries, unclassified


def main(argv):
    _force_utf8_stdio()
    verbose = "-v" in argv or "--verbose" in argv

    root = repo_root()
    if root is None:
        return 2
    os.chdir(root)

    path = os.path.join(root, HEADER)
    if not os.path.isfile(path):
        print("找不到 %s" % HEADER.replace("\\", "/"))
        return 2

    entries, unclassified = parse(path)

    # ---- R0：解析器必须覆盖整个 enum 体 ----
    if unclassified:
        print("RESULT: 解析失败 —— enum 体里有 %d 行无法归类" % len(unclassified))
        for lineno, text in unclassified:
            print("  %s:%d  %s" % (HEADER.replace("\\", "/"), lineno, text))
        print("\n说明：无法归类意味着本脚本会**静默少算**，那样它报的 CLEAN 没有意义。")
        print("      请更新 parse() 以支持新的写法（别改成「跳过这一行」）。")
        return 1
    if not entries:
        print("RESULT: 解析失败 —— 一个枚举项都没读到（文件格式变了？）")
        return 1

    # ---- R1：展开保留区间，查占用冲突 ----
    occupied = {}
    for name, value, lineno, hi in entries:
        for v in range(value, (hi + 1) if hi is not None else value + 1):
            occupied.setdefault(v, []).append((name, lineno, hi is not None))

    clashes = {v: who for v, who in occupied.items() if len(who) > 1}

    ranges = [(n, v, hi, ln) for n, v, ln, hi in entries if hi is not None]

    if verbose:
        print("仓库：%s" % root)
        print("枚举项：%d（其中带保留区间的 %d）" % (len(entries), len(ranges)))
        print("占用值域：%d .. %d，共 %d 个值"
              % (min(occupied), max(occupied), len(occupied)))
        if ranges:
            print("保留区间：")
            for name, v, hi, ln in ranges:
                print("    %-22s %d..%d  (%d 个)  line %d"
                      % (name, v, hi, hi - v + 1, ln))

    if clashes:
        # 同一组参与者可能在多个连续值上重复出现 ⇒ 按"参与者集合"归并，
        # 并把值域压缩成 705-710 这种形式，否则一个 11 值的区间会刷 11 行。
        groups = {}
        for v in sorted(clashes):
            key = tuple(sorted(n for n, _, _ in clashes[v]))
            groups.setdefault(key, []).append(v)

        def compress(values):
            out, start, prev = [], values[0], values[0]
            for v in values[1:] + [None]:
                if v is not None and v == prev + 1:
                    prev = v
                    continue
                out.append(str(start) if start == prev else "%d-%d" % (start, prev))
                if v is not None:
                    start = prev = v
            return ", ".join(out)

        print("RESULT: %d 组 ID 冲突（共 %d 个值）" % (len(groups), len(clashes)))
        for key, values in groups.items():
            detail = "  ||  ".join(
                "%s (line %d%s)" % (n, ln, ", 保留区间" if is_range else "")
                for n, ln, is_range in clashes[values[0]]
            )
            print("  值 %s : %s" % (compress(values), detail))
        print("\n说明：区间是给 `Cmd::XxxFirst + i` 用的，所以**区间撞单点也是真碰撞** ——"
              "症状是点 A 菜单触发 B 动作，编译与运行都不报错。")
        return 1

    print("RESULT: CLEAN —— %d 个命令 ID 无重复占用（%d 项，含 %d 个保留区间）"
          % (len(occupied), len(entries), len(ranges)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
