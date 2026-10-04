#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NPPM_/NPPN_ 消息号契约守卫：把 src/plugin/npp/NppMessages.h 里**手工抄录**
的数值，逐条对着上游权威源核对。

为什么需要它：
    这些数值是"接口数值"（事实），但它们进入本仓库的方式是**人工抄录**：
    NppMessages.h 的注释写着"从公开事实源人工核对后抄录"，而在此之前**没有任何
    机器检查**。抄错一个数字的后果不是崩溃、也不是返回 0，而是**静默换一条消息**
    —— `case nn::NPPM_X` 匹配到的是另一个线上消息号，于是宿主回答了**另一个问题**。
    插件读到的是"看起来正常"的返回值。

    本批（批次 120）就是这么抓到一个真缺陷的：`NPPM_GETMENUBAR` 在官方 API 里
    **根本不存在**，而它占的 `NPPMSG + 52` 是真实的 `NPPM_ISTABBARHIDDEN`
    （BOOL：标签栏是否隐藏）。我们的分发器对 +52 返回的是 **HMENU** ⇒
    插件问"标签栏隐藏了吗"会拿到一个非空指针，读成 TRUE。
    ★ 注意：只比对"数值是否等于某个上游数值"是**抓不到**这个的 —— 1052 确实是
      上游的合法数值，只是**属于另一个名字**。所以判据必须**按名字对账**。

规则：
    R1  我们声明的每个名字，必须在冻结事实表里存在（否则"上游无此名"）。
        —— 这条正是抓出 NPPM_GETMENUBAR 的那一条。
    R2  名字相同则数值必须相同。
    R3  基值（NPPMSG / RUNCOMMAND_USER / NPPN_FIRST）必须与上游一致。
    R4  `kNppKnownMsgs` 清单必须与 `NppMsg` 枚举**双向一致**。
        —— 这条补的是 tests/test_nppm_marshal.cpp 自己声明的边界："本守卫证明
           「分类表 ↔ 清单」一致，**不**证明「清单 ↔ 枚举」一致（枚举不可遍历）"。
           C++ 枚举不可遍历是真的，但**文本**可遍历：本脚本已经解析了枚举体，
           顺带把清单也解析了就能对账。漏加清单 ⇒ 这里红（以前只能靠纪律）。
    R5  视图/菜单常量（`kAllOpenFiles` / `kPrimaryView` / `kSecondView` /
        `kMainViewPos` / `kSubViewPos` / `NppPluginMenu` / `NppMainMenu`）必须按
        **名字**与上游对应的 #define 一致。这些值同样是 load-bearing：分发器用
        `==` 比较它们（PluginManager.cpp），抄错一个 = 宿主按**另一个视图/菜单**
        作答，与 R1/R2 抓的是同一类"静默答错"（不是崩溃、不是返回 0）。
        ★ **项数本身就是断言**：映射表 CONST_OF、事实表 constants 段、头文件里
          解析到的 `constexpr int` 行，三条口径必须都是 7 条；任何一条对不上都红
          —— 少配一条 = 那一条永远不被核对，而守卫仍然报 CLEAN。
        ★ 头文件里出现**未登记**的 `constexpr int` 也判失败：否则将来新增常量会
          静默绕过本规则（与 R0 同一个道理：宁可红，不可静默少算）。
    R0（前置）**解析器必须覆盖整个 enum 体与常量声明区**。凡有无法归类的行，
        直接判失败并打印出来 —— "解析器跟不上文件格式"会表现为**静默少算**，
        而少算的守卫比没有守卫更危险（check-command-ids.py 写脚本时连踩两次，
        两次都安静报 CLEAN）。

用法：
    python scripts/check-nppm-contract.py                # 检查（0 = 干净 / 1 = 有违规）
    python scripts/check-nppm-contract.py -v             # 额外打印计数与未声明的上游条目
    python scripts/check-nppm-contract.py --refresh FILE # 从抓取的上游头文件重建事实表
                                                        # （需要网络：见 --refresh 的提示）

退出码：0 = 干净；1 = 有违规或解析失败；2 = 环境问题。
"""

import json
import os
import re
import subprocess
import sys

HEADER = os.path.join("src", "plugin", "npp", "NppMessages.h")
FACTS = os.path.join("scripts", "nppm-contract.json")

UPSTREAM_URL = (
    "https://raw.githubusercontent.com/notepad-plus-plus/notepad-plus-plus/"
    "master/PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h"
)

# 我们头文件里的三个基值常量 → 上游宏名
BASE_OF = {
    "kNppMsgBase": "NPPMSG",
    "kRunCmdBase": "RUNCOMMAND_USER",
    "kNppnBase": "NPPN_FIRST",
}
# 我们头文件里的视图/菜单常量 → 上游宏名。★ 必须一一对应：R5 按名字对账，
# 映射表本身就是"哪些常量受本守卫保护"的边界（漏登记一条 = 它永远不被核对）。
CONST_OF = {
    "kAllOpenFiles": "ALL_OPEN_FILES",
    "kPrimaryView": "PRIMARY_VIEW",
    "kSecondView": "SECOND_VIEW",
    "kMainViewPos": "MAIN_VIEW",
    "kSubViewPos": "SUB_VIEW",
    "NppPluginMenu": "NPPPLUGINMENU",
    "NppMainMenu": "NPPMAINMENU",
}

WM_USER = 0x400  # Windows 固定值；上游 NPPMSG = (WM_USER + 1000)

# 我们 enum 体的起止
RE_ENUM_OPEN = re.compile(r"^enum\s+\w+\s*(:\s*[\w: ]+)?\s*\{$")
RE_ENUM_CLOSE = re.compile(r"^\};$")
# enum 项：NAME = <base> + <int>,
RE_ENTRY = re.compile(
    r"^(NPPM_[A-Z0-9_]+|NPPN_[A-Z0-9_]+)\s*=\s*"
    r"(kNppMsgBase|kRunCmdBase|kNppnBase)\s*\+\s*(\d+)$"
)
# kNppKnownMsgs 清单的起止与条目
RE_LIST_OPEN = re.compile(r"^inline\s+constexpr\s+unsigned\s+kNppKnownMsgs\s*\[\s*\]\s*=\s*\{$")
RE_LIST_ITEM = re.compile(r"^(NPPM_[A-Z0-9_]+)$")
# 基值常量定义：constexpr UINT kNppMsgBase = WM_USER + 1000;
RE_BASE_DEF = re.compile(
    r"^constexpr\s+UINT\s+(kNppMsgBase|kRunCmdBase|kNppnBase)\s*=\s*"
    r"(WM_USER\s*\+\s*)?(\d+)\s*;"
)
# ★ 基值也可能写成**枚举成员**（kNppnBase 就是）：kNppnBase = 1000,
#   R0 就是靠"这行无法归类"把这一条抓出来的 —— 第一版解析器只认 constexpr 形态，
#   于是 NPPN 那一族的基值恒为 0，34 条通知码会被整体判成"值不一致"（静默错判）。
RE_BASE_IN_ENUM = re.compile(
    r"^(kNppMsgBase|kRunCmdBase|kNppnBase)\s*=\s*(WM_USER\s*\+\s*)?(\d+)$"
)
# 视图/菜单常量定义（R5）：constexpr int kAllOpenFiles = 0;
RE_CONST_INT = re.compile(r"^constexpr\s+int\s+([A-Za-z_]\w*)\s*=\s*(\d+)\s*;$")
# 派生计数：它不是上游事实（由上一条 sizeof 派生），显式放行而不是当未登记处理
RE_KNOWN_COUNT = re.compile(
    r"^inline\s+constexpr\s+std::size_t\s+kNppKnownCount\s*=$"
)


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


# ---------------------------------------------------------------- 解析我们的头文件

def parse_header(path):
    """返回 (bases, entries, known, consts, unclassified)。

    bases:       {常量名: 绝对数值}
    entries:     [(name, base, offset, abs_value, lineno)]
    known:       [(name, lineno)] —— kNppKnownMsgs 清单里的条目
    consts:      [(name, value, lineno)] —— 视图/菜单常量（R5 用）
    unclassified:[(lineno, text)] —— enum/清单体里、或常量声明区里无法归类的行；
                 非空即解析失败。
    """
    with open(path, "rb") as fh:
        lines = fh.read().decode("utf-8", "replace").splitlines()

    bases, entries, known, consts, unclassified = {}, [], [], [], []
    in_enum = in_list = False
    for lineno, raw in enumerate(lines, 1):
        code = raw.split("//", 1)[0]
        stripped = code.strip()

        m = RE_BASE_DEF.match(stripped)
        if m:
            v = int(m.group(3)) + (WM_USER if m.group(2) else 0)
            bases[m.group(1)] = v
            continue

        if in_list:
            if RE_ENUM_CLOSE.match(stripped):
                in_list = False
                continue
            if not stripped:
                continue
            m = RE_LIST_ITEM.match(stripped.rstrip(",").strip())
            if m:
                known.append((m.group(1), lineno))
            else:
                unclassified.append((lineno, raw.strip()))
            continue

        if not in_enum:
            if RE_ENUM_OPEN.match(stripped):
                in_enum = True
            elif RE_LIST_OPEN.match(stripped):
                in_list = True
            elif RE_KNOWN_COUNT.match(stripped):
                pass  # 派生计数，显式放行
            else:
                m = RE_CONST_INT.match(stripped)
                if m:
                    consts.append((m.group(1), int(m.group(2)), lineno))
                elif stripped.startswith(("constexpr", "inline constexpr")):
                    # 未登记的常量声明：本脚本无法判断它是否也是上游事实，
                    # 静默放过 = 将来新增常量会绕过 R5。宁可红。
                    unclassified.append((lineno, raw.strip()))
            continue
        if RE_ENUM_CLOSE.match(stripped):
            in_enum = False
            continue
        if not stripped:
            continue

        body = stripped.rstrip(",").strip()
        m = RE_BASE_IN_ENUM.match(body)
        if m:
            bases[m.group(1)] = int(m.group(3)) + (WM_USER if m.group(2) else 0)
            continue
        m = RE_ENTRY.match(body)
        if not m:
            unclassified.append((lineno, raw.strip()))
            continue
        name, base, off = m.group(1), m.group(2), int(m.group(3))
        entries.append((name, base, off, bases.get(base, 0) + off, lineno))

    return bases, entries, known, consts, unclassified


# ------------------------------------------------------------- 解析上游权威头文件

def parse_upstream(path, extra_names=()):
    """把上游 Notepad_plus_msgs.h 解析成 (基值, {名字: 绝对数值}, {额外常量: 值})。

    上游的写法是 `#define NPPM_X (NPPMSG + 4)`，而 RUNCOMMAND 族用**符号常量**
    （`(RUNCOMMAND_USER + FULL_CURRENT_PATH)`）⇒ 必须递归展开 #define 体，
    否则那一族会全部"无法求值"（写本脚本时第一版就漏了，12 条全空）。

    extra_names 是 R5 要核对的普通常量（ALL_OPEN_FILES 等）—— 它们的宏名不长
    NPPM_/NPPN_ 前缀，收不进 facts，所以单独解一份；求值器 ev 是闭包，不外泄。
    """
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8", "replace")

    defines = {}
    for m in re.finditer(r"^\s*#define\s+([A-Za-z_]\w*)\s+([^\r\n/]+)", text, re.M):
        defines.setdefault(m.group(1), m.group(2).strip())

    def ev(name, depth=0):
        if depth > 16 or name not in defines:
            return None
        expr = defines[name]
        for ident in sorted(set(re.findall(r"[A-Za-z_]\w*", expr)), key=len, reverse=True):
            if ident == "WM_USER":
                continue
            sub = ev(ident, depth + 1)
            if sub is None:
                return None
            expr = re.sub(r"\b%s\b" % ident, str(sub), expr)
        expr = expr.replace("WM_USER", str(WM_USER))
        if not re.fullmatch(r"[\d\s()+\-*/]+", expr):
            return None
        try:
            return int(eval(expr, {"__builtins__": {}}, {}))
        except Exception:
            return None

    bases = {up: ev(up) for up in ("NPPMSG", "RUNCOMMAND_USER", "NPPN_FIRST")}
    facts = {}
    for m in re.finditer(r"^\s*#define\s+(NPPM_[A-Z0-9_]+|NPPN_[A-Z0-9_]+)\s+", text, re.M):
        v = ev(m.group(1))
        if v is not None:
            facts[m.group(1)] = v
    extras = {n: ev(n) for n in extra_names}
    return bases, facts, extras


def upstream_sha256(path):
    import hashlib

    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def refresh(fetch_path):
    if not os.path.isfile(fetch_path):
        print("找不到上游文件：%s" % fetch_path)
        print("\n怎么拿到它（需要网络；沙箱里 curl 可直连 raw.githubusercontent）：")
        print("  curl -sS -o %s %s" % (fetch_path.replace("\\", "/"), UPSTREAM_URL))
        return 2

    # 上游宏名取自 CONST_OF 的值，保证"映射表"与"事实表"不会各写一份而漂移。
    up_names = sorted(set(CONST_OF.values()))
    if len(up_names) != len(CONST_OF):
        print("CONST_OF 里有重复的上游宏名 —— 映射表必须一一对应")
        return 2

    bases, facts, constants = parse_upstream(fetch_path, up_names)
    missing = [k for k, v in bases.items() if v is None]
    if missing:
        print("解析上游失败：基值 %s 无法求值（上游改了写法？）" % ", ".join(missing))
        return 2
    if len(facts) < 100:
        print("解析上游失败：只解出 %d 条（上游格式变了？宁可失败也别写一张残缺的表）"
              % len(facts))
        return 2

    # ---- 视图/菜单常量（R5 的事实源）----
    bad = [n for n, v in constants.items() if v is None]
    if bad:
        print("解析上游失败：常量 %s 无法求值（上游删了/改了这些宏？宁可失败也不要"
              "写一张缺条目的表）" % ", ".join(bad))
        return 2

    doc = {
        "_comment": [
            "上游 Notepad++ 消息号的冻结事实表（name → 绝对数值）。",
            "由 scripts/check-nppm-contract.py --refresh <上游头文件> 生成，**不要手改**。",
            "接口数值属于事实；本表只记录事实，不含上游任何代码或注释。",
            "守卫 check-nppm-contract.py 用本表核对 src/plugin/npp/NppMessages.h。",
            "facts = NPPM_/NPPN_ 消息号；constants = 视图/菜单等被宿主与插件"
            "共同引用的上游宏值（规则 R5）。",
        ],
        "provenance": {
            "repo": "notepad-plus-plus/notepad-plus-plus",
            "path": "PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h",
            "url": UPSTREAM_URL,
            "file_sha256": upstream_sha256(fetch_path),
            "bases": bases,
            "counts": {
                "NPPM": sum(1 for k in facts if k.startswith("NPPM_")),
                "NPPN": sum(1 for k in facts if k.startswith("NPPN_")),
                "constants": len(constants),
            },
        },
        "facts": dict(sorted(facts.items())),
        "constants": dict(sorted(constants.items())),
    }
    with open(FACTS, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2, sort_keys=False)
        fh.write("\n")
    print("已重建 %s：NPPM %d 条 / NPPN %d 条 / 常量 %d 条（sha256 %s）"
          % (FACTS.replace("\\", "/"),
             doc["provenance"]["counts"]["NPPM"],
             doc["provenance"]["counts"]["NPPN"],
             doc["provenance"]["counts"]["constants"],
             doc["provenance"]["file_sha256"][:16]))
    return 0


def main(argv):
    _force_utf8_stdio()

    if "--refresh" in argv:
        i = argv.index("--refresh")
        if i + 1 >= len(argv):
            print("--refresh 需要一个参数：抓取到的上游头文件路径")
            return 2
        root = repo_root()
        if root is None:
            return 2
        os.chdir(root)
        return refresh(argv[i + 1])

    verbose = "-v" in argv or "--verbose" in argv

    root = repo_root()
    if root is None:
        return 2
    os.chdir(root)

    if not os.path.isfile(FACTS):
        print("找不到事实表 %s —— 用 --refresh 生成" % FACTS.replace("\\", "/"))
        return 2
    with open(FACTS, "rb") as fh:
        doc = json.loads(fh.read().decode("utf-8"))
    facts = doc["facts"]
    up_bases = doc["provenance"]["bases"]

    if not os.path.isfile(HEADER):
        print("找不到 %s" % HEADER.replace("\\", "/"))
        return 2
    bases, entries, known, consts, unclassified = parse_header(HEADER)

    # ---- R0：解析器必须覆盖整个 enum 体、清单体与常量声明区 ----
    if unclassified:
        print("RESULT: 解析失败 —— 有 %d 行无法归类" % len(unclassified))
        for lineno, text in unclassified:
            print("  %s:%d  %s" % (HEADER.replace("\\", "/"), lineno, text))
        print("\n说明：无法归类意味着本脚本会**静默少算**，那样它报的 CLEAN 没有意义。")
        print("      请更新 parse_header() 以支持新的写法（别改成「跳过这一行」），"
              "或把新常量登记进 CONST_OF（若它来自上游）。")
        return 1
    if not entries:
        print("RESULT: 解析失败 —— 一个消息项都没读到（文件格式变了？）")
        return 1
    if not known:
        print("RESULT: 解析失败 —— 没读到 kNppKnownMsgs 清单（格式变了？）")
        return 1

    # ---- R3：基值必须与上游一致 ----
    bad_bases = []
    for const, up_name in BASE_OF.items():
        if const not in bases:
            bad_bases.append("%s（头文件里没解析到）" % const)
        elif up_bases.get(up_name) != bases[const]:
            bad_bases.append("%s = %d，上游 %s = %s"
                             % (const, bases[const], up_name, up_bases.get(up_name)))

    # ---- R1 / R2 ----
    unknown, mismatch = [], []
    for name, _base, _off, value, lineno in entries:
        if name not in facts:
            unknown.append((name, lineno, value))
        elif facts[name] != value:
            mismatch.append((name, lineno, value, facts[name]))

    # ---- R4：kNppKnownMsgs ↔ NppMsg 枚举（双向） ----
    enum_nppm = {e[0] for e in entries if e[0].startswith("NPPM_")}
    listed = {n for n, _ln in known}
    not_listed = sorted(enum_nppm - listed)     # 枚举有、清单没有
    not_enum = sorted(listed - enum_nppm)       # 清单有、枚举没有
    dup_list = sorted({n for n, _ln in known if [x for x, _ in known].count(n) > 1})

    # ---- R5：视图/菜单常量必须按**名字**与上游对账 ----
    const_bad = []
    up_consts = doc.get("constants")
    if not isinstance(up_consts, dict) or not up_consts:
        print("RESULT: 事实表缺 constants 段（或为空）—— 用 --refresh 重建"
              "（本守卫宁可失败，也不静默跳过这一族常量）")
        return 2
    # ★ 项数本身就是断言：三条口径必须吻合，少配一条 = 那一条永远不被核对。
    if len(CONST_OF) != len(up_consts):
        const_bad.append("映射表 CONST_OF %d 条 ≠ 事实表 constants %d 条"
                         % (len(CONST_OF), len(up_consts)))
    if len(consts) != len(CONST_OF):
        const_bad.append("头文件解析到 %d 条 constexpr int ≠ 映射表 %d 条"
                         % (len(consts), len(CONST_OF)))
    parsed_consts = {n: v for n, v, _ln in consts}
    for our, up in sorted(CONST_OF.items()):
        if our not in parsed_consts:
            const_bad.append("%s：头文件里没有这个常量（映射表声明了它）" % our)
        elif up not in up_consts:
            const_bad.append("%s：事实表里没有上游宏 %s" % (our, up))
        elif parsed_consts[our] != up_consts[up]:
            const_bad.append("%s = %d，上游宏 %s = %d"
                             % (our, parsed_consts[our], up, up_consts[up]))

    if verbose:
        print("仓库：%s" % root)
        print("事实表：%s（%d 条；上游 %s）"
              % (FACTS.replace("\\", "/"), len(facts),
                 doc["provenance"]["file_sha256"][:16]))
        print("我们声明：%d 条（NPPM %d / NPPN %d）；清单 %d 条"
              % (len(entries),
                 len(enum_nppm),
                 sum(1 for e in entries if e[0].startswith("NPPN_")),
                 len(listed)))
        print("视图/菜单常量：头文件 %d 条 / 映射表 %d 条 / 事实表 %d 条（R5）"
              % (len(consts), len(CONST_OF), len(up_consts)))
        rest = sorted(set(facts) - {e[0] for e in entries})
        print("上游有而我们未声明：%d 条（信息性，不计失败）" % len(rest))

    if (bad_bases or unknown or mismatch or not_listed or not_enum or dup_list
            or const_bad):
        n = (len(bad_bases) + len(unknown) + len(mismatch)
             + len(not_listed) + len(not_enum) + len(dup_list) + len(const_bad))
        print("RESULT: %d 处违规" % n)
        for t in bad_bases:
            print("  R3 基值与上游不一致：%s" % t)
        for name, lineno, value in unknown:
            print("  R1 %s:%d  上游**没有**这个名字（我们声明为 %d）"
                  % (HEADER.replace("\\", "/"), lineno, value))
        for name, lineno, value, up in mismatch:
            print("  R2 %s:%d  %s：我们 = %d，上游 = %d"
                  % (HEADER.replace("\\", "/"), lineno, name, value, up))
        for name in not_listed:
            print("  R4 枚举里有 %s，但 kNppKnownMsgs 清单里没有"
                  "（分类守卫会因此漏掉它）" % name)
        for name in not_enum:
            print("  R4 清单里有 %s，但 NppMsg 枚举里没有（清单与枚举已漂移）" % name)
        for name in dup_list:
            print("  R4 清单里 %s 出现多次" % name)
        for t in const_bad:
            print("  R5 视图/菜单常量与上游不一致：%s" % t)
        print("\n说明：抄错一个消息号的症状**不是**返回 0、也不是崩溃，而是"
              "「静默换一条消息」——")
        print("      宿主回答了另一个问题，插件读到的返回值看起来正常。"
              "所以必须按**名字**对账：")
        print("      只查「数值是否等于某个上游数值」抓不到「名字是编的、数值是别人的」"
              "这一类。")
        print("      视图/菜单常量同理：宿主用 == 拿它们跟插件传来的 wParam 比较，"
              "抄错 = 按另一个视图/菜单作答。")
        return 1

    print("RESULT: CLEAN —— %d 个消息号与上游一致（NPPM %d / NPPN %d）；"
          "清单 %d 条与枚举双向一致；视图/菜单常量 %d 条与上游一致"
          % (len(entries), len(enum_nppm),
             sum(1 for e in entries if e[0].startswith("NPPN_")), len(listed),
             len(consts)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
