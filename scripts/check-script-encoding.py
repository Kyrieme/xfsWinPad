#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脚本编码守卫：带非 ASCII 字节的 PowerShell 脚本必须带 UTF-8 BOM。

为什么需要这条规则
------------------
PowerShell 5.1 解析**没有 BOM** 的 .ps1 时，用的是**系统 ANSI 代码页**
（本机实测 acp = gb2312），而不是 UTF-8。实测证据 —— 同一份文件，只差一个 BOM，
文件里写的都是 UTF-8 的 `'中文'`（6 个字节）：

    无 BOM -> 解出 3 个字符，码位 U+6D93 U+E15F U+6783   （乱码）
    有 BOM -> 解出 2 个字符，码位 U+4E2D U+6587          （正确）

后果不止"注释显示成乱码"。GBK 解码是**按字节对**消费的，所以当前导字节正好落在
行尾时，它会**把换行符当成后半个字节吞掉** —— 下一行代码被并进注释里，于是花括号
失配，而报错出现在**一个根本不含花括号的行**上。批次 101 实测：解析器报
`unexpected }` 指向一行 `$smpDir = Join-Path $stubDir "samples"`，那一行没有花括号。
当时只有注释从 ASCII 改成中文、再改回 ASCII，解析结果就从 0 -> 1 -> 0 地翻转，
代码一字未动。

这类错误还有个恶劣性质：**它随字节奇偶而变**。同一份文件里有的中文注释会炸、
有的不会；`chroma-e2e.ps1` 与 `ate-langs-e2e.ps1` 带着中文一直照跑。
所以"上次没炸"完全不能当作保证 —— 只能靠这条规则。

两种安全的写法
--------------
  1. 整份文件只用 ASCII（每个字节 < 0x80）；或
  2. 文件以 UTF-8 BOM（EF BB BF）开头 —— 此时 PowerShell 按 UTF-8 解码。
本脚本强制：**只要出现任何非 ASCII 字节，就必须带 BOM**。
纯 ASCII 文件加不加 BOM 都不管。

用法：
    python scripts/check-script-encoding.py            # 检查（0 干净 / 1 有违规）
    python scripts/check-script-encoding.py -v         # 同时打印通过项统计

退出码：0 = 干净；1 = 有违规；2 = 环境问题（不在 git 仓库里 / git 不可用）。
"""

import os
import subprocess
import sys

# 只查 PowerShell 脚本：Python/CMake 等默认就是 UTF-8，没有这个问题。
SUFFIXES = (".ps1", ".psm1")

UTF8_BOM = b"\xef\xbb\xbf"

# 单个文件超过这个大小就不扫（防御性上限，和 check-public-surface.py 一致）
MAX_SCAN_BYTES = 4 * 1024 * 1024


def _force_utf8_stdio() -> None:
    """把标准输出/错误切到 UTF-8。

    与 check-public-surface.py 同样的理由：CI 上 Python 的 stdout 默认跟随本地
    代码页，而本脚本会打印中文。编码失败会抛 UnicodeEncodeError 并以非零码退出，
    那会把"规则通过"误报成"守卫失败"。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
        except (AttributeError, ValueError, OSError):
            pass


_force_utf8_stdio()


def git_ls_files():
    """列出被跟踪的文件（相对路径，/ 分隔）。"""
    try:
        out = subprocess.check_output(
            ["git", "ls-files", "-z"], stderr=subprocess.PIPE
        )
    except (OSError, subprocess.CalledProcessError) as e:
        print("ERROR: 无法执行 git ls-files（请在 git 仓库内运行）：%s" % e,
              file=sys.stderr)
        sys.exit(2)
    files = [p.decode("utf-8", "surrogateescape")
             for p in out.split(b"\0") if p]
    return [f.replace("\\", "/") for f in files]


def check_encoding(root, files):
    """返回 (违规列表, 检查计数)。"""
    bad = []
    checked = 0
    for f in files:
        low = f.lower()
        if not low.endswith(SUFFIXES):
            continue
        if f.startswith("third_party/"):
            continue                      # 上游文件原样保留，不改
        p = os.path.join(root, f.replace("/", os.sep))
        try:
            if os.path.getsize(p) > MAX_SCAN_BYTES:
                continue
            with open(p, "rb") as fh:
                raw = fh.read()
        except OSError:
            continue

        checked += 1
        has_bom = raw.startswith(UTF8_BOM)
        # 跳过 BOM 本身再找非 ASCII，这样"只有 BOM"的文件不会被误判。
        body = raw[len(UTF8_BOM):] if has_bom else raw
        first_non_ascii = None
        for i, b in enumerate(body):
            if b >= 0x80:
                first_non_ascii = i
                break
        if first_non_ascii is None:
            continue                      # 纯 ASCII，安全
        if has_bom:
            continue                      # 有 BOM，PowerShell 按 UTF-8 解码，安全

        # 报出所在行号，方便定位
        line_no = body.count(b"\n", 0, first_non_ascii) + 1
        bad.append("非 ASCII 且无 UTF-8 BOM：%s:%d" % (f, line_no))
    return bad, checked


def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    try:
        root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"],
                                       stderr=subprocess.PIPE).decode().strip()
    except (OSError, subprocess.CalledProcessError) as e:
        print("ERROR: 不在 git 仓库里：%s" % e, file=sys.stderr)
        sys.exit(2)
    os.chdir(root)

    files = git_ls_files()
    bad, checked = check_encoding(root, files)

    if verbose:
        print("仓库：%s" % root)
        print("跟踪文件：%d" % len(files))
        print("检查的 PowerShell 脚本：%d" % checked)

    if bad:
        print("RESULT: %d 处违规" % len(bad))
        for b in bad:
            print("  " + b)
        print("\n说明：PowerShell 5.1 用系统 ANSI 代码页解析无 BOM 的 .ps1，"
              "非 ASCII 注释可能吞掉换行、")
        print("      并报出一个行号对不上的幽灵语法错误。"
              "请给该文件加 UTF-8 BOM，或把非 ASCII 内容改成 ASCII。")
        return 1

    print("RESULT: CLEAN —— 所有含非 ASCII 的脚本都带 UTF-8 BOM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
