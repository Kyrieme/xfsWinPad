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
    R6  src/ 下**宽字符串字面量**里不许出现 CJK（汉字 / 假名 / 全角标点）。
        Windows 上的界面文案一律走宽字符（CreateWindowExW / SetWindowTextW /
        MessageBoxW），所以"宽串含 CJK"几乎必然是**绕过 Tr() 的硬编码文案** ——
        换语言时它不会跟着变，而编译器和 R1 都看不见它。

        ⚠️ 覆盖面边界（同 R1，诚实说明，别当成"全查过了"）：
           * **窄串不看** —— 那是 R7 的地盘（见下）。
           * 注释与 raw 字符串不算（不是要显示给用户的东西）。
           * 只看字面量本身；拼接出来的文案里，每个字面量仍会被逐个查到。
           * 白名单按 (路径, 字面量正文) 精确匹配，**每条都必须仍然命中**
             （陈旧 ⇒ 报红），且**项数本身就是断言**（CJK_ALLOW_COUNT）。
    R7  src/ 下**非宽串**（即前缀不以 L 结尾）的字符串字面量里不许出现 CJK。
        为什么会存在这种字面量：内核层（src/language、src/bigfile 等）有一条硬
        约束 —— **不引 I18n.h**，以保持分层。那里的诊断/错误因此不产出文案，只
        报"是哪一类 + 参数"，由 UI 侧按 id 查语言键（批次 129 的 RunNote / Err、
        批次 134 的 Chroma DiagMsgId 都是这个套路）。窄串 CJK 只应剩在这些地方：
        日志文案、手册原文样例（生成文件里）、static_assert 编译期消息、以及
        插件没给分类名时的兜底串。R6 只看 L"..."，看不见它们；编译器和 R1 同样
        看不见。

        覆盖面边界（诚实说明）：
           * 只扫 src/。tests/ 不纳入 —— 测试里的中文是断言数据，不是界面文案。
           * 注释与 raw 字符串不算（复用 R6 的掩码器，不另写一套）。
           * 前缀以 L 结尾的字面量归 R6，本规则不重复报（两条合起来覆盖全部
             字符串字面量，raw 除外）。
           * 白名单按 (路径, 字面量正文) 精确匹配，每条都必须仍然命中
             （陈旧 ⇒ 报红），且项数本身就是断言（NARROW_ALLOW_COUNT）。
    R8  zh-TW 必须是 zh-CN 的**忠实繁中转换**。zh-TW 是简繁转换的产物（同形同义
        的机械映射，机器可核对），所以用"机器核对 + 显式点头"的方式守住它：
        (a) zh-TW 与 zh-CN 的**键集合必须一致**（只加进一份 ⇒ 另一份把 id 原样
            显示给用户）；
        (b) 取值与 zh-CN **逐字相同且含 CJK** 的键，必须在 SAME_CN 白名单里 ——
            那只能解释为"简繁同形"（"更新"、"移除"）；
        (c) 取值不得含 SIMP_ONLY 里的**简体独有字形**（如"关"⇒ 应为"關"）。

        ⚠️ 覆盖面边界（诚实说明，别当成"全查过了"）：
           * **只覆盖 zh-TW，不覆盖 ja / ko** —— 那两份是人工翻译，译文正确性
             无法自动判定（同批次 129 的如实标注）。
           * 防的是"新键只加进 zh-CN、zh-TW 里拷一份简中原文"这类**漏转**
             （批次 87 真实发生过）；它抓不出"繁中写得通顺但语义译错"。
           * 字形表精度优先：只收**出现在 zh-CN 语料里**的简体独有字，并剔除
             简繁两用字（台 / 后 / 划 / 于 在繁中合法）—— 宁可不报，不可误报。
           * SAME_CN 每条都必须仍然成立（不再逐字相同 ⇒ 陈旧报红）；SIMP_ONLY
             每个字都必须仍出现在 zh-CN 语料里（否则陈旧报红）；两张表的项数
             本身就是断言（SAME_CN_COUNT / SIMP_ONLY_COUNT）。
    （仅提示）en.json 里存在、但没有任何代码引用的键。可能是历史遗留，也可能是
      "键加了但忘了用"，两种情况都不构成缺陷，列出来供人工判断，不影响退出码。

    R3/R4/R5 的共同点：漏掉任何一处的后果都是**静默**的 ——
      有文件没进下拉 ⇒ 用户看不到该语言；
      有文件没拷贝规则 ⇒ exe 旁没有该 json，I18n::Load 失败后悄悄沿用旧语言；
      加了项没改计数 ⇒ 下拉里可见，选中后不生效。

用法：
    python scripts/check-lang-keys.py            # 检查（0 = 干净 / 1 = 有违规）
    python scripts/check-lang-keys.py -v         # 同时打印未被引用的键
    python scripts/check-lang-keys.py --root DIR # 对一棵**仓库副本**跑（不读 git）

    --root 是给自测用的：把 root 直接指到一棵复制出来的树，规则本身一行不改。
    **本脚本自己也要有回归** —— 每条规则是否真的承重，靠
    `scripts/_lang-keys-selftest.py`（克隆 src/ + resources/ 到临时目录、逐条变异、
    断言退出码与命中的规则名，全程不碰真实工作树）。改了本文件就跑它一次。

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

# ---- R6：UI 文案不许硬编码中文 -----------------------------------------------
#
# 宽字符串字面量（L"..."），正文允许 \" \\ 转义。窄串不匹配 —— 见 R6 盲点说明。
RE_WIDE_LITERAL = re.compile(r'\bL"((?:[^"\\]|\\.)*)"')
# CJK 汉字 / 假名 / 全角标点。出现即说明这是"给人看的文案"，不是内部标识符。
RE_CJK = re.compile(r"[\u3000-\u303F\u3040-\u30FF\u3400-\u4DBF"
                    r"\u4E00-\u9FFF\uF900-\uFAFF\uFF00-\uFFEF]")

# 允许硬编码的宽串（按 "相对路径" + 字面量正文 精确匹配）。
# 每条都必须**仍然命中**：文案删了却没同步这里 ⇒ 陈旧项报红。
# 加条目：跑一次守卫，它会打印 `R6 路径:行 宽串里硬编码了中文 "..."`，照抄正文即可。
CJK_ALLOW = {
    ("src/ai/AiPanel.cpp", "选中"),                       # 子串探测：探的就是界面文案
    ("src/log/LogPanel.cpp", "＋"),                       # 按钮符号，不是文案
    ("src/app/PreferencesDialog.cpp", "简体中文"),         # 语言下拉的母语标签：刻意不翻译
    ("src/app/PreferencesDialog.cpp", "繁體中文"),
    ("src/app/PreferencesDialog.cpp", "日本語"),
    ("src/language/CraftProject.cpp", "设置"),             # 仅测试探针用（ToolOriginName）
    ("src/language/CraftProject.cpp", "编译测试计划 "),     # 死数据：UI 从不渲染 BuildStep::label
    ("src/language/CraftProject.cpp", "编译向量 "),
    ("src/language/CraftProject.cpp", "链接向量目标"),
}
# 项数本身就是断言：删条目必须显式改这里，不允许"悄悄少一条"。
CJK_ALLOW_COUNT = 9

# ---- R7：内核层的窄串诊断文案不许硬编码中文 ---------------------------------
#
# 任意字符串字面量及其编码前缀（L / u / U / u8 / 空）。掩码器已把注释、字符
# 字面量、raw 字符串抹平，所以这里匹配到的都是**真代码里的字符串字面量**。
RE_ANY_STRING = re.compile(r'([A-Za-z0-9_]*)"((?:[^"\\]|\\.)*)"')

# 允许硬编码的窄串（按 "相对路径" + 字面量正文 精确匹配）。每条都必须仍然命中。
NARROW_ALLOW = {
    # ① 手册原文原样入库的示例（含中文占位）。本文件由 gen_cpp_db.py 生成，
    #    禁止手动修改 —— 改了下次重跑生成器就没了。
    ("src/language/Chroma3380Db.cpp",
     'TDO_PRINTF(\\"CSV_FIELD SITE=%d TITLE=%s CONTENT=%s 【TEST_NO=%d】\\\\n\\", SOCKET, \\"TDO_PRINTF\\", freq[SOCKET],(int) ;'),
    # ② 日志文案（Logger::Info / Error —— 落日志文件，不是界面文案）。
    ("src/plugin/DockManager.cpp", "DockManager: DockWidget 收到无效 hClient"),
    ("src/plugin/DockManager.cpp", "DockManager: 拒绝 hClient（既非本进程窗口，也不是受信任代理承载）"),
    ("src/plugin/PluginManager.cpp", "插件: "),
    # ③ 插件没给 category 时的兜底分类名。**这一条会显示在命令面板上** —— 登记在
    #    这里是为了让它可见，不是因为它是好写法（插件 API 走 UTF-8 窄串通道）。
    ("src/plugin/PluginManager.cpp", "插件"),
    # ④ static_assert 的编译期诊断消息（给开发者看，不随语言切换）。
    ("src/plugin/npp/NppCompat.h", "下面的偏移按 Win64 写死；换架构必须重新对着上游核对"),
    ("src/plugin/npp/NppCompat.h", "FuncItem 步长必须是 152（代理按步长寻址）"),
    ("src/plugin/npp/NppCompat.h", "ShortcutKey 必须无填充：顺序错位会把 key 误读成 modifier"),
    ("src/plugin/npp/NppCompat.h", "NppData 是三个连续指针"),
    ("src/plugin/npp/NppDocking.h", "下面的偏移按 Win64 写死；换架构必须重新对着上游核对"),
    ("src/plugin/npp/NppDocking.h", "DockedWidgetData 尾部无额外填充"),
    ("src/plugin/oop/DmmMarshal.h", "DmmRegHead 必须无填充（布局契约）"),
    ("src/plugin/oop/DmmMarshal.h", "DmmIconHead 必须无填充（布局契约）"),
    ("src/plugin/oop/DmmMarshal.h", "DmmTwoStrHead 必须无填充（布局契约）"),
    ("src/plugin/oop/SciBridge.h", "Sci_CharacterRange 是 2 × long"),
    ("src/plugin/oop/SciBridge.h", "Sci_CharacterRangeFull 是 2 × intptr_t"),
}
# 项数本身就是断言：删条目必须显式改这里。
NARROW_ALLOW_COUNT = 16

# ---- R8：zh-TW 必须是 zh-CN 的忠实繁中转换 ------------------------------------
#
# 为什么只查繁中：五份语言里 en / zh-CN 是母本，zh-TW 是**简繁转换**的产物（同形
# 同义的机械映射，机器可核对）；ja / ko 是人工翻译，质量无法自动验证 —— 本规则
# **不覆盖 ja / ko**（同批次 129 的如实标注）。要防的失效模式：新键只加进 zh-CN，
# zh-TW 里拷一份简中原文，界面切到繁體中文就露馅；批次 87 真实发生过（18 个键
# ja / ko / zh-TW 三份全缺，用户切过去才看见）。

ZH_CN = "zh-CN"
ZH_TW = "zh-TW"

# (b) zh-TW 与 zh-CN 取值**逐字相同**且含 CJK 的键 —— 只允许"简繁同形"这一类。
#     为什么需要它：整条拷过来的简中原文如果字形全是简繁同形（"更新"、"移除"），
#     (c) 的字形扫描看不见，只能靠"和白名单对不上"发现。
#     每条都必须**仍然成立**（不再逐字相同 ⇒ 陈旧报红）；项数本身就是断言。
SAME_CN = {
    "bigfile.lines",                  # 行
    "bigfile.statuslines",            # {0} 行
    "cmd.palette",                    # 命令面板
    "cmd.redo",                       # 重做
    "cmd.unknown",                    # 命令 {0}
    "cmd.zoomin",                     # 放大
    "enc.utf8bom",                    # 包含 BOM 的 UTF-8
    "git.branch.new",                 # 新建分支…
    "git.commit",                     # 提交…
    "git.fetch",                      # 抓取
    "git.pull",                       # 拉取
    "git.push",                       # 推送
    "input.cancel",                   # 取消
    "log.exclude",                    # 排除
    "log.savepreset",                 # 存
    "menu.edit.lineops.trim",         # 去除行尾空格(&T)
    "menu.edit.redo",                 # 重做(&R)
    "menu.macro.playback",            # 播放(&P)
    "menu.tools",                     # 工具(&T)
    "menu.view.theme.dark",           # 深色(&D)
    "menu.view.zoomin",               # 放大(&I)
    "pal.title",                      # 命令面板 (Ctrl+Shift+P)
    "panel.ai.abort",                 # 停止
    "panel.ai.ai",                    # AI：
    "panel.ai.you",                   # 我：
    "panel.compile.col.where",        # 位置
    "panel.compile.level.warning",    # 警告
    # 下面三条是纯格式串（只有全角标点，字形本来就简繁一致）
    "panel.compile.nocraft.crafthome",  # CRAFT_HOME：{0}
    "panel.compile.nocraft.patcmp",     # patcmp：{0}
    "panel.compile.nocraft.plncmp",     # plncmp：{0}
    "panel.compile.notfound",         # （未找到）
    "panel.compile.step.ok",          # 成功（{0} 毫秒）
    "panel.diag.col.line",            # 行
    "panel.diag.level.warning",       # 警告
    "panel.stdf.ai",                  # AI 分析
    "panel.terminal.fmt",             # {0} — {1}（{2}）
    "plugadmin.action.update",        # 更新
    "plugadmin.btn.remove",           # 移除(&R)
    "plugadmin.btn.update",           # 更新(&U)
    "plugadmin.col.desc",             # 描述
    "plugadmin.col.version",          # 版本
    "plugadmin.incompat.abi",         # ABI 版本不符
    "plugadmin.incompat.col.reason",  # 原因
    "plugadmin.tab.available",        # 可用
    "plugadmin.tab.updates",          # 更新
    "plugcmd.unnamed",                # (未命名)
    "prefs.aibackend.oc",             # opencode（agent 全功能）
    "prefs.aimodel",                  # 模型 ID：
    "prefs.caret1",                   # 1 像素
    "prefs.caret2",                   # 2 像素
    "prefs.caret3",                   # 3 像素
    "prefs.fontfmt",                  # {0}，{1}
    "prefs.theme.dark",               # 深色
    "sc.conflict.cmd",                # 命令 {0}
    "sc.itemfmt",                     # {0}（{1}）
    "sc.modify",                      # 修改…
    "sc.modify.cmdlabel",             # 命令：
    "stdf.col.max",                   # 最大值
    "stdf.col.mean",                  # 平均值
    "stdf.col.min",                   # 最小值
    "stdf.dl.hwbin",                  # 硬 bin
    "stdf.flt.all",                   # 全部
    "stdf.flt.allbin",                # 全部 HW Bin
    "stdf.flt.allsite",                # 全部 Site
    "stdf.good",                      # 良品
    "stdf.stat.hilim",                # 上限 (High)
    "stdf.stat.lolim",                # 下限 (Low)
    "stdf.stat.max",                  # 最大值
    "stdf.stat.min",                  # 最小值
    "stdf.stat.over",                 # 超限 (低/高)
    "winlist.col.size",               # 大小
    "winlist.view1",                  # 主
    "winlist.view2",                  # 右
}
SAME_CN_COUNT = 73

# (c) 简体独有字形表（简 → 常见繁体，繁体只用于报错提示）。
#     来源与派生：OpenCC `STCharacters.txt`（Apache-2.0）里取值 ≠ 键的字，去掉
#     `TSCharacters.txt` 的繁体键，再与 `resources/lang/zh-CN.json` 实际出现过的
#     字求交集 —— 只收"真的可能被抄进繁中的简中字"。
#     台 / 后 / 划 / 于 四个是**简繁两用字**（台灣、皇后、划船、于姓在繁中合法），
#     刻意剔除：精度优先，宁可不报不可误报（同 R1 / R6 的取舍）。
#     表里的每个字都必须**仍然出现在 zh-CN 语料里**（语料变了 ⇒ 陈旧报红，届时
#     重跑派生）；项数本身就是断言。
SIMP_ONLY = {
    "与": "與", "业": "業", "两": "兩", "个": "個", "为": "為", "义": "義",
    "书": "書", "产": "產", "仅": "僅", "从": "從", "会": "會", "体": "體",
    "侧": "側", "关": "關", "内": "內", "写": "寫", "冲": "衝", "准": "準",
    "凭": "憑", "击": "擊", "则": "則", "创": "創", "删": "刪", "别": "別",
    "务": "務", "动": "動", "区": "區", "单": "單", "占": "佔", "历": "歷",
    "压": "壓", "参": "參", "双": "雙", "发": "發", "叠": "疊", "号": "號",
    "吗": "嗎", "启": "啟", "围": "圍", "图": "圖", "圆": "圓", "场": "場",
    "墙": "牆", "处": "處", "备": "備", "复": "復", "头": "頭", "夹": "夾",
    "实": "實", "宽": "寬", "对": "對", "导": "導", "将": "將", "属": "屬",
    "带": "帶", "帮": "幫", "并": "並", "应": "應", "开": "開", "异": "異",
    "弃": "棄", "强": "強", "当": "當", "录": "錄", "径": "徑", "态": "態",
    "总": "總", "户": "戶", "执": "執", "扩": "擴", "扫": "掃", "报": "報",
    "择": "擇", "挂": "掛", "损": "損", "换": "換", "据": "據", "数": "數",
    "断": "斷", "无": "無", "时": "時", "显": "顯", "暂": "暫", "机": "機",
    "杂": "雜", "权": "權", "条": "條", "来": "來", "构": "構", "标": "標",
    "栏": "欄", "树": "樹", "样": "樣", "档": "檔", "检": "檢", "没": "沒",
    "浅": "淺", "测": "測", "浏": "瀏", "溃": "潰", "滤": "濾", "点": "點",
    "状": "狀", "独": "獨", "现": "現", "盖": "蓋", "盘": "盤", "码": "碼",
    "确": "確", "离": "離", "称": "稱", "签": "簽", "类": "類", "级": "級",
    "线": "線", "组": "組", "终": "終", "经": "經", "绑": "綁", "结": "結",
    "络": "絡", "绝": "絕", "统": "統", "继": "繼", "绪": "緒", "续": "續",
    "编": "編", "缩": "縮", "网": "網", "节": "節", "范": "範", "获": "獲",
    "补": "補", "装": "裝", "见": "見", "规": "規", "视": "視", "览": "覽",
    "计": "計", "认": "認", "记": "記", "设": "設", "访": "訪", "诊": "診",
    "词": "詞", "译": "譯", "试": "試", "话": "話", "该": "該", "语": "語",
    "误": "誤", "说": "說", "请": "請", "读": "讀", "调": "調", "败": "敗",
    "贮": "貯", "贴": "貼", "费": "費", "资": "資", "转": "轉", "软": "軟",
    "载": "載", "较": "較", "辑": "輯", "输": "輸", "边": "邊", "达": "達",
    "过": "過", "运": "運", "还": "還", "进": "進", "远": "遠", "违": "違",
    "连": "連", "选": "選", "释": "釋", "钮": "鈕", "链": "鏈", "销": "銷",
    "锁": "鎖", "错": "錯", "键": "鍵", "镜": "鏡", "长": "長", "闭": "閉",
    "问": "問", "间": "間", "队": "隊", "随": "隨", "静": "靜", "页": "頁",
    "项": "項", "预": "預", "题": "題", "颜": "顏", "驱": "驅", "骤": "驟",
}
SIMP_ONLY_COUNT = 198


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


def _ident_prefix(text, i):
    """往回吃掉 i 之前的标识符字符，返回前缀串。

    用来判断 i 处的 `'` / `"` 到底是什么：`L'x'` 的 `'` 前面是编码前缀 `L`（算字面量），
    而 `1'000'000` 的 `'` 前面是数字 `1`（数字分隔符，不算字面量）。
    """
    k = i
    while k > 0 and (text[k - 1].isalnum() or text[k - 1] == "_"):
        k -= 1
    return text[k:i]


# char 字面量允许的编码前缀；raw 字符串同理。
CHAR_PREFIXES = ("", "L", "u", "U", "u8")
RAW_PREFIXES = ("R", "LR", "uR", "UR", "u8R")


def mask_noncode(text):
    """把注释 / 字符字面量 / raw 字符串抹成空格（保长度、保留换行）。

    R6 只在"真代码"里找宽串字面量：注释里写着的 L"中文" 只是说明文字，
    raw 字符串是数据块（插件目录等），都不是要显示给用户的东西。
    抹成等长空格而不是删掉，是为了让匹配偏移仍然等于原文偏移 —— 行号才好数。
    """
    out = list(text)
    n = len(text)

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    i = 0
    while i < n:
        c = text[i]
        if c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j == -1 else j
            blank(i, j)
            i = j
            continue
        if c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            blank(i, j)
            i = j
            continue
        # 字符字面量：'x' / '\'' / L'"'。**必须按前缀判断**，不能只看"前一个字符
        # 是不是标识符字符" —— L'"' 的 ' 前面正是标识符字符 L。
        # 漏判它的代价极大：里面的 " 会被当成字符串起始，掩码器从此与源码失步，
        # 一路吞掉后面的宽串（实测把 CraftProject.cpp 的三条中文 label 全掩掉了）。
        if c == "'" and _ident_prefix(text, i) in CHAR_PREFIXES:
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            blank(i, min(j + 1, n))
            i = min(j + 1, n)
            continue
        if c == '"':
            # raw 字符串：R"delim( ... )delim"，前缀只能是 R / LR / uR / UR / u8R。
            if _ident_prefix(text, i) in RAW_PREFIXES:
                p = text.find("(", i)
                if p != -1:
                    term = ")" + text[i + 1:p] + '"'
                    q = text.find(term, p)
                    q = n if q == -1 else q + len(term)
                    blank(i, q)
                    i = q
                    continue
            # 普通字符串：**不抹**（那正是 R6 要找的东西），但整段跳过，
            # 免得串里的 // 或 ' 被当成注释/字面量起始。
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            i = min(j + 1, n)
            continue
        i += 1
    return "".join(out)


def cjk_wide_literals(root):
    """扫 src/，返回 [(相对路径, 行号, 字面量正文)]：宽串里含 CJK 的那些。"""
    found = []
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
            masked = mask_noncode(text)
            for m in RE_WIDE_LITERAL.finditer(masked):
                body = m.group(1)
                if not RE_CJK.search(body):
                    continue
                found.append((rel, masked.count("\n", 0, m.start()) + 1, body))
    return found


def cjk_narrow_literals(root):
    """扫 src/，返回 [(相对路径, 行号, 字面量正文)]：非宽串字面量里含 CJK 的那些。

    "非宽串" = 编码前缀不以 `L` 结尾（含空前缀、u8）。以 L 结尾的归 R6，本函数
    不重复报 —— 两条规则合起来覆盖全部字符串字面量（raw 除外，已被掩码器抹平）。
    掩码器复用 R6 的那一份：报的是**真代码里的字面量**，注释里的中文不算。
    """
    found = []
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
            masked = mask_noncode(text)
            for m in RE_ANY_STRING.finditer(masked):
                prefix, body = m.group(1), m.group(2)
                if prefix.endswith("L") or prefix.endswith("R"):
                    continue
                if not RE_CJK.search(body):
                    continue
                found.append((rel, masked.count("\n", 0, m.start()) + 1, body))
    return found


def main(argv):
    _force_utf8_stdio()
    verbose = "-v" in argv or "--verbose" in argv

    # --root：直接指定树根，跳过 git 探测。给自测用 —— 它在一棵复制出来的树上跑，
    # 这样才能"改一份、断言守卫变红、再还原"而**不碰真实工作树**。
    if "--root" in argv:
        i = argv.index("--root")
        if i + 1 >= len(argv):
            print("--root 后面缺少目录参数")
            return 2
        root = os.path.abspath(argv[i + 1])
        if not os.path.isdir(root):
            print("--root 指向的目录不存在：%s" % root)
            return 2
    else:
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

    # ---- R6：UI 文案不许硬编码中文（宽串 CJK 白名单）----
    #
    # 这是 R1 的反面：R1 查"代码引用的键在不在语言文件里"，R6 查"该走 Tr() 的
    # 文案有没有绕过它直接被写进代码"。两者都只在**静态**层面兜底。
    cjk = cjk_wide_literals(root)
    hit_allow = set()
    for rel, lineno, body in cjk:
        if (rel, body) in CJK_ALLOW:
            hit_allow.add((rel, body))
            continue
        problems.append('R6 %s:%d 宽串里硬编码了中文 "%s" —— '
                        "界面文案要走 Tr()/Fmt()，否则换语言时不会跟着变"
                        % (rel, lineno, body))
    for rel, body in sorted(CJK_ALLOW):
        if (rel, body) not in hit_allow:
            problems.append('R6 白名单陈旧：%s 里的 "%s" 已经不在代码里了 —— '
                            "删掉这一条，并同步改 CJK_ALLOW_COUNT" % (rel, body))
    if len(CJK_ALLOW) != CJK_ALLOW_COUNT:
        problems.append("R6 白名单项数 = %d，但 CJK_ALLOW_COUNT = %d —— "
                        "加/删条目时必须一起改（项数本身就是断言）"
                        % (len(CJK_ALLOW), CJK_ALLOW_COUNT))

    # ---- R7：内核层的窄串诊断文案不许硬编码中文（非宽串 CJK 白名单）----
    #
    # 内核层不引 I18n.h（分层硬约束），诊断消息只能是窄串 UTF-8 中文 —— R6 看不见。
    narrow = cjk_narrow_literals(root)
    narrow_hit = set()
    for rel, lineno, body in narrow:
        if (rel, body) in NARROW_ALLOW:
            narrow_hit.add((rel, body))
            continue
        problems.append('R7 %s:%d 窄串里硬编码了中文 "%s" —— '
                        "内核层不能引 I18n.h，所以要么走既有的枚举/键映射，"
                        "要么显式登记进 NARROW_ALLOW" % (rel, lineno, body))
    for rel, body in sorted(NARROW_ALLOW):
        if (rel, body) not in narrow_hit:
            problems.append('R7 白名单陈旧：%s 里的 "%s" 已经不在代码里了 —— '
                            "删掉这一条，并同步改 NARROW_ALLOW_COUNT" % (rel, body))
    if len(NARROW_ALLOW) != NARROW_ALLOW_COUNT:
        problems.append("R7 白名单项数 = %d，但 NARROW_ALLOW_COUNT = %d —— "
                        "加/删条目时必须一起改（项数本身就是断言）"
                        % (len(NARROW_ALLOW), NARROW_ALLOW_COUNT))

    # ---- R8：zh-TW 必须是 zh-CN 的忠实繁中转换 ----
    #
    # 与 R1/R6/R7 不同，这条查的是**两份语言文件之间**的关系：zh-TW 是 zh-CN 的
    # 简繁转换产物，所以"是不是忠实转换"能用机器核对。只查这一对 —— ja / ko 是
    # 人工翻译，译文正确性无法自动判定（见 docstring 的覆盖面边界）。
    r8_same_n = 0   # zh-TW 与 zh-CN 逐字相同且含 CJK 的键数
    r8_simp_n = 0   # zh-TW 里命中简体字形的键数
    if ZH_CN not in dicts or ZH_TW not in dicts:
        problems.append("R8 缺少语言文件（%s）—— 无法做繁中核对"
                        % ", ".join(c for c in (ZH_CN, ZH_TW) if c not in dicts))
    else:
        cn = dicts[ZH_CN]
        tw = dicts[ZH_TW]

        # (a) 键集合必须一致：只加进一份 ⇒ 另一份把原始 id 显示给用户。
        only_cn = sorted(set(cn) - set(tw))
        only_tw = sorted(set(tw) - set(cn))
        if only_cn:
            problems.append("R8 %s.json 里的 %d 个键在 %s.json 里不存在：%s —— "
                            "切到繁體中文会直接显示原始 id"
                            % (ZH_CN, len(only_cn), ZH_TW, ", ".join(only_cn[:8])))
        if only_tw:
            problems.append("R8 %s.json 里的 %d 个键在 %s.json 里不存在：%s"
                            % (ZH_TW, len(only_tw), ZH_CN, ", ".join(only_tw[:8])))

        # (b) 与 zh-CN 逐字相同且含 CJK 的键，必须登记在 SAME_CN（简繁同形）。
        same_hit = set()
        for key in sorted(set(cn) & set(tw)):
            cv, tv = cn[key], tw[key]
            if not isinstance(cv, str) or not isinstance(tv, str):
                continue
            if tv != cv or not RE_CJK.search(cv):
                continue
            r8_same_n += 1
            if key in SAME_CN:
                same_hit.add(key)
                continue
            problems.append('R8 %s.%s = "%s" 与 %s.json 逐字相同且含汉字 —— '
                            "是简繁同形就登记进 SAME_CN，否则说明这份没做繁中转换"
                            % (ZH_TW, key, tv, ZH_CN))
        for key in sorted(SAME_CN):
            if key not in same_hit:
                problems.append('R8 SAME_CN 陈旧："%s" 已不再与 %s.json 逐字相同 —— '
                                "删掉这一条，并同步改 SAME_CN_COUNT" % (key, ZH_CN))
        if len(SAME_CN) != SAME_CN_COUNT:
            problems.append("R8 SAME_CN 项数 = %d，但 SAME_CN_COUNT = %d —— "
                            "加/删条目时必须一起改（项数本身就是断言）"
                            % (len(SAME_CN), SAME_CN_COUNT))

        # (c) zh-TW 取值不得含简体独有字形（报错时给出对应繁体）。
        for key in sorted(tw):
            val = tw[key]
            if not isinstance(val, str):
                continue
            bad = sorted({ch for ch in val if ch in SIMP_ONLY})
            if bad:
                r8_simp_n += 1
                detail = "、".join("%s（应为「%s」）" % (ch, SIMP_ONLY[ch]) for ch in bad)
                problems.append('R8 %s.%s = "%s" 含简体字形：%s'
                                % (ZH_TW, key, val, detail))

        # SIMP_ONLY 自校验：每个简体字都必须仍出现在 zh-CN 语料里，否则是死条目
        # （语料变了 ⇒ 表过时，届时重跑派生）。项数本身就是断言。
        cn_corpus = set("".join(v for v in cn.values() if isinstance(v, str)))
        for ch in sorted(SIMP_ONLY):
            if ch not in cn_corpus:
                problems.append('R8 SIMP_ONLY 陈旧："%s" 已经不在 %s.json 的语料里 —— '
                                "删掉这一条（或重跑派生），并同步改 SIMP_ONLY_COUNT"
                                % (ch, ZH_CN))
        if len(SIMP_ONLY) != SIMP_ONLY_COUNT:
            problems.append("R8 SIMP_ONLY 项数 = %d，但 SIMP_ONLY_COUNT = %d —— "
                            "加/删条目时必须一起改（项数本身就是断言）"
                            % (len(SIMP_ONLY), SIMP_ONLY_COUNT))

    # ---- 提示：en.json 里没被任何代码引用的键 ----
    unused = sorted(k for k in base if k not in used)

    if verbose:
        print("仓库：%s" % root)
        print("语言文件：%s" % ", ".join(codes))
        print("下拉列表：%s（kUiLangCount = %s）"
              % (", ".join(ui) if ui else "<解析失败>", ui_count))
        print("基准 %s.json 键数：%d" % (BASE_LANG, len(base)))
        print("src/ 引用的键数：%d" % len(used))
        print("宽串含 CJK：%d 处（白名单 %d 条）" % (len(cjk), len(CJK_ALLOW)))
        print("窄串含 CJK：%d 处（白名单 %d 条）" % (len(narrow), len(NARROW_ALLOW)))
        print("繁中核对：%s ↔ %s 逐字相同且含汉字 %d 键（SAME_CN %d 条）；"
              "zh-TW 命中简体字形 %d 键（字形表 %d 字）"
              % (ZH_TW, ZH_CN, r8_same_n, len(SAME_CN), r8_simp_n, len(SIMP_ONLY)))
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

    print("RESULT: CLEAN —— 代码引用的 %d 个语言键全部存在；"
          "宽串 CJK 白名单 %d 条、窄串 CJK 白名单 %d 条全部命中；"
          "繁中核对：SAME_CN %d 条、简体字形表 %d 字全部对得上"
          % (len(used), len(CJK_ALLOW), len(NARROW_ALLOW),
             len(SAME_CN), len(SIMP_ONLY)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
