#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-lang-keys.py 的自测：证明每条规则都真的承重。

为什么需要它：
    守卫本身也会腐烂。R1/R6/R7/R8 的每条规则此前都是靠"手工变异 + 跑守卫 + 看它变红
    + 还原"验证的，但那些变异脚本是 temp/ 下的一次性产物，跑完就删 —— 下次改守卫时，
    没有任何东西能证明旧规则还承重：一条被改瞎的规则照样打印 "RESULT: CLEAN"。
    scripts/_profile-guard-selftest.ps1 已经为 profile 守卫立了这个模式，本文件把它
    搬到语言键守卫上。

做法（关键：**不碰真实工作树**）：
    1) check-lang-keys.py 新增 `--root`，可以直接对一棵树跑、不再依赖 git 探测；
    2) 本脚本把仓库的 `src/` 与 `resources/lang/` 复制成一棵"克隆树"（放临时目录）；
    3) 每个用例只改克隆树里的一份文件（或删掉一份白名单命中的文件），断言守卫
       **退出码 == 1** 且输出里出现**预期那条规则**的文案 —— 只断言"变红"是不够的，
       红错规则等于规则没工作；
    4) 用例跑完立刻从字节快照还原，并断言还原后与快照逐字节一致；
    5) 全程对真实仓库的守卫脚本与若干真实文件做 SHA256 前后比对 —— 克隆测试绝不能
       把真实工作树带脏；
    6) 正向控制：未变异的克隆树必须 exit 0。否则后面那些"红"什么也说明不了 ——
       红可能是克隆树本来就脏，而不是变异造成的。

为什么"项数即断言"那几条要改**守卫的副本**：
    CJK_ALLOW_COUNT / NARROW_ALLOW_COUNT / SAME_CN_COUNT / SIMP_ONLY_COUNT 是脚本里的
    常量，克隆树改不动它。所以这四条把守卫复制到临时目录、就地改掉常量再跑 —— 改的是
    副本，真实脚本的 SHA256 前后必须一致。

用法（用仓库配的那个 python，不是 WindowsApps 的空壳）：
    <python> scripts/_lang-keys-selftest.py        # 跑全部用例
    <python> scripts/_lang-keys-selftest.py -v     # 同时打印出问题的那些行

退出码：0 = 全部通过；1 = 有断言失败；2 = 环境问题（克隆失败 / 守卫不可用）。
"""

import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUARD = os.path.join(REPO, "scripts", "check-lang-keys.py")
PY = sys.executable

# 导入守卫只是为了读它的事实表，但 importlib 会顺手在 scripts/ 下落一个 __pycache__
# —— 一次自测不该在工作树里留垃圾（.gitignore 也没忽略 pyc）。关掉写字节码。
sys.dont_write_bytecode = True

# 克隆树里要复制的两处（守卫真正读的东西）。刻意**不**整仓复制：resources/ 下还有
# 图标等大文件，守卫一个也不看。
COPY_DIRS = ["src", os.path.join("resources", "lang")]

# 真实工作树里用来做"没被动过"比对的样本文件：守卫自己 + 三份最容易被顺手改的文件。
WITNESSES = [
    os.path.join("scripts", "check-lang-keys.py"),
    os.path.join("src", "app", "MainWindow.cpp"),
    os.path.join("src", "plugin", "PluginManager.cpp"),
    os.path.join("resources", "lang", "zh-TW.json"),
]

VERBOSE = "-v" in sys.argv[1:] or "--verbose" in sys.argv[1:]

_fail = 0


def say(s):
    print(s)


def check(what, ok, detail=""):
    global _fail
    if ok:
        say("  ok   " + what)
    else:
        _fail += 1
        say("  FAIL " + what + (("  << " + detail) if detail else ""))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_guard_module():
    """把守卫当模块导入，只为读它的事实表（模块级只有常量与函数，导入无副作用）。"""
    spec = importlib.util.spec_from_file_location("_clk_under_test", GUARD)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_guard(clone, guard_path=GUARD):
    proc = subprocess.run(
        [PY, guard_path, "--root", clone],
        cwd=REPO,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout


def sub_once(pattern, repl, text, label):
    """替换必须真的发生 —— 否则用例是在"没变异"的状态下跑，红绿都没有意义。"""
    new, n = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if n != 1:
        raise AssertionError("变异没生效（%s）：找不到 %r" % (label, pattern))
    return new


def sub_all(pattern, repl, text, label):
    """同 sub_once，但要求替换**每一处**：同一段文本可能出现多次（例如 CMake 里
    `lang/ja.json` 同时出现在源路径与目标路径里），只改一处等于没改。"""
    new, n = re.subn(pattern, repl, text, flags=re.MULTILINE)
    if n < 1:
        raise AssertionError("变异没生效（%s）：找不到 %r" % (label, pattern))
    return new


def read_text(path):
    """读取文本，并把**行尾统一成 LF**。

    变异用的正则里有 `^...$`（例如改 COUNT_CASES 的 `CJK_ALLOW_COUNT = N`）。
    本仓没有 .gitattributes，`core.autocrlf=true` ⇒ 同一次检出在不同环境可能拿到
    LF 或 CRLF；原样保留 CRLF 时，`$` 前的那个 `\\r` 会让 `[ \\t]*$` 匹配不上，
    变异"没生效"，自测当场抛 AssertionError。**实测**：本机与 CI 都因此红过
    （同一行 `sub_once(r"^%s[ \\t]*=[ \\t]*(\\d+)[ \\t]*$" ...)`）。统一成 LF 后
    判据与行尾无关。
    """
    with open(path, "rb") as fh:
        return (fh.read().decode("utf-8")
                .replace("\r\n", "\n").replace("\r", "\n"))


def write_text(path, text):
    with open(path, "wb") as fh:
        fh.write(text.encode("utf-8"))


def apply_edits(clone, edits):
    """按用例表改克隆重，返回字节快照（还原用）。"""
    snaps = []
    for rel, mutate in edits:
        path = os.path.join(clone, rel)
        snaps.append((path, open(path, "rb").read()))
        if mutate is None:
            os.remove(path)
        else:
            write_text(path, mutate(read_text(path)))
    return snaps


def restore(clone, snaps):
    for path, data in snaps:
        with open(path, "wb") as fh:
            fh.write(data)


# R7 的变异（模块级常量：主用例表和"自测的负控"两处都要用同一个）。
R7_NARROW_EDIT = ("src/language/Chroma3380Diagnostics.cpp",
                  lambda t: t + '\nstatic const char* kNcProbeNarrow = "窄串硬编码的中文";\n')


# ---------------------------------------------------------------------------
# 用例表
#
# 每项：(名称, 期望退出码, 期望输出片段列表, [(相对路径, 变异函数 | None)])
#   变异函数收原文返回新文；None 表示删除该文件。所有被触碰的文件都会先做字节快照。
#   路径以 "<G>" 开头表示"守卫脚本自己"，会被拷成临时副本改，不动真身。
# ---------------------------------------------------------------------------
def build_cases(clone, guard_mod):
    cn = json.loads(read_text(os.path.join(clone, "resources", "lang", "zh-CN.json")))
    tw = json.loads(read_text(os.path.join(clone, "resources", "lang", "zh-TW.json")))

    # R8(b) 用：找一个 zh-CN / zh-TW 取值不同、且 zh-CN 侧含汉字的键（两边不同 ⇒
    # 必然不在 SAME_CN 里，改成逐字相同就一定会被点名）。
    cjk = guard_mod.RE_CJK
    diff_key = next(k for k in sorted(set(cn) & set(tw))
                    if isinstance(cn[k], str) and isinstance(tw[k], str)
                    and cn[k] != tw[k] and cjk.search(cn[k]))

    # R8(c) 用：任取一个简体独有字形。真实仓 R8(c) 是 CLEAN 的，所以它必然不在
    # 任何 zh-TW 取值里；直接追加到某个值上就一定触发。
    simp_char = "关" if "关" in guard_mod.SIMP_ONLY else sorted(guard_mod.SIMP_ONLY)[0]
    any_key = sorted(k for k in tw if isinstance(tw[k], str))[0]

    # R8(a) 用：任取一个两边都有的键（删了它就一定触发"键集合不一致"）。刻意不写死
    # 键名 —— 写死的话，键改名后用例会**静默变成空操作**（删不存在的键 = 没改文件），
    # 于是"期望变红"的断言失败或空转，两种都不是我们想要的。
    drop_key = sorted(cn)[0]

    cases = []

    # --- R1 ---
    cases.append((
        "R1 调用点：Tr() 引用了不存在的键", 1,
        ["R1 引用了", "panel.zzz.notexist"],
        [("src/app/MainWindow.cpp",
          lambda t: t + '\nvoid NcProbeCall() { (void)Tr(L"panel.zzz.notexist"); }\n')],
    ))
    cases.append((
        "R1 数据表项：{L\"key\", ...} 引用了不存在的键", 1,
        ["R1 引用了", "panel.zzz.notexist"],
        [("src/app/MainWindow.cpp",
          lambda t: t + '\nstatic const struct { const wchar_t* k; int v; } kNcProbeTab[]'
                    ' = { {L"panel.zzz.notexist", 0} };\n')],
    ))

    # --- R6 ---
    cases.append((
        "R6 宽串里硬编码中文", 1,
        ["R6 ", "宽串里硬编码了中文"],
        [("src/app/MainWindow.cpp",
          lambda t: t + '\nstatic const wchar_t* kNcProbeWide = L"硬编码的中文";\n')],
    ))
    cases.append((
        "R6 掩码：注释里的中文/字面量不算（必须仍是 CLEAN）", 0,
        [],
        [("src/app/MainWindow.cpp",
          lambda t: t + '\n// NcProbe: 这行注释里的中文和 L"中文" 都不该被报\n'
                    'static const wchar_t* kNcProbeMask = L"plain";\n')],
    ))
    cases.append((
        "R6 白名单陈旧：命中项所在的文件被删掉", 1,
        ["R6 白名单陈旧", "src/ai/AiPanel.cpp"],
        [("src/ai/AiPanel.cpp", None)],
    ))

    # --- R7 ---
    cases.append((
        "R7 窄串里硬编码中文", 1,
        ["R7 ", "窄串里硬编码了中文"],
        [R7_NARROW_EDIT],
    ))
    cases.append((
        "R7 白名单陈旧：命中项所在的文件被删掉", 1,
        ["R7 白名单陈旧", "src/plugin/PluginManager.cpp"],
        [("src/plugin/PluginManager.cpp", None)],
    ))

    # --- R3 / R4 / R5 ---
    cases.append((
        "R3 语言文件与 kUiLangs[] 不一致", 1,
        ["R3 ", "kUiLangs[]"],
        [("src/app/PreferencesDialog.cpp",
          lambda t: sub_once(r'L"ja"', 'L"jax"', t, "kUiLangs 改语言 id"))],
    ))
    cases.append((
        "R4 kUiLangCount 与实际项数不符", 1,
        ["R4 kUiLangCount"],
        [("src/app/PreferencesDialog.cpp",
          lambda t: sub_once(r"kUiLangCount\s*=\s*(\d+)",
                             lambda m: "kUiLangCount = %d" % (int(m.group(1)) - 1),
                             t, "kUiLangCount 减一"))],
    ))
    cases.append((
        "R5 语言文件没有拷贝到 exe 旁的规则", 1,
        ["R5 ", "ja.json"],
        [("src/CMakeLists.txt",
          lambda t: sub_all(r"lang/ja\.json", "lang/jaX.json", t, "删掉 ja 的拷贝规则"))],
    ))

    # --- R8 ---
    cases.append((
        "R8(a) zh-TW 少了一个键（%s）" % drop_key, 1,
        ["R8 ", drop_key],
        [("resources/lang/zh-TW.json",
          lambda t: json.dumps({k: v for k, v in json.loads(t).items()
                                if k != drop_key},
                               ensure_ascii=False, indent=2))],
    ))
    cases.append((
        "R8(b) zh-TW 某键整条抄了 zh-CN 原文（%s）" % diff_key, 1,
        ["R8 ", "逐字相同且含汉字", diff_key],
        [("resources/lang/zh-TW.json",
          lambda t: json.dumps(dict(json.loads(t), **{diff_key: cn[diff_key]}),
                               ensure_ascii=False, indent=2))],
    ))
    cases.append((
        "R8(c) zh-TW 取值里出现简体独有字形「%s」" % simp_char, 1,
        ["R8 ", "含简体字形", any_key],
        [("resources/lang/zh-TW.json",
          lambda t: json.dumps(dict(json.loads(t),
                                    **{any_key: tw[any_key] + simp_char}),
                               ensure_ascii=False, indent=2))],
    ))

    return cases


# 「项数即断言」四条：改的是**守卫的副本**，真实脚本不能被碰。
COUNT_CASES = [
    ("CJK_ALLOW_COUNT", "R6 白名单项数"),
    ("NARROW_ALLOW_COUNT", "R7 白名单项数"),
    ("SAME_CN_COUNT", "R8 SAME_CN 项数"),
    ("SIMP_ONLY_COUNT", "R8 SIMP_ONLY 项数"),
]


def main():
    tmp = tempfile.mkdtemp(prefix="xfs-langkeys-selftest-")
    clone = os.path.join(tmp, "clone")
    try:
        say("repo   = " + REPO)
        say("guard  = " + GUARD)
        say("clone  = " + clone)

        # 克隆树绝不能落在仓库里，否则"没碰真实工作树"这句话就不成立了。
        # 不同盘符时 commonpath 会抛 ValueError —— 那本身就说明不在同一棵树里。
        try:
            inside = os.path.commonpath([clone, REPO]) == REPO
        except ValueError:
            inside = False
        if inside:
            say("SELFTEST-BROKEN: 克隆树落在仓库里了 —— 拒绝继续")
            return 2

        witnesses_before = {w: sha256(os.path.join(REPO, w)) for w in WITNESSES}

        # ---- 建房 ----
        os.makedirs(clone)
        for rel in COPY_DIRS:
            dst = os.path.join(clone, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copytree(os.path.join(REPO, rel), dst)

        guard_mod = load_guard_module()

        # ---- C0 正向控制：干净克隆树必须 CLEAN ----
        rc, out = run_guard(clone)
        say("")
        say("[C0] 正向控制：未变异的克隆树")
        check("干净克隆树 exit 0（否则后面的红都说明不了什么）", rc == 0,
              "exit=%d，输出：\n%s" % (rc, out.strip()[:2000]))
        if rc == 0:
            check("输出里是 CLEAN", "RESULT: CLEAN" in out, out.strip()[:400])

        # ---- C1 默认路径没被 --root 改坏：对真实仓跑（不传 --root）----
        proc = subprocess.run([PY, GUARD], cwd=REPO, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, encoding="utf-8",
                              errors="replace")
        say("")
        say("[C1] 不带 --root（走 git 探测）对真实仓")
        check("真实仓 exit 0（本批改守卫不能把它弄红）", proc.returncode == 0,
              "exit=%d，输出：\n%s" % (proc.returncode, proc.stdout.strip()[:2000]))

        # ---- 逐条负控 ----
        for name, want_exit, wants, edits in build_cases(clone, guard_mod):
            snaps = apply_edits(clone, edits)

            rc, out = run_guard(clone)
            say("")
            say("[NC] " + name)
            check("exit == %d（实际 %d）" % (want_exit, rc), rc == want_exit,
                  out.strip()[:1200])
            for want in wants:
                check("输出里出现 %r" % want, want in out, out.strip()[:1200])
            if VERBOSE and rc != 0:
                for line in out.strip().splitlines():
                    if line.startswith("  R"):
                        say("       " + line.strip())

            restore(clone, snaps)
            for path, data in snaps:
                check("克隆重已逐字节还原：%s" % os.path.relpath(path, clone),
                      sha256(path) == hashlib.sha256(data).hexdigest())

        # ---- 自测的负控：把守卫改瞎，自测必须能发现 ----
        #
        # 这是 _profile-guard-selftest.ps1 的 -Case nc 的等价物。上面每个用例都断言
        # "变异 ⇒ 变红"，但如果**那条规则根本没在跑**，红仍可能来自别的地方（同一次
        # 变异顺带触发了别的规则）。所以这里把 R7 的报告语句摘掉、重跑同一个变异，
        # 断言"改瞎的守卫**抓不到**它" —— 抓不到，才能证明上面那次红是 R7 给的。
        neutered = os.path.join(tmp, "guard-neutered.py")
        write_text(neutered, sub_once(r"problems\.append\('R7 %s:%d",
                                      "(lambda *_a: None)('R7 %s:%d",
                                      read_text(GUARD), "摘掉 R7 的报告语句"))
        snaps = apply_edits(clone, [R7_NARROW_EDIT])
        rc, out = run_guard(clone, neutered)
        restore(clone, snaps)
        say("")
        say("[SELF-NC] R7 的报告语句被摘掉后，同一个变异必须**不再**变红")
        check("改瞎的守卫 exit 0（说明它真的没在报）", rc == 0, out.strip()[:800])
        check("改瞎的守卫输出里没有 R7 违规", "\n  R7 " not in out, out.strip()[:800])
        check("克隆重已逐字节还原", all(
            sha256(p) == hashlib.sha256(d).hexdigest() for p, d in snaps))

        # ---- 项数即断言：改守卫的副本 ----
        for const, want in COUNT_CASES:
            src = read_text(GUARD)
            new = sub_once(r"^%s[ \t]*=[ \t]*(\d+)[ \t]*$" % const,
                           lambda m: "%s = %d" % (const, int(m.group(1)) - 1),
                           src, const)
            patched = os.path.join(tmp, "guard-%s.py" % const)
            write_text(patched, new)
            rc, out = run_guard(clone, patched)
            say("")
            say("[NC] %s 改错 ⇒ %s" % (const, want))
            check("exit == 1（实际 %d）" % rc, rc == 1, out.strip()[:1200])
            check("输出里出现 %r" % want, want in out, out.strip()[:1200])

        # ---- 真实工作树必须纹丝不动 ----
        say("")
        say("[C2] 真实工作树完整性")
        for w in WITNESSES:
            now = sha256(os.path.join(REPO, w))
            check("未改动：%s" % w, now == witnesses_before[w],
                  "before=%s after=%s" % (witnesses_before[w][:16], now[:16]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    say("")
    if _fail == 0:
        say("VERDICT: LANG-KEYS-SELFTEST-PASS")
        return 0
    say("VERDICT: LANG-KEYS-SELFTEST-FAIL（%d 项断言未过）" % _fail)
    return 1


if __name__ == "__main__":
    sys.exit(main())
