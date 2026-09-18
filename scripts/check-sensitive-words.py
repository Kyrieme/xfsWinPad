#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描工作树里的敏感词（客户名 / 产品代号 / 料号 / 人名 / 项目代号 …）。

设计要点：**词表不在本脚本里**，而是从一个私密文件读入
（默认 `.workbuddy/sensitive-words.txt`，该目录已被 .gitignore 排除）。
所以本脚本可以安全入库 —— 公开仓库里看不到任何一个敏感词本身。

用法
----
    python scripts/check-sensitive-words.py              # 扫 git 跟踪的文件（提交前用）
    python scripts/check-sensitive-words.py --all        # 扫整个工作树（含未跟踪的 temp/ 等）
    python scripts/check-sensitive-words.py --staged     # 只扫暂存区（适合挂 pre-commit）
    python scripts/check-sensitive-words.py --words F    # 指定词表
    python scripts/check-sensitive-words.py --list       # 只打印词表
    python scripts/check-sensitive-words.py --add 新词   # 追加一个词到词表（幂等）

退出码：0 = 干净；1 = 命中；2 = 用法或词表问题。

词表格式：一行一个词，`#` 开头为注释，空行忽略。匹配大小写不敏感。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys


def _force_utf8_stdio() -> None:
    """把标准输出/错误切到 UTF-8（理由同 check-public-surface.py）。

    windows runner 上的 Python 默认用本地代码页输出，打印中文时
    UnicodeEncodeError 会让"扫描干净"以非零码退出，误导 CI 判红。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
        except (AttributeError, ValueError, OSError):
            pass


_force_utf8_stdio()

DEFAULT_WORDS = os.path.join(".workbuddy", "sensitive-words.txt")

# --all 模式下跳过的目录（构建产物 / 缓存 / 版本库自身）
SKIP_DIRS = {
    ".git", "build", "dist", "node_modules", ".vs", ".idea", ".cache",
    "out", "cmake-build-debug", "cmake-build-release",
}

# 词表里少于这个长度的词会被跳过（太短会满屏误报）
MIN_WORD_LEN = 3

# 只看这些文本扩展名之外的，一律当二进制跳过
SKIP_EXT = {
    ".exe", ".dll", ".lib", ".obj", ".pdb", ".ilk", ".exp", ".res",
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".cur", ".icns",
    ".zip", ".7z", ".gz", ".tar", ".pdf", ".docx", ".xlsx", ".pptx",
    ".woff", ".woff2", ".ttf", ".otf", ".eot", ".bin", ".dat",
}


def repo_root() -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            stderr=subprocess.DEVNULL,
        )
        return out.decode("utf-8", "replace").strip()
    except Exception:
        return os.getcwd()


def load_words(path: str, explicit: bool) -> list[str]:
    if not os.path.exists(path):
        if explicit:
            # 用户明确指了词表却不存在 —— 必须报错，不能悄悄放过
            print("词表不存在：%s" % path, file=sys.stderr)
            sys.exit(2)
        # 默认词表不存在（例如 CI runner 上没有私密区）→ 跳过而不是失败
        print("SKIP: 未配置词表（%s）—— 跳过敏感词扫描。" % path)
        print("      本地用 --add <词> 建表，或用 --words <文件> 指定。")
        sys.exit(0)
    words, seen = [], set()
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            w = raw.strip()
            if not w or w.startswith("#"):
                continue
            if len(w) < MIN_WORD_LEN:
                print("跳过过短的词（<%d）：%r" % (MIN_WORD_LEN, w), file=sys.stderr)
                continue
            k = w.casefold()
            if k not in seen:
                seen.add(k)
                words.append(w)
    if not words:
        print("词表里没有有效词条：%s" % path, file=sys.stderr)
        sys.exit(2)
    # 长词优先，避免短词先命中把长词的上下文切碎
    words.sort(key=len, reverse=True)
    return words


def listed_files(root: str, mode: str) -> list[str]:
    if mode == "staged":
        cmd = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"]
    else:
        cmd = ["git", "ls-files", "-z"]
    try:
        out = subprocess.check_output(cmd, cwd=root, stderr=subprocess.DEVNULL)
    except Exception as e:
        print("git 取文件列表失败：%s" % e, file=sys.stderr)
        sys.exit(2)
    return [p for p in out.decode("utf-8", "surrogateescape").split("\0") if p]


def walk_files(root: str) -> list[str]:
    acc = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            acc.append(os.path.relpath(os.path.join(dirpath, fn), root))
    return acc


def read_text(path: str) -> str | None:
    if os.path.splitext(path)[1].lower() in SKIP_EXT:
        return None
    try:
        if os.path.getsize(path) > 40 * 1024 * 1024:      # 40MB 以上不查
            return None
        raw = open(path, "rb").read()
    except OSError:
        return None
    if b"\0" in raw[:8192]:                                 # 含 NUL → 二进制
        return None
    for enc in ("utf-8", "gb18030", "utf-16"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="扫描敏感词（词表在私密区，脚本本身可入库）")
    ap.add_argument("--words", default=None, help="词表文件（默认 %s）" % DEFAULT_WORDS)
    ap.add_argument("--all", action="store_true",
                    help="扫整个工作树（含未跟踪文件），而非仅 git 跟踪的文件")
    ap.add_argument("--staged", action="store_true", help="只扫暂存区（pre-commit 用）")
    ap.add_argument("--list", action="store_true", help="只打印词表条数/内容")
    ap.add_argument("--add", metavar="WORD", help="追加一个词到词表（幂等）")
    ap.add_argument("--max-hits", type=int, default=200, help="最多打印多少条命中")
    args = ap.parse_args()

    root = repo_root()
    words_path = args.words or os.path.join(root, DEFAULT_WORDS)

    if args.add:
        os.makedirs(os.path.dirname(words_path), exist_ok=True)
        exist = []
        if os.path.exists(words_path):
            with open(words_path, "r", encoding="utf-8") as f:
                exist = [ln.strip() for ln in f]
        if args.add.casefold() in {e.casefold() for e in exist if e.strip()}:
            print("已在词表中：%s" % args.add)
            return 0
        with open(words_path, "a", encoding="utf-8", newline="\n") as f:
            if exist and exist[-1].strip():
                f.write("\n")
            f.write(args.add + "\n")
        print("已追加到 %s：%s" % (words_path, args.add))
        return 0

    words = load_words(words_path, explicit=bool(args.words))

    if args.list:
        print("词表 %s：%d 条" % (words_path, len(words)))
        for w in words:
            print("  %s" % w)
        return 0

    pat = re.compile("|".join(re.escape(w) for w in words), re.IGNORECASE)

    mode = "staged" if args.staged else ("all" if args.all else "tracked")
    files = listed_files(root, mode) if mode != "all" else walk_files(root)

    self_rel = os.path.relpath(os.path.abspath(words_path), root).replace("\\", "/")
    hits, scanned = [], 0
    for rel in files:
        rel_norm = rel.replace("\\", "/")
        if rel_norm == self_rel:            # 词表自己当然含有全部词
            continue
        p = os.path.join(root, rel)
        if not os.path.isfile(p):
            continue
        text = read_text(p)
        if text is None:
            continue
        scanned += 1
        for ln, line in enumerate(text.splitlines(), 1):
            for m in pat.finditer(line):
                col = m.start() + 1
                ctx = line.strip()
                if len(ctx) > 110:
                    a = max(0, m.start() - 45)
                    ctx = ("…" if a else "") + line[a:a + 110].strip() + "…"
                hits.append((rel_norm, ln, col, m.group(0), ctx))
                if len(hits) >= args.max_hits:
                    break
            if len(hits) >= args.max_hits:
                break

    print("词表：%s（%d 条）" % (os.path.relpath(words_path, root), len(words)))
    print("模式：%s  扫描文件：%d" % (mode, scanned))
    if not hits:
        print("RESULT: CLEAN —— 未发现敏感词")
        return 0

    print("RESULT: %d 处命中" % len(hits))
    for rel, ln, col, w, ctx in hits:
        print("  %s:%d:%d  [%s]  %s" % (rel, ln, col, w, ctx))
    return 1


if __name__ == "__main__":
    sys.exit(main())
