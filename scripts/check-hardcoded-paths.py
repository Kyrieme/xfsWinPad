#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脚本绝对路径守卫：scripts/ 下不得把本仓库的检出位置写成字面量路径。

为什么需要这条规则
------------------
本仓的 e2e 探针要**启动** xfsWinPad.exe，早期版本把可执行文件路径写成了
`<盘符>:\\...\\<仓库目录>\\build\\bin\\Release\\xfsWinPad.exe` 这种**字面量默认值**。
字面量路径只能在一台机器上成立：

  * 换台机器 / 换个检出目录 clone，默认值指向**不存在的旧位置**。探针要么当场
    报"找不到 exe"，要么更糟 —— 那个位置上还留着**上一次构建**的旧 exe，于是
    它去测一个早已不是当前源码的东西。**一个悄悄不再对得上被测对象的验证资产，
    比没有更坏**（本仓批次 72 的教训）。
  * 在虚拟机里跑（本仓探针的常见用法）宿主盘符根本不存在：`chroma-e2e.ps1` 的
    注释就写着"宿主的 D:\\ 不是盘符（只有 VirtualBox 共享盘才是）"，一个硬编码
    `D:\\` 默认值会让整套探针在那边**完全跑不起来**。
  * `autocomplete-uia.ps1` 还硬编码了截图输出目录，在同一种换机场景下直接抛
    "目录不存在"。

同一类病本仓已经单独修过一次（`run-probe.cmd` 里硬编码的 `ROOT=D:\\...` 改成
"从脚本自身位置 `%~dp0..\\..` 推导"）。本守卫把那条结论**机器化**：覆盖整个
`scripts/`，防复发。

做法
----
默认值应当**从脚本自己的位置推导**，这样在任何检出目录、任何盘符下都对：

    param([string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot)
                                  "build\\bin\\Release\\xfsWinPad.exe"))

`$PSScriptRoot` 在**脚本 param 块的默认值里就可用**（本机 PowerShell 5.1 实测：
`-File` 调用时默认值解出的正是脚本所在目录）。

判定规则（窄而精确，零误报）
--------------------------
只揪**"指向本检出"**的字面量：一个 `<盘符>:<分隔符>` 开头、且路径段里**含有
本仓库目录名**（`os.path.basename(仓库根)`）的字符串。这条判据不碰正当代码里
的例子路径（如注释里写的 `C:\\Windows\\Temp\\x`，盘符后跟的不是本仓库名），
也不碰 PowerShell 里合法的运行时常量（`$env:APPDATA`、`$env:TEMP`）。

冻结豁免
--------
`EXEMPT` 是"文件名 -> 期望命中数"的冻结表。被豁免的文件里那处字面量是**真机
输出的样本前缀**，且只被取 `.Length` 用来和样本的字节常量对齐（拆开重算超出
本守卫职责）。**项数本身就是断言**：豁免的路径哪天真被改掉、命中数不再相等，
本守卫立刻转红 —— 不留一条"看起来还行"的死豁免。

用法：
    python scripts/check-hardcoded-paths.py          # 检查（0 干净 / 1 有违规）
    python scripts/check-hardcoded-paths.py -v       # 同时打印扫描计数

退出码：0 = 干净；1 = 有违规；2 = 环境问题（不在 git 仓库里 / git 不可用）。
"""

import os
import re
import subprocess
import sys

# 只扫自动化脚本目录：产品源码里出现"例子路径"是正常的，不该被这条规则管。
SCAN_PREFIX = "scripts/"

# 按扩展名挑文本文件（与 check-public-surface.py 同一套白名单思路）。
TEXT_SUFFIXES = (
    ".py", ".ps1", ".psm1", ".cmd", ".bat", ".sh",
)

# 单个文件超过这个大小就不扫（防御性上限，与其他守卫一致）。
MAX_SCAN_BYTES = 4 * 1024 * 1024

# 文件名 -> 期望命中数。见模块头"冻结豁免"。项数即断言，陈旧即红。
EXEMPT = {
    "scripts/craft-e2e.ps1": 1,
}


def _force_utf8_stdio() -> None:
    """把标准输出/错误切到 UTF-8（本脚本会打印中文）。

    与 check-public-surface.py / check-script-encoding.py 同样的理由：CI 上
    Python 的 stdout 默认跟随本地代码页，编码失败会抛 UnicodeEncodeError 并以
    非零码退出，把"规则通过"误报成"守卫失败"。
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


# 一个"候选绝对路径字面量"：盘符 + 分隔符，后面吃到行尾/引号/反引号为止。
# 候选拿到之后再看里面有没有本仓库目录名作为一个**完整路径段**
# （避免 `xfsPadSamples` 这种前缀撞车）——分两步写比一条带前瞻的长正则好读。
CANDIDATE = re.compile(r"[A-Za-z]:[\\/][^\r\n'\"`]*")


def build_pattern(repo_name):
    """返回 (候选正则, 仓库名路径段正则)。"""
    seg = re.compile(r"[\\/]" + re.escape(repo_name) + r"(?:[\\/]|$)")
    return CANDIDATE, seg


def check_paths(root, files, pattern):
    """返回 (违规列表, 每文件命中列表 {rel: [(行号, 字面量)]}, 扫描计数)。"""
    cand_re, seg_re = pattern
    hits = {}
    checked = 0
    for f in files:
        if not f.startswith(SCAN_PREFIX):
            continue
        if not f.lower().endswith(TEXT_SUFFIXES):
            continue
        p = os.path.join(root, f.replace("/", os.sep))
        try:
            if os.path.getsize(p) > MAX_SCAN_BYTES:
                continue
            with open(p, "rb") as fh:
                raw = fh.read()
        except OSError:
            continue
        checked += 1
        text = raw.decode("utf-8", "replace")
        for m in cand_re.finditer(text):
            if seg_re.search(m.group(0)) is None:
                continue
            line_no = text.count("\n", 0, m.start()) + 1
            hits.setdefault(f, []).append((line_no, m.group(0)))

    bad = []
    for f, found in sorted(hits.items()):
        want = EXEMPT.get(f)
        if want is None:
            for line_no, lit in found:
                bad.append("%s:%d  %s" % (f, line_no, lit))
        elif len(found) != want:
            bad.append("%s 命中 %d 处，冻结豁免表写的是 %d（豁免已陈旧）"
                       % (f, len(found), want))
    for f, want in EXEMPT.items():
        if f not in hits:
            bad.append("%s 冻结豁免了 %d 处，但一处都没命中（豁免已陈旧）"
                       % (f, want))
    return bad, hits, checked


def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    try:
        root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"],
                                       stderr=subprocess.PIPE).decode().strip()
    except (OSError, subprocess.CalledProcessError) as e:
        print("ERROR: 不在 git 仓库里：%s" % e, file=sys.stderr)
        sys.exit(2)
    os.chdir(root)

    repo_name = os.path.basename(os.path.normpath(root))
    files = git_ls_files()
    bad, hits, checked = check_paths(root, files, build_pattern(repo_name))

    if verbose:
        print("仓库：%s（目录名 %s）" % (root, repo_name))
        print("跟踪文件：%d" % len(files))
        print("扫描的脚本文件：%d" % checked)
        print("命中文件：%d  豁免：%d" % (len(hits), len(EXEMPT)))

    if bad:
        print("RESULT: %d 处违规" % len(bad))
        for b in bad:
            print("  " + b)
        print("\n说明：把本仓库检出位置写成字面量，只在一台机器上成立；"
              "换机/换目录/虚拟机里会")
        print("      指向不存在或陈旧的可执行文件。请改为从脚本自身位置推导，例如：")
        print("      param([string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "
              "\"build\\bin\\Release\\xfsWinPad.exe\"))")
        return 1

    print("RESULT: CLEAN —— scripts/ 下没有指向本检出的字面量绝对路径")
    return 0


if __name__ == "__main__":
    sys.exit(main())
