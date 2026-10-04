#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""守卫清单（唯一来源）：一次跑完全部机器守卫。

为什么要有这个文件
------------------
批次 141 发现：仓库里**写好了、但没有任何东西在跑**的守卫有两个 ——

  · `scripts/gen-sci-marshal.py --check`：它自己的用法说明里写着"校验（CI 守卫）"，
    五张 .inc 与 Scintilla.iface 的同步靠它，但 CI 从来没调过它。
  · `scripts/_lang-keys-selftest.py`：批次 135 专门为"守卫自己也会腐烂"写的回归，
    同样没人跑。

后果是这两条守卫**只在有人记得手工跑时才存在**：`.inc` 手改到与 iface 不符、
语言键守卫被改瞎成恒绿，都不会有东西变红。这正是批次 135 说的"守卫也会腐烂"，
只不过腐烂的方式是"根本没通电"。

更根本的问题是**守卫清单被抄了两份**：`.github/workflows/ci.yml` 里一串步骤，
本地"七道闸"是脑子里的另一串。两份迟早漂移 —— 上面两个漏网的就是这么来的。

做法
----
把清单收敛到这里一处：CI 调它、本地也调它。加守卫时只改这一个列表，
两边同时生效，不存在"CI 有、本地没有"或反过来。

批次 142 补上同一个病的第三个受害者：`scripts/_profile-guard-selftest.ps1`。
它是 `scripts/_profile-guard.ps1`（e2e 探针跑的时候挡住**用户真实 profile** 不被
改写的安全闸）的自测 —— 自测没通电，闸门悄悄坏掉时没人会发现，而下一次 e2e 探针
就会照常去改用户真实的 session.json。它同样是"写好了没人跑"，只是它是 PowerShell。
所以本清单同时接纳 .ps1 守卫（见下面"解释器"一节的 kind）。

**本文件只输出 ASCII**：它的输出会进 CI 日志，ASCII 免去代码页问题
（子守卫自己的中文输出照旧，由 PYTHONIOENCODING=utf-8 兜住）。

不在清单里 / 单独跑的
---------------------
  · `check-sensitive-words.py`：词表**刻意不入库**（CI 从 secret 取），拿不到词表
    时自跳过。它需要额外参数与环境，留在 CI 里单独一步。

用法
----
    <python> scripts/run-guards.py          # 跑全部守卫（0 全过 / 1 有红 / 2 环境问题）
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))

# 顺序 = 需要人看时的优先级：先接口事实与公开面（错了会静默撒谎），
# 再编码/生成物同步，最后跑一遍各守卫自己的回归（最慢，放最后）。
#
# 每条 = (标签, 解释器 kind, 相对 ROOT 的脚本路径, 调用表)
#   · kind "py" = 用**当前解释器**（sys.executable，避开 PATH 上那个 Store 空壳）；
#     "ps" = Windows PowerShell 5.1（本仓是 Windows-only 项目）。
#   · 调用表里**每一项都是一次独立进程**的参数列表。多数守卫只跑一次（[[]]）；
#     sci-marshal-sync 带 --check。profile 守卫自测要**三次** —— 它的三个用例
#     （normal / throw / nc）各自 `exit`，而 .ps1 里的 exit 会结束**调用者**会话
#     （脚本头注释自己写明了），所以必须各起一个进程，不能串在一个会话里。
#
# 每条上面那几行注释就是它的"为什么存在"——原来散在 ci.yml 各个 step 里，
# 现在跟清单放一起，加守卫时不会再出现"注释在 CI、守卫不在 CI"。
GUARDS = [
    # 公开仓库只能有代码 + 三个 .md；私密笔记/样本/厂商手册靠 .gitignore 挡。
    # 守卫自己吃过的坑：R5 会拒"源码注释里出现私密文档名"。
    ("public-surface", "py", "scripts/check-public-surface.py", [[]]),

    # Tr() 取不到键时**回退成打印裸 id**（用户看到 "panel.compile.title"），
    # 不是编译错误。本守卫把"代码引用的每个键都必须在五种语言里存在"钉死。
    ("lang-keys", "py", "scripts/check-lang-keys.py", [[]]),

    # CommandIds.h 手工赋值、单值与保留区间混排（`Cmd::XxxFirst + i`）。
    # 区间撞车会让菜单项**静默**触发另一个动作（批次 39 真踩过），
    # 编译器与链接器都不会吭声。
    ("command-ids", "py", "scripts/check-command-ids.py", [[]]),

    # NppMessages.h 里的 NPPM_/NPPN_ 号是手工转录的接口事实。号错了不崩、
    # 也不返回 0 —— 它静默派发**另一条**消息，宿主答非所问、插件读到貌似合理的值。
    # 批次 120：一个上游根本不存在的 NPPM_GETMENUBAR 坐在 NPPMSG+52（真身是
    # NPPM_ISTABBARHIDDEN，返回 BOOL），却拿菜单句柄回答它。所以**只比值抓不住**
    # （1052 是合法值，只是属于别的名字），必须按名字对上冻结的事实表。
    ("nppm-contract", "py", "scripts/check-nppm-contract.py", [[]]),

    # 同一类洞，同目录：NppDocking.h 的停靠常量与 NppCompat.h 的结构体布局
    # 也是手工转录的接口事实。常量错 ⇒ 按错的位解 uMask / 把 DMM_MOVE 当别的派发；
    # 字段**顺序**错 ⇒ 从邻字段读出面板标题或命令号（"注册成功但名字是乱码"）。
    # 分工：字段顺序在这里对上上游头文件；由此得出的字节**偏移**由两个头文件里的
    # static_assert 钉住 —— 单靠 static_assert 只能证明"我们自洽"，证不了"跟随上游"。
    ("npp-abi-contract", "py", "scripts/check-npp-abi-contract.py", [[]]),

    # PowerShell 5.1 解析**无 BOM** 的 .ps1 用系统 ANSI 代码页，不是 UTF-8：
    # 非 ASCII 注释会按字节对消费，行尾时**把换行吞掉**、下一行代码并进注释，
    # 于是花括号失配、报错指在一个不含花括号的行上（批次 101 实测）。
    # 规则：任何含非 ASCII 字节的脚本必须带 UTF-8 BOM（或整份纯 ASCII）。
    ("script-encoding", "py", "scripts/check-script-encoding.py", [[]]),

    # scripts/ 里的探针要把本仓库的检出位置当成**字面量**写进默认值：只能在一台
    # 机器上成立。换机/换目录后默认值指向不存在（或**陈旧**）的 exe，探针就去测
    # 一个早已不是当前源码的东西 —— "一个悄悄不再对得上被测对象的验证资产，比
    # 没有更坏"（批次 72 教训；run-probe.cmd 的硬编码 ROOT 已单独修过一次）。
    # 虚拟机里宿主盘符根本不存在（见 chroma-e2e.ps1 注释），硬编码 D:\ 让整套探针
    # 在那边跑不起来。本守卫把 run-probe.cmd 的那条结论推广到整个 scripts/ 并接电。
    ("hardcoded-paths", "py", "scripts/check-hardcoded-paths.py", [[]]),

    # 生成物同步：五张 .inc（形状/入参/出参/出入参/结构）是从 Scintilla.iface
    # 生成的，手改一次就与 iface 分家，而 716 条直发消息照样"能跑"。
    # 本步就是生成器自己的"校验（CI 守卫）"模式：只校验、不写回（0 干净 / 1 过期）。
    # ⚠ 批次 141 之前 CI 从未调用它 —— 它自称 CI 守卫却没人跑。
    ("sci-marshal-sync", "py", "scripts/gen-sci-marshal.py", [["--check"]]),

    # 守卫自身的回归（批次 135）：逐条变异所有语言键守卫的克隆树，断言每条规则
    # 都真的会红、且红的是**预期那条**（只断言"变红"不够，红错规则等于没工作）。
    # 最慢的一条（约 25s），但它是"其余守卫没有悄悄变成恒绿"的唯一保证。
    ("lang-keys-selftest", "py", "scripts/_lang-keys-selftest.py", [[]]),

    # profile 安全闸的自测（批次 142 接电）：_profile-guard.ps1 在 e2e 探针跑之前
    # 快照用户的**真实** profile、跑完还原，并删掉探针新建的 slot —— 一个残留 slot
    # 会在之后每次启动都多开一个幽灵窗口（批次 108 的 post-mortem）。
    # 它会打印"restored N file(s)"，而一个什么都没做的闸照样打印这句话。
    # 这个自测把闸跑在 %TEMP% 的**假** profile 上，断言改写真的被还原，外加一个
    # 负控（把还原那一行抽掉，改写必须**存活**）。三个用例各要一个进程。
    ("profile-guard-selftest", "ps", "scripts/_profile-guard-selftest.ps1",
     [["-Case", c] for c in ("normal", "throw", "nc")]),
]


def build_argv(kind, path, extra):
    """把一条守卫的**一次**调用拼成完整命令行。"""
    if kind == "py":
        return [sys.executable, path] + extra
    if kind == "ps":
        # -NoProfile / -ExecutionPolicy Bypass：CI 与本机都不该被用户的 profile
        # 或执行策略改变结果（守卫要的是脚本本身的行为）。
        return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", path] + extra
    raise ValueError("未知解释器 kind：%r" % kind)


def main():
    # 子进程继承本进程的 stdio；这里只需要把结论汇总。
    failed = []
    for label, kind, rel, calls in GUARDS:
        path = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(path):
            print("ERROR: 守卫脚本不存在：%s" % rel, file=sys.stderr)
            return 2
        print("=== %s ===" % label)
        rc = 0
        for extra in calls:
            rc = subprocess.call(build_argv(kind, path, extra), cwd=ROOT)
            if rc != 0:
                break                       # 一次调用红了就够，不必跑完剩下的用例
        if rc == 0:
            print("--- %s: OK ---" % label)
        else:
            print("--- %s: FAILED (exit %d) ---" % (label, rc))
            failed.append((label, rc))

    print("")
    if failed:
        print("RESULT: %d/%d 守卫未通过 —— %s"
              % (len(failed), len(GUARDS), ", ".join(l for l, _ in failed)))
        return 1
    print("RESULT: CLEAN —— %d 条守卫全部通过" % len(GUARDS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
