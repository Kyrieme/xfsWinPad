#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NPP 插件 ABI 契约守卫：把 src/plugin/npp 下**手工抄录**的停靠常量与结构体
布局，逐条对着上游权威头文件核对。

为什么需要它（与 check-nppm-contract.py 同源的理由，只是换了族）：
    批次 120 用 check-nppm-contract.py 把 NppMessages.h 的 66 个消息号变成了
    "对着上游可核对"，并当场抓到一个真缺陷（NPPM_GETMENUBAR 是编出来的名字）。
    但同一个洞在**同目录的另外两个头文件**上还开着：
      * NppDocking.h —— 36 个停靠常量 + DockedWidgetData 的字段序；
      * NppCompat.h  —— FuncItem / ShortcutKey / NppData 的字段序。
    这些也是"接口数值/布局是事实、进仓库靠人工抄录"，而在此之前**没有任何机器
    检查**。抄错的后果同样是静默的：
      * 常量抄错 ⇒ 宿主按错误的位去解码 uMask，或把 DMM_MOVE 派发成别的动作；
      * 结构体字段序抄错 ⇒ 宿主读到的面板标题/命令 id 全是隔壁字段的内容，
        表现为"注册成功但名字乱码/命令串位"，不崩、不报错。

    ★ 本批（批次 121）逐条核对的结果是**全部正确** —— 也就是说本批**没有修掉
      任何缺陷**。它的价值不是"修了 bug"，而是把这 36 个常量与 4 个结构体的
      "今天恰好是对的"变成"以后每次改动都会被机器拦下"。
      一份靠人手核对一次、此后无人复核的契约，和一份没核对过的契约，在时间上
      是同一件事。

    ★ 本批还修掉一处**不对称**：产品侧的 DockedWidgetData 一条偏移断言都没有，
      而**测试夹具** tests/oop_plugins/oop_dmm.cpp 反而自己写了 5 条。守卫比被
      守卫的对象还严 —— 这是反的。本批把偏移断言补到产品侧头文件里（见
      NppDocking.h / NppCompat.h 的 static_assert）。

规则：
    R1  我们配对的每个上游名字，必须存在于冻结事实表（否则"上游无此名"）。
    R2  名字相同则数值必须相同。
    R3  结构体的**字段顺序**必须与上游逐项一致（按配对表翻译后比较序列）。
        —— 偏移由 C++ 侧 static_assert 钉死，顺序由本守卫钉死，两者合起来才
           等价于"布局正确"：static_assert 只能证明"我们和自己的期望一致"，
           证明不了"期望本身跟着上游走"。
    R4  结构体字段必须**全部**被配对表覆盖（漏配 = 无法归类 = 失败）。
    R0（前置）**解析器必须覆盖整个常量区与结构体体**。凡有无法归类的行，直接
        判失败并打印出来 —— "解析器跟不上文件格式"会表现为**静默少算**，而
        少算的守卫比没有守卫更危险（check-command-ids.py 与
        check-nppm-contract.py 都踩过这个坑：都安静报 CLEAN）。
        ★ 本脚本额外加了"项数本身就是断言"（常量必须恰好 36 条、结构体必须恰好
          4 个），因为"对 n 个元素成立"在 n==0 时是空真 —— 解析器一行都没读到时
          所有规则都会"通过"。

用法：
    python scripts/check-npp-abi-contract.py            # 检查（0 干净 / 1 有违规）
    python scripts/check-npp-abi-contract.py -v         # 额外打印计数与未声明条目
    python scripts/check-npp-abi-contract.py --refresh DIR
                                                        # 从 DIR 里的三个上游头文件
                                                        # 重建事实表（DIR 缺省为
                                                        # temp 树下的上游头文件目录）

退出码：0 = 干净；1 = 有违规或解析失败；2 = 环境问题。
"""

import hashlib
import json
import os
import re
import subprocess
import sys

HEADER_DOCK = os.path.join("src", "plugin", "npp", "NppDocking.h")
HEADER_COMPAT = os.path.join("src", "plugin", "npp", "NppCompat.h")
FACTS = os.path.join("scripts", "npp-abi-contract.json")

RAW = ("https://raw.githubusercontent.com/notepad-plus-plus/notepad-plus-plus/"
       "master/")

# 上游三个头文件：键 → (仓库内路径, 抓取用的本地文件名)
UPSTREAM = {
    "docking": ("PowerEditor/src/WinControls/DockingWnd/Docking.h", "Docking.h"),
    "resource": ("PowerEditor/src/WinControls/DockingWnd/dockingResource.h",
                 "dockingResource.h"),
    "iface": ("PowerEditor/src/MISC/PluginsManager/PluginInterface.h",
              "PluginInterface.h"),
}

# 本地常量名 → (上游名字, 出自哪个上游文件)
# ★ 这张表是**显式**的：本地名是驼峰带 k 前缀，上游是 SCREAMING_SNAKE，机械
#   转换对不上（kDwsIconTab 会变成 DWS_ICON_TAB，而上游是 DWS_ICONTAB）。
#   配错对会立刻表现为 R2 数值不一致（这些数值互不相同），不会静默放过。
PAIRING = {
    # ---- Docking.h ----
    "kCaptionTop": ("CAPTION_TOP", "docking"),
    "kCaptionBottom": ("CAPTION_BOTTOM", "docking"),
    "kContLeft": ("CONT_LEFT", "docking"),
    "kContRight": ("CONT_RIGHT", "docking"),
    "kContTop": ("CONT_TOP", "docking"),
    "kContBottom": ("CONT_BOTTOM", "docking"),
    "kContMax": ("DOCKCONT_MAX", "docking"),
    "kDwsIconTab": ("DWS_ICONTAB", "docking"),
    "kDwsIconBar": ("DWS_ICONBAR", "docking"),
    "kDwsAddInfo": ("DWS_ADDINFO", "docking"),
    "kDwsUseOwnDarkMode": ("DWS_USEOWNDARKMODE", "docking"),
    "kDwsParamsAll": ("DWS_PARAMSALL", "docking"),
    "kDwsDfContLeft": ("DWS_DF_CONT_LEFT", "docking"),
    "kDwsDfContRight": ("DWS_DF_CONT_RIGHT", "docking"),
    "kDwsDfContTop": ("DWS_DF_CONT_TOP", "docking"),
    "kDwsDfContBottom": ("DWS_DF_CONT_BOTTOM", "docking"),
    "kDwsDfFloating": ("DWS_DF_FLOATING", "docking"),
    # ---- dockingResource.h ----
    "kDmmMsg": ("DMM_MSG", "resource"),
    "DMM_CLOSE": ("DMM_CLOSE", "resource"),
    "DMM_DOCK": ("DMM_DOCK", "resource"),
    "DMM_FLOAT": ("DMM_FLOAT", "resource"),
    "DMM_DOCKALL": ("DMM_DOCKALL", "resource"),
    "DMM_FLOATALL": ("DMM_FLOATALL", "resource"),
    "DMM_MOVE": ("DMM_MOVE", "resource"),
    "DMM_UPDATEDISPINFO": ("DMM_UPDATEDISPINFO", "resource"),
    "DMM_DROPDATA": ("DMM_DROPDATA", "resource"),
    "DMM_MOVE_SPLITTER": ("DMM_MOVE_SPLITTER", "resource"),
    "DMM_CANCEL_MOVE": ("DMM_CANCEL_MOVE", "resource"),
    "DMM_LBUTTONUP": ("DMM_LBUTTONUP", "resource"),
    "kDmnFirst": ("DMN_FIRST", "resource"),
    "DMN_CLOSE": ("DMN_CLOSE", "resource"),
    "DMN_DOCK": ("DMN_DOCK", "resource"),
    "DMN_FLOAT": ("DMN_FLOAT", "resource"),
    "DMN_SWITCHIN": ("DMN_SWITCHIN", "resource"),
    "DMN_SWITCHOFF": ("DMN_SWITCHOFF", "resource"),
    "DMN_FLOATDROPPED": ("DMN_FLOATDROPPED", "resource"),
}

# 允许存在的"本地私有"常量（不配对上游）。★ 故意留成空表：一旦有人往
# NppDocking.h 的常量区加东西，必须显式决定它是"上游契约"还是"本地私有"，
# 否则 R0 会红。这是"无法归类要变成失败"的具体落点。
LOCAL_ONLY = frozenset()

# 结构体：(本地头文件, 本地结构名) → (上游文件, 上游结构名, 字段配对表)
# 字段配对表为 None 表示两侧字段名**完全相同**，直接按序列比较。
STRUCTS = {
    ("dock", "DockedWidgetData"): ("docking", "DockedWidgetData", None),
    ("compat", "FuncItem"): ("iface", "FuncItem", {
        "itemName": "_itemName",
        "func": "_pFunc",
        "cmdID": "_cmdID",
        "initCheck": "_init2Check",
        "shortcut": "_pShKey",
    }),
    ("compat", "ShortcutKey"): ("iface", "ShortcutKey", {
        "ctrl": "_isCtrl",
        "alt": "_isAlt",
        "shift": "_isShift",
        "key": "_key",
    }),
    ("compat", "NppData"): ("iface", "NppData", {
        "npp": "_nppHandle",
        "scintillaMain": "_scintillaMainHandle",
        "scintillaSecond": "_scintillaSecondHandle",
    }),
}

# "项数本身就是断言"：解析器一行都没读到时，全称命题空真。
EXPECT_CONSTS = 36
EXPECT_STRUCTS = 4

WM_USER = 0x400

RE_LOCAL_CONST = re.compile(
    r"^constexpr\s+(?:int|UINT|BOOL|DWORD|unsigned)\s+([A-Za-z_]\w*)\s*=\s*(.+?);$"
)
RE_DEFINE = re.compile(r"^\s*#define\s+([A-Za-z_]\w*)\s+([^\r\n/]+)", re.M)
RE_FIELD = re.compile(
    r"^\s*(?:const\s+|unsigned\s+|signed\s+|struct\s+)*"
    r"[A-Za-z_][\w:]*\s*[\*&]*\s*"
    r"([A-Za-z_]\w*)\s*"
    r"(?:\[\s*([A-Za-z_]\w*|\d+)\s*\])?\s*"
    r"(?:=[^;]*)?;\s*$"
)
RE_MENUITEMSIZE = re.compile(r"\bconst\s+int\s+menuItemSize\s*=\s*(\d+)\s*;")


def _force_utf8_stdio():
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


def read_text(path):
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", "replace")


def sha256_of(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def strip_comments(text):
    """去掉块注释与行注释。

    ★ 必须先去注释再解析：上游 dockingResource.h 里有
        //#define DMM_GETIMAGELIST            (DMM_MSG + 8)
        //#define DMM_GETICONPOS              (DMM_MSG + 9)
      这类**注释掉的**定义。行首锚定（^\\s*#define）本来就不会匹配 `//#define`，
      但块注释里另起一行的 `#define` 会被匹配到 —— 那会让守卫把"上游删掉的
      常量"当成"上游还有"，于是我们保留一个早已消失的常量却报 CLEAN。
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


# ------------------------------------------------------------------ 表达式求值

def _literal_to_dec(m):
    """把 C 整数字面量（含 u/U/L 后缀）转成十进制字符串。

    ★ 必须先做这一步再找标识符：`0x5000` 里的 `x5000` 满足 [A-Za-z_]\\w*，
      会被当成一个待解析的**标识符**，于是 resolve 失败、整条表达式判为
      "求不出值"。写本脚本时第一版就踩了这个坑，报错信息还是
      "DMM_CANCEL_MOVE = (DMM_MSG + 12) 求不出值（上游改了写法？）" ——
      把解析器缺陷说成了上游的问题。
    """
    t = m.group(0).rstrip("uUlL")
    return str(int(t, 16) if t[:2].lower() == "0x" else int(t, 10))


def eval_expr(expr, resolve, depth=0):
    """把一条 C 宏/常量表达式求成 32 位无符号整数；求不出来返回 None。

    支持：十进制/十六进制字面量（含 u/U/L 后缀）、TRUE/FALSE、WM_USER、
          标识符（递归解析）、+ - * / % << >> & | ^ ~ ( )。
    不支持就返回 None —— 由调用方判失败，**不猜**。
    """
    e = re.sub(r"\bTRUE\b", "1", expr)
    e = re.sub(r"\bFALSE\b", "0", e)
    e = re.sub(r"\b0[xX][0-9A-Fa-f]+[uUlL]*\b|\b\d+[uUlL]*\b", _literal_to_dec, e)
    for ident in sorted(set(re.findall(r"[A-Za-z_]\w*", e)), key=len, reverse=True):
        if ident == "WM_USER":
            continue
        sub = resolve(ident, depth + 1)
        if sub is None:
            return None
        e = re.sub(r"\b%s\b" % ident, str(sub), e)
    e = e.replace("WM_USER", str(WM_USER))
    if not re.fullmatch(r"[\d\s()+\-*/%<>&|^~]+", e):
        return None
    try:
        return int(eval(e, {"__builtins__": {}}, {})) & 0xFFFFFFFF
    except Exception:
        return None


def make_evaluator(exprs):
    """exprs: {名字: 表达式}；返回按需递归求值的 resolve(name, depth)。"""
    cache = {}

    def resolve(name, depth=0):
        if name in cache:
            return cache[name]
        if depth > 32 or name not in exprs:
            return None
        v = eval_expr(exprs[name], resolve, depth)
        if v is None:
            return None
        cache[name] = v
        return v

    return resolve


# ------------------------------------------------------------- 解析本地头文件

def parse_local_constants(path):
    """返回 (consts, unclassified)。

    consts:       [(名字, 表达式, 行号)]
    unclassified: [(行号, 原文)] —— 以 constexpr 开头却读不出"名=值"的行。
    """
    consts, unclassified = [], []
    for lineno, raw in enumerate(read_text(path).splitlines(), 1):
        code = strip_comments(raw).strip()
        if not code.startswith("constexpr"):
            continue
        m = RE_LOCAL_CONST.match(code)
        if not m:
            unclassified.append((lineno, raw.strip()))
            continue
        consts.append((m.group(1), m.group(2).strip(), lineno))
    return consts, unclassified


def extract_struct_body(text, name):
    """取出 `struct NAME { ... }` 的体（花括号配对，不靠正则回溯）。"""
    for m in re.finditer(r"\bstruct\s+%s\s*\{" % re.escape(name), text):
        start = text.index("{", m.start())
        depth = 0
        for j in range(start, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    return text[start + 1:j]
    return None


def parse_struct_fields(text, name):
    """返回 (fields, unclassified)；fields = [(字段名, 数组界或 None)]。"""
    body = extract_struct_body(strip_comments(text), name)
    if body is None:
        return None, []
    fields, unclassified = [], []
    for raw in body.splitlines():
        code = raw.strip()
        if not code:
            continue
        m = RE_FIELD.match(code)
        if not m:
            unclassified.append((0, raw.strip()))
            continue
        fields.append((m.group(1), m.group(2)))
    return fields, unclassified


# ------------------------------------------------------------- 解析上游头文件

def parse_upstream_defines(text):
    """{宏名: 表达式}。行首锚定 ⇒ `//#define X` 不会被当成定义。"""
    defines = {}
    for m in RE_DEFINE.finditer(strip_comments(text)):
        defines.setdefault(m.group(1), m.group(2).strip())
    return defines


def refresh(src_dir):
    paths = {}
    for key, (_repo_path, fname) in UPSTREAM.items():
        p = os.path.join(src_dir, fname)
        if not os.path.isfile(p):
            print("找不到上游文件：%s" % p.replace("\\", "/"))
            print("\n怎么拿到它们（需要网络；沙箱里 curl 可直连 raw.githubusercontent）：")
            for k, (repo_path, fn) in UPSTREAM.items():
                print("  curl -sS -o %s/%s %s%s"
                      % (src_dir.replace("\\", "/"), fn, RAW, repo_path))
            return 2
        paths[key] = p

    texts = {k: read_text(p) for k, p in paths.items()}
    defines = {k: parse_upstream_defines(t) for k, t in texts.items()}

    # 常量：把三个文件的宏并成一张表，按名字求值。
    all_exprs = {}
    for k in ("docking", "resource"):
        all_exprs.update(defines[k])
    resolve = make_evaluator(all_exprs)

    facts, origin = {}, {}
    wanted = {}
    for local, (up, src) in PAIRING.items():
        wanted[up] = src
    for up, src in sorted(wanted.items()):
        if up not in all_exprs:
            print("解析上游失败：%s 在 %s 里找不到（上游改名/删了？）"
                  % (up, UPSTREAM[src][0]))
            return 2
        v = resolve(up)
        if v is None:
            print("解析上游失败：%s = `%s` 求不出值（上游改了写法？）"
                  % (up, all_exprs[up]))
            return 2
        facts[up] = v
        origin[up] = UPSTREAM[src][0]

    if len(facts) != EXPECT_CONSTS:
        print("解析上游失败：解出 %d 条常量，期望 %d 条 —— 宁可失败也别写一张残缺的表"
              % (len(facts), EXPECT_CONSTS))
        return 2

    # 结构体字段序
    structs = {}
    for (local_key, local_name), (src, up_name, _pair) in STRUCTS.items():
        fields, unclassified = parse_struct_fields(texts[src], up_name)
        if fields is None:
            print("解析上游失败：%s 里找不到 struct %s" % (UPSTREAM[src][0], up_name))
            return 2
        if unclassified:
            print("解析上游失败：%s 的 struct %s 有 %d 行读不出字段"
                  % (UPSTREAM[src][0], up_name, len(unclassified)))
            for _ln, t in unclassified:
                print("    %s" % t)
            return 2
        structs[up_name] = [f[0] for f in fields]

    if len(structs) != EXPECT_STRUCTS:
        print("解析上游失败：解出 %d 个结构体，期望 %d 个" % (len(structs), EXPECT_STRUCTS))
        return 2

    m = RE_MENUITEMSIZE.search(texts["iface"])
    if not m:
        print("解析上游失败：PluginInterface.h 里找不到 menuItemSize")
        return 2
    item_name_size = int(m.group(1))

    doc = {
        "_comment": [
            "上游 Notepad++ 插件 ABI 契约的冻结事实表。",
            "由 scripts/check-npp-abi-contract.py --refresh <目录> 生成，**不要手改**。",
            "接口数值与结构体字段序属于事实；本表只记录事实，不含上游代码或注释。",
            "守卫 check-npp-abi-contract.py 用本表核对 src/plugin/npp/NppDocking.h",
            "与 src/plugin/npp/NppCompat.h。",
        ],
        "provenance": {
            "repo": "notepad-plus-plus/notepad-plus-plus",
            "files": {
                k: {"path": UPSTREAM[k][0], "url": RAW + UPSTREAM[k][0],
                    "file_sha256": sha256_of(paths[k])}
                for k in sorted(UPSTREAM)
            },
            "counts": {"constants": len(facts), "structs": len(structs)},
            "func_item_name_size": item_name_size,
        },
        "constants": dict(sorted(facts.items())),
        "constant_origin": dict(sorted(origin.items())),
        "structs": {k: structs[k] for k in sorted(structs)},
    }
    with open(FACTS, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2, sort_keys=False)
        fh.write("\n")
    print("已重建 %s：常量 %d 条 / 结构体 %d 个（docking %s，resource %s，iface %s）"
          % (FACTS.replace("\\", "/"), len(facts), len(structs),
             doc["provenance"]["files"]["docking"]["file_sha256"][:12],
             doc["provenance"]["files"]["resource"]["file_sha256"][:12],
             doc["provenance"]["files"]["iface"]["file_sha256"][:12]))
    return 0


# ------------------------------------------------------------------------ 主流程

def main(argv):
    _force_utf8_stdio()

    if "--refresh" in argv:
        i = argv.index("--refresh")
        src = argv[i + 1] if i + 1 < len(argv) else os.path.join("temp", "_upstream")
        root = repo_root()
        if root is None:
            return 2
        os.chdir(root)
        return refresh(src)

    verbose = "-v" in argv or "--verbose" in argv

    root = repo_root()
    if root is None:
        return 2
    os.chdir(root)

    if not os.path.isfile(FACTS):
        print("找不到事实表 %s —— 用 --refresh 生成" % FACTS.replace("\\", "/"))
        return 2
    doc = json.loads(read_text(FACTS))
    facts = doc["constants"]
    origin = doc["constant_origin"]
    up_structs = doc["structs"]
    item_name_size = doc["provenance"]["func_item_name_size"]

    problems = []

    # ---------------- R0 + R1 + R2：常量 ----------------
    consts, unclassified = parse_local_constants(HEADER_DOCK)
    compat_consts, compat_unclassified = parse_local_constants(HEADER_COMPAT)

    if unclassified:
        problems.append(("R0", "%s 的常量区有 %d 行无法归类："
                         % (HEADER_DOCK.replace("\\", "/"), len(unclassified)),
                         ["%d  %s" % (ln, t) for ln, t in unclassified]))
    if compat_unclassified:
        problems.append(("R0", "%s 有 %d 行 constexpr 无法归类："
                         % (HEADER_COMPAT.replace("\\", "/"), len(compat_unclassified)),
                         ["%d  %s" % (ln, t) for ln, t in compat_unclassified]))

    # ★ "项数本身就是断言"：解析器读不到东西时，下面所有规则都会空真通过。
    if len(consts) != EXPECT_CONSTS:
        problems.append(("R0", "%s 只解析出 %d 个常量，期望 %d 个 —— 解析器很可能"
                         "跟不上文件格式（那样报的 CLEAN 没有意义）"
                         % (HEADER_DOCK.replace("\\", "/"), len(consts), EXPECT_CONSTS),
                         []))
    # NppCompat.h 现在一个契约常量都没有。这一条是"永远亮的位"：它一旦变红，
    # 说明有人往那里加了常量，必须显式决定要不要配对上游。
    if compat_consts:
        problems.append(("R0", "%s 出现了 %d 个 constexpr，但本守卫的配对表里没有 ——"
                         "请把它们分类（配对上游名，或加进 LOCAL_ONLY）"
                         % (HEADER_COMPAT.replace("\\", "/"), len(compat_consts)),
                         ["%d  %s" % (ln, name) for name, _e, ln in compat_consts]))

    local_exprs = {name: expr for name, expr, _ln in consts}
    resolve_local = make_evaluator(local_exprs)

    values = {}
    for name, expr, lineno in consts:
        v = resolve_local(name)
        if v is None:
            problems.append(("R0", "%s:%d  %s = `%s` 求不出值"
                             % (HEADER_DOCK.replace("\\", "/"), lineno, name, expr), []))
            continue
        values[name] = v
        if name in LOCAL_ONLY:
            continue
        if name not in PAIRING:
            problems.append(("R0", "%s:%d  %s 既没配对上游、也不在 LOCAL_ONLY 里"
                             "（无法归类 = 失败）"
                             % (HEADER_DOCK.replace("\\", "/"), lineno, name), []))
            continue
        up, _src = PAIRING[name]
        if up not in facts:
            problems.append(("R1", "%s:%d  %s 配对的上游名 %s 在事实表里不存在"
                             % (HEADER_DOCK.replace("\\", "/"), lineno, name, up), []))
        elif facts[up] != v:
            problems.append(("R2", "%s:%d  %s = %d，上游 %s（%s）= %d"
                             % (HEADER_DOCK.replace("\\", "/"), lineno, name, v,
                                up, origin.get(up, "?"), facts[up]), []))

    # ---------------- R3 + R4：结构体字段序 ----------------
    header_of = {"dock": HEADER_DOCK, "compat": HEADER_COMPAT}
    for (local_key, local_name), (src, up_name, pair) in sorted(STRUCTS.items()):
        hpath = header_of[local_key]
        local_fields, bad = parse_struct_fields(read_text(hpath), local_name)
        if local_fields is None:
            problems.append(("R0", "%s 里找不到 struct %s"
                             % (hpath.replace("\\", "/"), local_name), []))
            continue
        if bad:
            problems.append(("R0", "%s 的 struct %s 有 %d 行读不出字段"
                             % (hpath.replace("\\", "/"), local_name, len(bad)),
                             ["%s" % t for _ln, t in bad]))
            continue
        if up_name not in up_structs:
            problems.append(("R0", "事实表里没有 struct %s" % up_name, []))
            continue
        up_fields = up_structs[up_name]

        local_names = [f[0] for f in local_fields]
        if pair is None:
            mapped = local_names
        else:
            unpaired = [n for n in local_names if n not in pair]
            if unpaired:
                problems.append(("R4", "%s 的 struct %s 有 %d 个字段没进配对表：%s"
                                 % (hpath.replace("\\", "/"), local_name,
                                    len(unpaired), ", ".join(unpaired)), []))
                continue
            mapped = [pair[n] for n in local_names]

        if mapped != up_fields:
            detail = []
            for i in range(max(len(mapped), len(up_fields))):
                a = mapped[i] if i < len(mapped) else "<缺>"
                b = up_fields[i] if i < len(up_fields) else "<多>"
                if a != b:
                    detail.append("第 %d 个字段：我们 %s，上游 %s" % (i + 1, a, b))
            if len(mapped) != len(up_fields):
                detail.append("字段数：我们 %d，上游 %d" % (len(mapped), len(up_fields)))
            problems.append(("R3", "%s 的 struct %s 字段序与上游不一致"
                             % (hpath.replace("\\", "/"), local_name), detail))

        # itemName 的数组界：上游由 menuItemSize 给出，我们写死 64。
        if local_name == "FuncItem":
            bound = dict(local_fields).get("itemName")
            if bound != str(item_name_size):
                problems.append(("R2", "%s 的 FuncItem::itemName 数组界是 %s，上游"
                                 " menuItemSize = %d"
                                 % (hpath.replace("\\", "/"), bound, item_name_size), []))

    if verbose:
        print("仓库：%s" % root)
        print("事实表：%s（常量 %d / 结构体 %d；docking %s）"
              % (FACTS.replace("\\", "/"), len(facts), len(up_structs),
                 doc["provenance"]["files"]["docking"]["file_sha256"][:16]))
        print("本地常量：%d 条（配对 %d / 本地私有 %d）"
              % (len(consts), sum(1 for n, _e, _l in consts if n in PAIRING),
                 sum(1 for n, _e, _l in consts if n in LOCAL_ONLY)))
        print("结构体：%d 个（%s）"
              % (len(STRUCTS), ", ".join(sorted(n for _k, n in STRUCTS))))

    if problems:
        n = sum(1 + len(d) for _r, _msg, d in problems)
        print("RESULT: %d 处违规" % n)
        for rule, msg, detail in problems:
            print("  %s %s" % (rule, msg))
            for line in detail:
                print("      %s" % line)
        print("\n说明：抄错常量/字段序的症状是**静默**的 ——")
        print("      常量抄错 ⇒ 宿主用错误的位解码 uMask、把 DMM_MOVE 派发成别的动作；")
        print("      字段序抄错 ⇒ 宿主读到的面板标题/命令 id 是隔壁字段的内容"
              "（\"注册成功但名字乱码\"）。")
        print("      两者都不崩、不报错、返回值看起来正常，所以必须对着权威源按名字核对。")
        return 1

    print("RESULT: CLEAN —— 常量 %d 个与上游一致（docking %d / resource %d）；"
          "结构体 %d 个字段序一致（%s）"
          % (len(consts),
             sum(1 for n, _e, _l in consts
                 if n in PAIRING and PAIRING[n][1] == "docking"),
             sum(1 for n, _e, _l in consts
                 if n in PAIRING and PAIRING[n][1] == "resource"),
             len(STRUCTS), ", ".join(sorted(n for _k, n in STRUCTS))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
