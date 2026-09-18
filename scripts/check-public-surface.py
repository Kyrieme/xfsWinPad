#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""公开面守卫：确认只有允许公开的内容进入版本控制。

和 scripts/check-sensitive-words.py 的分工：
  - check-sensitive-words.py  查**内容**（敏感词），词表是私密的 → 在 CI 上只能跳过；
  - 本脚本查**结构**（哪些文件可以公开），规则本身就是公开的 → 可以常驻 CI。

规则（全部针对 `git ls-files` 可见的文件，即会随公开仓库发布的东西）：
  R1 不得跟踪私密目录（temp/ dist/ build/ .workbuddy/ .vs/ 等）。
  R2 不得跟踪私密文档（TODO.md / HANDOFF.md / LESSONS.md / PROJECT.md / ARCHITECTURE.md）。
  R3 跟踪的 .md 只允许白名单三个 + third_party/ 下的上游文件。
  R4 根目录 .txt 只允许 CMakeLists.txt（需求文档等一律不入库）。
  R5 公开文件不得在正文里指向上游不存在的私密文档（写给自己看的指针，公开读者无从打开）。
  R6 公开文件不得写出私密工作区 temp/ 下的**具体文件名**（提及目录本身可以）。

用法：
    python scripts/check-public-surface.py            # 检查（退出码 0 干净 / 1 有违规）
    python scripts/check-public-surface.py -v         # 同时打印通过项统计

退出码：0 = 干净；1 = 有违规；2 = 环境问题（不在 git 仓库里 / git 不可用）。
"""

import os
import re
import subprocess
import sys

# ---------------------------------------------------------------- 规则数据

# R1：这些前缀下的文件永远不该被跟踪
DENY_PREFIXES = (
    "temp/",
    "dist/",
    "build/",
    ".workbuddy/",
    ".vs/",
)

# R2：这些文件名（不区分大小写，比较 basename）不该被跟踪
DENY_BASENAMES = {
    "todo.md",
    "handoff.md",
    "lessons.md",
    "project.md",
    "architecture.md",
}

# R3：允许公开的 .md（其余 .md 一律视为私密）
MD_ALLOWLIST = {
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "src/plugin/sdk/PLUGIN_DEV_GUIDE.md",
}
# third_party/ 下是上游自带的说明文件，原样保留
MD_ALLOWED_PREFIXES = ("third_party/",)

# R4：根目录允许的 .txt
ROOT_TXT_ALLOWLIST = {"CMakeLists.txt"}

# R5：正文里不得出现的私密文档指针；例外文件（必须提到它们才能工作）
PRIVATE_REF_PATTERNS = (
    re.compile(r"\.workbuddy/"),
    re.compile(r"docs/chroma3380"),
    re.compile(r"\bTODO\.md\b"),
    re.compile(r"\bHANDOFF\.md\b"),
    re.compile(r"\bLESSONS\.md\b"),
    re.compile(r"\bPROJECT\.md\b"),
    re.compile(r"\bARCHITECTURE\.md\b"),
    # 通用规则：docs/ 下从不发布任何 .md，指过去就是死指针。
    # 用通用式而非逐个列名，新增私密文档时无需改本文件。
    re.compile(r"(?<![\w/.\-])docs/[A-Za-z0-9_\-]+\.md"),
)
PRIVATE_REF_EXEMPT = {
    ".gitignore",                       # 必须列出私密文件才能忽略它们
    "scripts/check-sensitive-words.py",  # 要说明词表放哪里
    "scripts/check-public-surface.py",   # 本文件自身
}

# R6：`temp/` 下的具体文件名不得出现在公开文件里。
# 只看「temp/ 后面紧跟名字」的形态：`temp/` 单独出现（提及目录本身）不算，
# 而 `C:\Windows\Temp\x`、`%TEMP%\x`、`foo_temp/x` 这类运行时常量也不算
# （前一个字符是路径分隔符/下划线/大小写不同，故用下位断言排除）。
TEMP_FILE_PATTERNS = (
    re.compile(r"(?<![A-Za-z0-9_\\/:.\-])temp[/\\](?=[A-Za-z0-9_.\-])"),
)

# R5 只扫文本文件；按扩展名白名单挑，避免把二进制读进来
TEXT_EXTS = (
    ".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".rc", ".rh",
    ".py", ".ps1", ".bat", ".cmd", ".sh",
    ".json", ".yml", ".yaml", ".toml", ".ini", ".cfg", ".txt", ".md",
    ".gitignore", ".gitattributes", ".rsp", ".def", ".manifest",
)
# 单个文件超过这个大小就不扫（防御性上限）
MAX_SCAN_BYTES = 4 * 1024 * 1024


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


def check_paths(files):
    """R1–R4：纯路径规则。返回违规描述列表。"""
    bad = []
    for f in files:
        low = f.lower()
        base = low.rsplit("/", 1)[-1]

        for pref in DENY_PREFIXES:
            if low.startswith(pref):
                bad.append("R1 私密目录被跟踪：%s" % f)
                break

        if base in DENY_BASENAMES:
            bad.append("R2 私密文档被跟踪：%s" % f)

        if base.endswith(".md"):
            allowed = (f in MD_ALLOWLIST
                       or any(f.startswith(p) for p in MD_ALLOWED_PREFIXES))
            if not allowed:
                bad.append("R3 未在白名单内的 .md：%s"
                           "（如确要公开，请加入 MD_ALLOWLIST）" % f)

        if "/" not in f and base.endswith(".txt") and f not in ROOT_TXT_ALLOWLIST:
            bad.append("R4 根目录 .txt 未在白名单内：%s" % f)
    return bad


def is_text_candidate(path):
    low = path.lower()
    if "/" not in low and low.startswith(".git"):
        return True
    return low.endswith(TEXT_EXTS)


def check_private_refs(root, files):
    """R5/R6：公开文件正文里不得出现私密文档指针或 temp/ 下的具体文件名。"""
    bad = []
    for f in files:
        if f in PRIVATE_REF_EXEMPT:
            continue
        if f.startswith("third_party/"):
            continue
        if not is_text_candidate(f):
            continue
        p = os.path.join(root, f.replace("/", os.sep))
        try:
            if os.path.getsize(p) > MAX_SCAN_BYTES:
                continue
            with open(p, "rb") as fh:
                raw = fh.read()
        except OSError:
            continue
        if b"\0" in raw[:4096]:       # 二进制探测
            continue
        text = raw.decode("utf-8", "replace")
        for i, line in enumerate(text.splitlines(), 1):
            for pat in PRIVATE_REF_PATTERNS:
                m = pat.search(line)
                if m:
                    bad.append("R5 引用了私密文档（%s）：%s:%d" % (m.group(0), f, i))
                    break
            for pat in TEMP_FILE_PATTERNS:
                if pat.search(line):
                    bad.append("R6 写出了 temp/ 下的具体文件名：%s:%d" % (f, i))
                    break
    return bad


def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"],
                                   stderr=subprocess.PIPE).decode().strip()
    os.chdir(root)

    files = git_ls_files()
    bad = check_paths(files) + check_private_refs(root, files)

    if verbose:
        print("仓库：%s" % root)
        print("跟踪文件：%d" % len(files))
        print("文档白名单：%s" % ", ".join(sorted(MD_ALLOWLIST)))

    if bad:
        print("RESULT: %d 处违规" % len(bad))
        for b in bad:
            print("  " + b)
        print("\n说明：公开仓库只允许代码与三个 .md。"
              "私密文件请留在本地并用 .gitignore 排除。")
        return 1

    print("RESULT: CLEAN —— 公开面只有允许的文件")
    return 0


if __name__ == "__main__":
    sys.exit(main())
