#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen-sci-marshal.py — 生成 src/plugin/oop/SciMarshalTable.inc（SCI_* 形状表）。

为什么必须生成，而不是手写
--------------------------
SCI_* 有 826 条消息，其中 110 条带指针。手写这张表必然漏项，而漏项的后果是
**"危险地可用"**：中转窗会把一条带外来指针的消息**直发**给宿主的 Scintilla，
宿主按"同一个数值地址在自己的地址空间里"解引用 ⇒ 访问违例（这就是批次 110
那条规律：WM_USER 之上的消息不封送参数）。所以表从 Scintilla **自己的**机器
可读接口定义生成，并用两条**独立**判据对撞。

三条判据（互相独立）
--------------------
  A  third_party/scintilla/include/Scintilla.iface
     参数类型 ∈ PTR_TYPES ⇒ 带指针；返回类型 == "pointer" ⇒ 带指针。
     **A 是唯一的判定来源。**
  B  third_party/scintilla/include/ScintillaCall.h
     C++ 签名里参数或返回类型出现 '*' ⇒ 带指针。属性访问器在 ScintillaCall.h
     里用的是**去前缀**的名字（iface 的 GetTextLength -> C++ 的 TextLength），
     所以要回退匹配。**B 只做对撞，不改判定。**
  C  third_party/scintilla/include/ScintillaMessages.h
     iface 与它在交集上的 id 必须逐条一致；iface 多出来的名字必须**恰好**等于
     iface 里 `cat Deprecated` 那一段（当前 7 条）。两者都是 Scintilla 生成的，
     但由不同的模板渲染 ⇒ 能抓出本脚本的解析漏洞。

覆盖面边界（诚实声明 —— 别把"0 违规"当成"全覆盖"）
--------------------------------------------------
  * B 只覆盖 810/826 个名字。剩下的 16 条见 UNMATCHED_EXPECTED（7 条
    Deprecated + 9 条 Direct/Document 指针消息）—— 对它们**只有 A 一路判据**。
  * 本脚本只回答"这条 SCI_* 的参数/返回里有没有指针"，**不**回答"宿主的
    Scintilla 是否实现了它"。未实现的消息会返回 0，与"被拒答"**同形** ⇒
    测试里别拿"返回 0"当"实现正确"的证据。
  * 类型词表（PTR_TYPES / VALUE_TYPES）是**穷举**的：见到没见过的类型直接
    报错退出，而不是默认成"值类型"。Scintilla 升级后必须有人来复核一次 ——
    这正是我们要的"失败要响"。

第二张表（批次 113）：入参指针族的**参数布局**
---------------------------------------------
形状表只回答"有没有指针"。要真把入参串**过桥**，还得知道"指针在哪个参数、
长度从哪来"—— 这也是从 iface 生成的（SciBridgeTable.inc），因为 54 条里有
三种反直觉的形态，手写必错：

  * **19 条的串在 wParam**（单参数消息，如 SetText/ReplaceSel）。只看 lp 会漏。
  * **2 条 wp 与 lp 都是串**（SetProperty / SetRepresentation）⇒ 要内联两段。
  * **1 条 wp 是长度但首参名不是 length**（AutoCShow 的 lengthEntered）⇒
    它必须走 NUL 扫描；按长度截断会**静默**砍掉补全列表（不崩，但功能坏）。

长度来源的判据是首参名**精确等于** "length"（不是前缀匹配）。名单冻在
EXPECT_LEN_NAMED 里，改名就报错。

第三张表（批次 114）：出参族的**容量与 NUL 语义**
------------------------------------------------
出参族（kOutStr，30 条）要过桥，得先回答两个问题：**宿主该分配多大缓冲**、
**回传给插件多少字节**。两个答案都在 Scintilla 的**实现**里，而且**只差一个
字节** —— iface 的注释只说「返回长度」，不说容量：

  * lParam == 0 是 Scintilla 出参协议自带的「查长度」调用（不写缓冲，返回需要
    的字节数）⇒ 容量 = 该返回值（记 need）。这一条对**所有**可桥接条目成立，
    包括那些内部走 if (buffer) 保护的分支。
  * StringResult() 写 len+1（**含**结尾 NUL）；BytesResult() 写 len（**不含**）。
    ⇒ 回传字节数 = need+1 或 need。差一个字节就是**越界写 1 字节**（堆破坏，
    而且多半不崩）。
  * GetText / GetCurLine 还要被 wParam 钳制 ⇒ 回传 min(need, wp)+1。
  * **有两条必须拒答**（写入量/NUL 语义随运行期状态变化，静态表分不了）：
    - TargetAsUTF8：NUL 语义取决于运行期文档编码（Unicode 模式走 GetCharRange，
      **不**写 NUL；非 Unicode 模式走 MultiByteFromWideChar 且写 text[len]=0）。
    - GetTag：写入量取决于有没有配 regex（有 ⇒ need+1；无 ⇒ **只写 1 字节**），
      而返回值恒是那个先写死的 length ⇒ "probe 的返回值就是容量"对它不成立。
    ★ GetTag 这个反例是**真 Scintilla 实证**抓出来的 —— 只静态读实现会看到
      memcpy 那一支、把它错判成 kNul。这就是"判据 B 不能省"的实证。

判据 A 是下面冻结的四份名单（人读 Scintilla 实现逐条得出，附证据行）。
判据 B 是**真 Scintilla 实证**（tests/test_sci_out.cpp）：建一个真 Scintilla 窗，
对每条走一遍 SciBridgeOutCall —— 哨兵校验（写多了会踩哨兵）+ NUL 位置校验
（NUL 语义搞反会被抓）合起来让**分类错了就必然拒答**。这是唯一能证明
「差一个字节」没搞反的办法（静态分类做不到）。

第四张表（批次 115）：出入参族的**入参串 + 出参缓冲**
----------------------------------------------------
出入参族（kInOutStr，5 条）的形状是 (string, stringresult)：**入参串在 wParam**
（NUL 结尾）、**出参缓冲在 lParam**。所以它 = 入参族的一半 + 出参族的一半：

  * 出参侧的容量规则与批次 114 的 kOutStr **完全一样**（同属 SciOutKind 桶空间，
    复用 SciOutBytesToCopy / SciOutWroteOnlyExpected）⇒ 本表只多回答一个问题：
    「这条消息要不要带入参串」。在表里 = 要。
  * 入参侧的布局就是批次 113 的 kStrInWp（串在 wParam）⇒ 代理侧**复用**
    SciReadInbound(SciInLayout::kStrInWp, ...) 读串，不另写一套。
  * ★ 这个 wire 约定是**推**出来的，不是手写的：build_inout() 断言本族每条的
    参数恰好是 (string, stringresult) 且顺序如此（INOUT_PTYPES）。形状一变
    （多一个参数 / 顺序反了）就必须重新设计 wire —— 断言就是那次复核的触发器。
  * **两条必须拒答**：
    - EncodedFromUTF8(2449)：① 入参长度来自成员 lengthForEncode，由**伴生消息**
      SetLengthForEncode(2448) 设定 ⇒ 代理侧不知道要送多少字节；② NUL 语义随
      IsUnicodeMode() 变（Unicode 模式 memcpy 不写 NUL，非 Unicode 模式写
      encoded[n]='\0'）。
    - GetRepresentation(2666)：probe **分不了两种 0** —— repr 不存在时直接
      return 0（**0 字节**），repr 存在但内容为空串时走 StringResult（**1 个 NUL**）。
      两者 probe 都返回 0 ⇒ 容量不可知。这条歧义在 tests/test_sci_inout.cpp 里
      用真 Scintilla **直接演示**（同一个 probe 结果、两种写入量）。

第五张表（批次 116）：结构体指针族的**展平方式**
----------------------------------------------
结构族（kStruct，8 条）是"指针指向**结构体**、结构体里**还有指针**"—— 比前四族都
难：既要**展平**结构体字段，又要**内联**结构体里的串，还要处理**条件回写**。
8 条分四类（三可桥 + 一拒答）：

  * **kRangeOut**（GetTextRange 2162 / GetTextRangeFull 2039）：只读结构 + 出缓冲。
    容量 **算得出**：`len = (cpMax == -1 ? docLen : cpMax) - cpMin`，写 `len+1`。
    ★ 与批次 114 的 kOutStr **相反**（那里容量不在消息里、必须探长度）。
    ⚠ 越界范围**故意拒答**：CellBuffer.cxx:358 越界时一个字节都不写，而
    Editor::GetTextRange 仍写 `buffer[len]='\0'` 并返回 len ⇒ 原生会留下"垃圾 +
    末尾 NUL"，桥接不把垃圾回灌给插件。
  * **kStyledOut**（GetStyledText 2015 / GetStyledTextFull 2778）：同上，容量
    `2*len + 2`（每字符 (char, style) 两字节 + 末尾两个 NUL）。
  * **kFindInOut**（FindText 2150 / FindTextFull 2196）：**入串 + 条件回写**。
    读 chrg + needle，**只在 `pos != -1` 时**写 chrgText（Editor.cxx:4318/4349）
    ⇒ 回写要用 **canary 判定"到底写没写"**，**不复刻 `pos != -1` 这条规则**
    （复刻 = 把产品逻辑抄一份到桥上，两份会漂）。
  * **kRefuseHandle**（FormatRange 2151 / FormatRangeFull 2777）：**永久拒答**。
    `AutoSurface(pfr->hdc, …)` 用的是**调用进程的 HDC**（GDI 句柄表进程私有），
    且 view.FormatRange 是**绘制**操作；公开结构 Sci_FormatRange 还带 HWND。

★ 本族是本项目第一个**真正的 in-out 族**（同一条消息里既有入参侧又有出参侧）。
★ `full` 标志（用 64 位 Sci_Position 还是 32 位 Sci_PositionCR）**由参数类型推出**：
  生成器断言「类型以 full 结尾」⟺「名字以 Full 结尾」—— 手写这个 bool 必错。

用法
----
    python scripts/gen-sci-marshal.py             # 生成/覆盖五个 .inc
    python scripts/gen-sci-marshal.py --check     # 只校验（0 干净 / 1 过期）
    python scripts/gen-sci-marshal.py --dump-types
    python scripts/gen-sci-marshal.py --dump-inbound
    python scripts/gen-sci-marshal.py --dump-outbound
    python scripts/gen-sci-marshal.py --dump-inout
    python scripts/gen-sci-marshal.py --dump-struct
    python scripts/gen-sci-marshal.py --show NAME [NAME ...]
"""

import os
import re
import sys

IFACE = "third_party/scintilla/include/Scintilla.iface"
CALL_H = "third_party/scintilla/include/ScintillaCall.h"
MSGS_H = "third_party/scintilla/include/ScintillaMessages.h"
OUT = "src/plugin/oop/SciMarshalTable.inc"
OUT_BRIDGE = "src/plugin/oop/SciBridgeTable.inc"
OUT_OUTBOUND = "src/plugin/oop/SciOutTable.inc"
OUT_INOUT = "src/plugin/oop/SciInOutTable.inc"
OUT_STRUCT = "src/plugin/oop/SciStructTable.inc"

# 生成的 .inc 一律用 CRLF（与仓库其余文件一致）。这里用 chr() 而不是字面转义，
# 免得补丁/编辑工具把转义序列在传输途中解成真实换行，写进源码里变成语法错误。
NL = chr(13) + chr(10)

# ---- 期望值（Scintilla 5.6.6）。对不上就是升级了 ⇒ 停下来复核，别自动放过 ----
EXPECT_FEATURES = 826        # iface 里带编号的 fun/get/set 条数
EXPECT_HEADER = 819          # ScintillaMessages.h 的枚举条数
EXPECT_DEPRECATED = 7        # iface 里 cat Deprecated 段条数
EXPECT_MATCHED_MIN = 800     # B 至少要覆盖这么多名字，否则说明 B 的解析退化了
EXPECT_PTR = 110             # 带指针条数（A 判据）

# ---- 入参指针族布局（批次 113）。见文件头"第二张表"-------------------------
# 期望条数 = kInStr + kInBytes。逐桶条数冻结：变了就是 Scintilla 升级或本脚本
# 的解析退化，两种都必须有人来复核。
EXPECT_IN_TOTAL = 54
EXPECT_IN_BUCKETS = {
    "kStrInLp": 33,      # lp 是串，wp 是值（NUL 结尾）
    "kStrInLpLen": 11,   # lp 是 wp 字节的串（wp **就是**长度）
    "kStrInWp": 7,       # wp 是串，lp 是值（**首参非空**的那些）
    "kStrBoth": 2,       # wp、lp 都是串
    "kBytesInLpLen": 1,  # lp 是 wp 字节的裸缓冲（cells）
}
# ★★ kStrInWp 的**完整名单**（7 条）。判据是"iface 的**槽 0** 是 string/cells"。
#   批次 113 那版把空参数槽压掉，于是 `(, string text)` 这种（槽 0 空、槽 1 是串）
#   被算成"串在 wp" ⇒ 12 条错判（SetText / ReplaceSel / SetWordChars /
#   SetWhitespaceChars / SetPunctuationChars / AutoCStops / AutoCSelect /
#   AutoCSetFillUps / SetFontLocale / SetDefaultFoldDisplayText /
#   SetSelectionSerialized / SetCopySeparator），跨进程调用**恒返回 0、静默不生效**。
#   现在把名单冻死：少一条 / 多一条都会让生成器报错停下来复核。
EXPECT_WP_NAMED = frozenset({
    "ClearRepresentation",              # 2667 (string encodedCharacter,)
    "SetRepresentationAppearance",      # 2766 (string encodedCharacter, RepresentationAppearance)
    "GetRepresentationAppearance",      # 2767 (string encodedCharacter,)
    "SetRepresentationColour",          # 2768 (string encodedCharacter, colouralpha)
    "GetRepresentationColour",          # 2769 (string encodedCharacter,)
    "GetPropertyInt",                   # 4010 (string key, int defaultValue)
    "PropertyType",                     # 4015 (string name,)
})
# 首参名**精确等于** "length" 的那些（wp 是缓冲字节数）。
# ⚠ AutoCShow 的首参叫 lengthEntered —— 已输入字符数，**不是**缓冲长度。
#   用 startswith("length") 判会把它错分进长度桶 ⇒ 补全列表被截断（静默功能缺陷，
#   不崩）。所以这里用 == 而不是前缀匹配，并把名单冻死。
EXPECT_LEN_NAMED = frozenset({
    "AddText", "AddStyledText", "SetStylingEx",
    "ReplaceTarget", "ReplaceTargetRE", "SearchInTarget", "AppendText",
    "CopyText", "ChangeInsertion", "ReplaceRectangular",
    "ReplaceTargetMinimal", "ChangeLastUndoActionText",
})
# wp 与 lp 都是串的那两条（双指针特例，必须单独内联两段）。
EXPECT_BOTH_PTR = frozenset({"SetRepresentation", "SetProperty"})

# ---- 出参族（kOutStr 30 条）的容量 / NUL 语义（批次 114）----------------------
# 回传字节数规则（need = 同一条消息 lParam==0 的返回值）：
#   kNul           -> need + 1          （实现写 len+1，含结尾 NUL）
#   kNoNul         -> need              （实现写 len，不含 NUL）
#   kClampWpNul    -> min(need, wp) + 1 （写之前被 wParam 钳制）
#   kRefuseRuntime -> 拒答              （NUL 语义随运行期文档编码变化，静态表分不了）
#
# 每条的证据（Scintilla 5.6.6 源码），改判据前先回去读一遍：
#   kNul        : Editor::StringResult() 写 len+1（Editor.cxx:6264）；
#                 GetSelText 的 ptr[iChar]='\0'；
#                 ScintillaBase::AutoCompleteGetCurrentText 的 memcpy(..., len+1)
#   kNoNul      : Editor::BytesResult() 写 len（Editor.cxx:6276，注释明写
#                 "No NUL termination"）；GetLine 的注释 "not NUL terminated"；
#                 CharClassify::GetCharsOfClass 逐字节写 count 个
#   kClampWpNul : Editor.cxx 的 GetText / GetCurLine：len=min(...,wParam) 后 ptr[len]=0
#   kRefuse     : 写入量/NUL 语义**随运行期状态变化**，静态表分不了。两条：
#                 · win32/ScintillaWin.cxx 的 TargetAsUTF8 —— Unicode 模式走
#                   GetCharRange（不写 NUL），非 Unicode 模式走
#                   MultiByteFromWideChar 且写 text[len]=0，取决于 IsUnicodeMode()。
#                 · Editor::GetTag（Editor.cxx:5910）—— `length = 2` 是**先写死**
#                   的，真正的写入量取决于有没有配 regex：
#                       text = pdoc->SubstituteByPosition(name, &length);  // 无 regex → nullptr
#                       if (text) memcpy(tagValue, text, length + 1);      // 有 regex：need+1
#                       else      *tagValue = '\0';                        // 无 regex：**只写 1 字节**
#                       return length;                                     // 恒返回 length(2)
#                   ⇒ 探长度那次的返回值（2）与真实写入量（1）无关，
#                     "probe 的返回值就是容量"这条协议对它**不成立**。
#                     这个反例是**真 Scintilla 实证**（tests/test_sci_out.cpp）抓出来的：
#                     静态读实现时只看到 memcpy 那一支，会把它错判成 kNul。
EXPECT_OUT_TOTAL = 30
EXPECT_OUT_BUCKETS = {"kNul": 14, "kNoNul": 12, "kClampWpNul": 2, "kRefuseRuntime": 2}
OUT_NUL_NAMED = frozenset({
    "GetSelText", "StyleGetInvisibleRepresentation", "StyleGetFont",
    "AutoCGetCurrentText", "GetDefaultFoldDisplayText",
    "GetFontLocale", "GetCopySeparator", "GetLexerLanguage",
    "PropertyNames", "DescribeKeyWordSets", "GetSubStyleBases",
    "NameOfStyle", "TagsOfStyle", "DescriptionOfStyle",
})
OUT_NONUL_NAMED = frozenset({
    "GetLine", "MarginGetText", "MarginGetStyles", "AnnotationGetText",
    "AnnotationGetStyles", "GetWordChars", "GetWhitespaceChars",
    "GetPunctuationChars", "GetTargetText", "EOLAnnotationGetText",
    "GetSelectionSerialized", "GetUndoActionText",
})
OUT_CLAMP_NAMED = frozenset({"GetCurLine", "GetText"})
OUT_REFUSE_NAMED = frozenset({"TargetAsUTF8", "GetTag"})

# ---- 出入参族（kInOutStr 5 条）的容量 / NUL 语义（批次 115）-------------------
# 形状是 (string, stringresult)：**入参串在 wParam、出参缓冲在 lParam**。
# 与批次 114 的差别只在"多了一段要送进去的串"；出参侧的容量规则**完全一样**，
# 所以复用同一个 SciOutKind 桶空间，只多一张表说明"哪些消息要带入参串"。
#
#   kNul           -> need + 1   （实现走 Editor::StringResult，写 len+1）
#   kRefuseRuntime -> 拒答
#
# 证据（Scintilla 5.6.6）：
#   kNul（3 条）：
#     · ScintillaBase.cxx:1079 / 1082  GetProperty(4008) / GetPropertyExpanded(4009)
#          return StringResult(lParam, DocumentLexState()->PropGet(wParam));
#       PropGet 可能返回 nullptr（没有词法器时 LexState::instance 为空，
#       ScintillaBase.cxx:717）—— StringResult 对 nullptr 的处理是 len=0 且写
#       *ptr=0（**1 字节**），与"空串"同形 ⇒ 仍然落在 need+1 里。
#       ⚠ 5.6.6 的 GetPropertyExpanded 用的就是 PropGet（与 GetProperty 同体）。
#     · ScintillaBase.cxx:1110  DescribeProperty(4016)
#          return StringResult(lParam, DocumentLexState()->DescribeProperty(wParam));
#   kRefuseRuntime（2 条）：
#     · win32/ScintillaWin.cxx:1262 EncodedFromUTF8(2449) —— 两个独立理由：
#       ① 入参长度取自成员 lengthForEncode，由**伴生消息** SetLengthForEncode(2448)
#          设定（Editor.cxx:8520）⇒ 代理侧看不到，不知道要送多少字节；
#       ② NUL 语义随 IsUnicodeMode() 变：Unicode 模式 memcpy(encoded, utf8, n)
#          **不写 NUL**；非 Unicode 模式 MultiByteFromWideChar 后 encoded[n]='\0'。
#     · Editor.cxx:8624 GetRepresentation(2666) —— 探长度**分不了两种 0**：
#          repr 不存在时直接 return 0，**一个字节都不写**；
#          repr 存在但内容为空串时走 StringResult，**写 1 个 NUL**。
#       两者 probe 都返回 0 ⇒ 容量不可知 ⇒ 拒答。
#       （这条歧义在 tests/test_sci_inout.cpp 里用真 Scintilla 直接演示。）
EXPECT_INOUT_TOTAL = 5
EXPECT_INOUT_BUCKETS = {"kNul": 3, "kRefuseRuntime": 2}
INOUT_NUL_NAMED = frozenset({
    "GetProperty", "GetPropertyExpanded", "DescribeProperty",
})
INOUT_REFUSE_NAMED = frozenset({"EncodedFromUTF8", "GetRepresentation"})
# 本族的 wire 约定（入参串 → wParam、出参缓冲 → lParam）是从这个参数形状**推**
# 出来的，不是手写的：build_inout() 会断言每一条都恰好是这个形状与顺序。
INOUT_PTYPES = ("string", "stringresult")

# ---- 结构体指针族（kStruct 8 条）的展平方式（批次 116）-----------------------
# 形状是「指针指向**结构体**，结构体里**还有指针**」⇒ 要把结构体**展平**进 wire、
# 并把里面的串**内联**。8 条分三类（+ 一类拒答）：
#
#   kRangeOut     -> 出缓冲，容量 = len + 1       GetTextRange(2162) / GetTextRangeFull(2039)
#   kStyledOut    -> 出缓冲，容量 = 2*len + 2     GetStyledText(2015) / GetStyledTextFull(2778)
#   kFindInOut    -> 入串 + **条件**回写 chrgText  FindText(2150) / FindTextFull(2196)
#   kRefuseHandle -> 拒答                         FormatRange(2151) / FormatRangeFull(2777)
#
# 证据（Scintilla 5.6.6）：
#   kRangeOut：Editor.cxx:6047 GetTextRange(char*, cpMin, cpMax)
#       cpEnd = (cpMax == -1) ? pdoc->Length() : cpMax；len = cpEnd - cpMin；
#       pdoc->GetCharRange(buffer, cpMin, len)；buffer[len] = '\0'；return len。
#       ⇒ **写 len+1 字节**（含结尾 NUL）。★ 容量由结构体字段**算得出**，不需要
#         探长度（与批次 114 的 kOutStr 相反 —— 那里容量不在消息里）。
#       ⚠ CellBuffer.cxx:358 在 (position + lengthRetrieve) > Length() 时**一个字节
#         都不写**就返回，但 Editor::GetTextRange 仍然写 buffer[len]='\0' 并返回 len
#         ⇒ 越界范围的结果是"len+1 个字节里只有最后一个是 NUL、前面全是垃圾"。
#         桥接**故意收窄**：cpEnd > docLen 一律拒答，绝不把垃圾回灌给插件。
#   kStyledOut：Editor.cxx:6036 GetStyledText(char*, cpMin, cpMax)
#       逐字符写 (char, style) 两字节，最后 buffer[2n] = buffer[2n+1] = '\0'
#       ⇒ **写 2*len+2 字节**。CharAt / StyleAtNoExcept 自带钳制 ⇒ 不会写出垃圾。
#   kFindInOut：Editor.cxx:4302 / 4333 FindText / FindTextFull
#       读 chrg.cpMin/cpMax + lpstrText（NUL 结尾的 needle），
#       **只在 pos != -1 时**写 chrgText.cpMin/cpMax（4318 / 4349）⇒ 回写是
#       **有条件的**；桥接用 canary 判定"到底写没写"，**不复刻 pos != -1 这条规则**。
#   kRefuseHandle：Editor.cxx:1968 FormatRange
#       AutoSurface(pfr->hdc, …) / AutoSurface(pfr->hdcTarget, …) 用的是**调用进程
#       的 HDC**（GDI 句柄表进程私有），且 view.FormatRange 是**绘制**操作；公开
#       结构 Sci_FormatRange 还带 HWND ⇒ 语义上**永久不可桥接**。
#
# ★ 本族是本项目第一个**真正的 in-out 族**（同一条消息里既有入参侧又有出参侧）。
EXPECT_STRUCT_TOTAL = 8
EXPECT_STRUCT_BUCKETS = {"kRangeOut": 2, "kStyledOut": 2,
                         "kFindInOut": 2, "kRefuseHandle": 2}
STRUCT_RANGE_NAMED = frozenset({"GetTextRange", "GetTextRangeFull"})
STRUCT_STYLED_NAMED = frozenset({"GetStyledText", "GetStyledTextFull"})
STRUCT_FIND_NAMED = frozenset({"FindText", "FindTextFull"})
STRUCT_REFUSE_NAMED = frozenset({"FormatRange", "FormatRangeFull"})
# 本族的 wire 约定（展平 chrg + 内联 needle）是从参数形状**推**出来的，不是手写的：
# build_struct() 会断言每一类的参数形状恰好如此。
# kRangeOut / kStyledOut 的形状：`(, textrange tr)` / `(, textrangefull tr)`。
# ⚠ 槽 0 是**空的**（wParam 未用），串指针在结构体里 —— 空槽必须保留在元组里，
#   否则就分不出"结构体在槽 0"还是"在槽 1"。
STRUCT_RANGE_PTYPES = frozenset({("", "textrange"), ("", "textrangefull")})
STRUCT_FIND_PTYPES = frozenset({("FindOption", "findtext"),
                                ("FindOption", "findtextfull")})
STRUCT_REFUSE_PTYPES = frozenset({("bool", "formatrange"),
                                  ("bool", "formatrangefull")})

# B 覆盖不到的名字：7 条 Deprecated + 9 条 Direct/Document 指针消息。
UNMATCHED_EXPECTED = frozenset({
    "GetDirectFunction", "GetDirectStatusFunction", "GetDirectPointer",
    "GetDocPointer", "CreateDocument", "GetCharacterPointer",
    "GetRangePointer", "CreateLoader", "PrivateLexerCall",
    "SetStyleBits", "GetStyleBits", "GetStyleBitsNeeded",
    "SetKeysUnicode", "GetKeysUnicode", "GetTwoPhaseDraw", "SetTwoPhaseDraw",
})

# ---- 类型词表（穷举；见文件头"覆盖面边界"第 3 条）----------------------------
PTR_STR = frozenset({"string", "stringresult", "pointer"})
PTR_STRUCT = frozenset({"textrange", "textrangefull", "findtext", "findtextfull",
                        "formatrange", "formatrangefull"})
PTR_BYTES = frozenset({"cells"})
PTR_TYPES = PTR_STR | PTR_STRUCT | PTR_BYTES

VALUE_TYPES = frozenset({
    "Accessibility", "Alpha", "AnnotationVisible", "AutoCompleteOption",
    "AutomaticFold", "Bidirectional", "CaretPolicy", "CaretSticky",
    "CaretStyle", "CaseInsensitiveBehaviour", "CaseVisible", "ChangeHistoryOption",
    "CharacterSet", "CursorShape", "DocumentOption", "EOLAnnotationVisible",
    "EdgeVisualStyle", "Element", "EndOfLine", "FindOption",
    "FoldAction", "FoldDisplayTextStyle", "FoldFlag", "FoldLevel",
    "FontQuality", "FontStretch", "FontWeight", "IMEInteraction",
    "IdleStyling", "IndentView", "IndicFlag", "IndicatorStyle",
    "Layer", "LineCache", "LineCharacterIndexType", "LineEndType",
    "MarginOption", "MarginType", "MarkerSymbol", "ModificationFlags",
    "MultiAutoComplete", "MultiPaste", "Ordering", "PhasesDraw",
    "PopUp", "PrintOption", "RepresentationAppearance", "ScaleTechnique",
    "SelectionMode", "Status", "Supports", "TabDrawMode",
    "Technology", "TypeProperty", "UndoFlags", "UndoSelectionHistoryOption",
    "VirtualSpace", "VisiblePolicy", "WhiteSpace", "Wrap",
    "WrapIndentMode", "WrapVisualFlag", "WrapVisualLocation", "bool",
    "colour", "colouralpha", "int", "keymod",
    "line", "position", "void",
})

FEAT_RE = re.compile(r"^(fun|get|set)\s+(\S+)\s+([A-Za-z_]\w*)(?:=(\d+))?\((.*)\)\s*$")
CAT_RE = re.compile(r"^cat\s+(\w+)")
ENUM_RE = re.compile(r"^\s*(\w+)\s*=\s*(\d+)\s*,", re.M)
# ScintillaCall.h 的一行签名（含 const/noexcept/override 等后缀）
SIG_RE = re.compile(r"^\s*([\w:<>]+)\s+(\w+)\s*\((.*)\)\s*"
                    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?;?\s*$")

SHAPE_ORDER = ("kPtrRet", "kStruct", "kRawPtr", "kInBytes",
               "kInOutStr", "kOutStr", "kInStr", "kValue")


def _force_utf8_stdio():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        return fh.read()


def parse_iface(root):
    """返回 (features, deprecated_names)。

    features: name -> {"id", "kind", "ret", "ptypes"}
    """
    features = {}
    deprecated = set()
    cur_cat = None
    dup = []
    for raw in read_text(os.path.join(root, IFACE)).split("\n"):
        line = raw.rstrip("\r").strip()
        m = CAT_RE.match(line)
        if m:
            cur_cat = m.group(1)
            continue
        m = FEAT_RE.match(line)
        if not m:
            continue
        kind, ret, name, num, params = m.groups()
        if num is None:
            continue                      # 无编号的特征不是消息，跳过
        # ★★ 空参数占位符**必须保留** —— 参数**按位置**映射 wParam / lParam，
        #    而 iface 的文件头明写 "param may be empty (null value)"：空占位符
        #    仍然占掉它那一格。例：
        #      fun void SetText=2181(, string text)          ⇒ 串在 **lParam**
        #      fun void ClearRepresentation=2667(string c,)  ⇒ 串在 **wParam**
        #    批次 113 起这里写的是 `if p.strip()`（丢掉空槽）⇒ 12 条被错判成
        #    kStrInWp ⇒ 跨进程 SETTEXT / REPLACESEL / SETWORDCHARS … **恒返回 0、
        #    静默不生效**（不崩、不报错）。而 e2e 夹具是照着同一张错表写的 ⇒
        #    两边自洽、全绿。批次 116 修，并加了 EXPECT_WP_NAMED 守卫。
        #    只去掉**末尾**的空槽（`(,)` 这种"无参数"写法、以及 `(string,)` 的尾槽），
        #    前导/中间的空占位符一律保留。
        plist = [p.split() for p in params.split(",")]
        while plist and not plist[-1]:
            plist.pop()
        ptypes = [p[0] if p else "" for p in plist]
        pnames = [p[1] if len(p) > 1 else "" for p in plist]
        if name in features:
            dup.append(name)
        features[name] = {"id": int(num), "kind": kind, "ret": ret,
                          "ptypes": ptypes, "pnames": pnames}
        if cur_cat == "Deprecated":
            deprecated.add(name)
    return features, deprecated, dup


def parse_call_h(root):
    """返回 name -> 是否有指针（任一重载带 '*' 即算）。"""
    ptr = {}
    for raw in read_text(os.path.join(root, CALL_H)).split("\n"):
        m = SIG_RE.match(raw.rstrip("\r"))
        if not m:
            continue
        ret, name, params = m.groups()
        if name in ("if", "for", "while", "switch", "return", "sizeof"):
            continue
        has = ("*" in params) or ("*" in ret)
        ptr[name] = ptr.get(name, False) or has
    return ptr


def parse_msgs_h(root):
    """ScintillaMessages.h 的 enum class Message 条目。"""
    txt = read_text(os.path.join(root, MSGS_H))
    if "enum class Message {" not in txt:
        raise SystemExit("ERROR: ScintillaMessages.h 里找不到 enum class Message")
    body = txt.split("enum class Message {", 1)[1].split("};", 1)[0]
    out = {}
    for m in ENUM_RE.finditer(body):
        out[m.group(1)] = int(m.group(2))
    return out

def lookup_call(ptr_map, name):
    """ScintillaCall.h 的查表：先按原名，再按去掉 Get/Set 前缀的名字。"""
    if name in ptr_map:
        return ptr_map[name]
    for pre in ("Get", "Set"):
        if name.startswith(pre) and name[len(pre):] in ptr_map:
            return ptr_map[name[len(pre):]]
    return None


def shape_of(ret, ptypes):
    if ret == "pointer":
        return "kPtrRet"
    if any(t in PTR_STRUCT for t in ptypes):
        return "kStruct"
    if "pointer" in ptypes:
        return "kRawPtr"
    if any(t in PTR_BYTES for t in ptypes):
        return "kInBytes"
    has_in = "string" in ptypes
    has_out = "stringresult" in ptypes
    if has_in and has_out:
        return "kInOutStr"
    if has_out:
        return "kOutStr"
    if has_in:
        return "kInStr"
    return "kValue"


IN_LAYOUT_ORDER = ("kStrInLp", "kStrInLpLen", "kStrInWp", "kStrBoth",
                   "kBytesInLpLen")
IN_STR_TYPES = frozenset({"string", "cells"})


def in_layout_of(ptypes, pnames):
    """入参族（kInStr / kInBytes）的参数布局。见文件头"第二张表"。

    只对 kInStr / kInBytes 调用；其余形状返回 kNone。
    SCI_* 至多两个参数，**按位置**映射 wParam / lParam（build() 会断言这一点）。

    ⚠ ptypes/pnames 是**按位置**的参数槽（空串 = 该槽为空），不是"非空参数压缩后
      的列表"。判据必须落在**槽号**上：`(, string text)` 的串在槽 1（lParam），
      `(string,)` 的串在槽 0（wParam）。把空槽压掉会让两者混为一谈 ——
      这正是批次 113 那 12 条错判的根因（见 parse_iface 的注释）。
    """
    wp_t = ptypes[0] if len(ptypes) > 0 else ""
    lp_t = ptypes[1] if len(ptypes) > 1 else ""
    wp_is_ptr = wp_t in IN_STR_TYPES
    lp_is_ptr = lp_t in IN_STR_TYPES
    if wp_is_ptr and lp_is_ptr:
        return "kStrBoth"
    if wp_is_ptr:
        return "kStrInWp"
    if not lp_is_ptr:
        return "kNone"            # 指针在第三个参数上：SCI_* 不可能，见断言
    # lp 是串：看首参是不是"长度"。⚠ 必须精确等于 "length" —— AutoCShow 的
    # lengthEntered 只是"已输入字符数"，当长度用会截断补全列表。
    if pnames and pnames[0] == "length" and wp_t in ("position", "int"):
        return "kBytesInLpLen" if lp_t == "cells" else "kStrInLpLen"
    return "kStrInLp"


def build_inbound(features, shapes):
    """返回入参族的 (id, name, layout) 列表 + 各桶条数 + 两组名单。"""
    entries = []
    for name, f in features.items():
        if shapes[name] not in ("kInStr", "kInBytes"):
            continue
        lay = in_layout_of(f["ptypes"], f["pnames"])
        if lay == "kNone":
            raise SystemExit(
                "ERROR: %s 被形状表判为 %s，但布局判据算不出布局"
                "（参数 %s）—— 停下来复核 in_layout_of。"
                % (name, shapes[name], list(zip(f["ptypes"], f["pnames"]))))
        entries.append((f["id"], name, lay))
    entries.sort(key=lambda e: e[0])
    counts = {k: 0 for k in IN_LAYOUT_ORDER}
    for _, _, lay in entries:
        counts[lay] += 1
    len_named = frozenset(n for n, f in features.items()
                          if shapes[n] in ("kInStr", "kInBytes")
                          and f["pnames"] and f["pnames"][0] == "length")
    both = frozenset(n for n, f in features.items()
                     if shapes[n] in ("kInStr", "kInBytes")
                     and in_layout_of(f["ptypes"], f["pnames"]) == "kStrBoth")
    # ★★ 守卫：kStrInWp 的名单必须**恰好**是 EXPECT_WP_NAMED，且每条的**槽 0**
    #    都必须非空（空槽 ⇒ 那格的"串"其实是槽 1 的，判据退化了）。
    #    这条断言的存在理由：批次 113 的空槽压缩让 12 条错判，而**当时的守卫
    #    （桶计数）照样通过** —— 因为它数的是"有多少条"，不是"是哪几条"。
    wp_named = frozenset(n for n, f in features.items()
                         if shapes[n] in ("kInStr", "kInBytes")
                         and in_layout_of(f["ptypes"], f["pnames"]) == "kStrInWp")
    if wp_named != EXPECT_WP_NAMED:
        raise SystemExit(
            "ERROR: kStrInWp 名单与冻结名单不符。\n"
            "  多出来：%s\n  少掉了：%s\n"
            "  ⇒ 槽 0 为空、串在槽 1（lParam）的消息**不该**落在这一桶。\n"
            "     先复核 parse_iface 有没有把空参数槽压掉，再复核 EXPECT_WP_NAMED。"
            % (sorted(wp_named - EXPECT_WP_NAMED),
               sorted(EXPECT_WP_NAMED - wp_named)))
    for n in sorted(wp_named):
        if not features[n]["ptypes"] or features[n]["ptypes"][0] not in IN_STR_TYPES:
            raise SystemExit(
                "ERROR: %s 被判成 kStrInWp，但它的**槽 0** 不是 string/cells"
                "（参数 %s）—— 空槽被压缩了？"
                % (n, list(zip(features[n]["ptypes"], features[n]["pnames"]))))
    return entries, counts, len_named, both


OUT_ORDER = ("kNul", "kNoNul", "kClampWpNul", "kRefuseRuntime")


def build_outbound(features, shapes):
    """返回出参族的 (id, name, kind) 列表 + 各桶条数。

    唯一判定来源是上面四份**冻结名单**。这里只做两个方向的完整性断言：
    每条恰好落在一个桶里（抓重复/漏判），四个桶的并集恰好等于 kOutStr 全体
    （抓多判）—— 形状表与名单必须互相锁死，改一边就会响。
    """
    kinds = {}
    for name in OUT_NUL_NAMED:
        kinds[name] = "kNul"
    for name in OUT_NONUL_NAMED:
        assert name not in kinds, name
        kinds[name] = "kNoNul"
    for name in OUT_CLAMP_NAMED:
        assert name not in kinds, name
        kinds[name] = "kClampWpNul"
    for name in OUT_REFUSE_NAMED:
        assert name not in kinds, name
        kinds[name] = "kRefuseRuntime"

    out_names = {n for n, sh in shapes.items() if sh == "kOutStr"}
    missing = out_names - set(kinds)
    extra = set(kinds) - out_names
    if missing or extra:
        raise SystemExit(
            "ERROR: 出参族名单与形状表不一致：漏判 %s，多判 %s。"
            " 形状表说是 kOutStr 就必须在四份名单里占一个；反之亦然。"
            " 停下来复核（多半是 Scintilla 升级加了新的 stringresult 消息，"
            " 或者本脚本的 shape_of 退化了）。"
            % (sorted(missing), sorted(extra)))

    entries = sorted((features[n]["id"], n, kinds[n]) for n in out_names)
    counts = {k: 0 for k in OUT_ORDER}
    for _, _, k in entries:
        counts[k] += 1
    return entries, counts


INOUT_ORDER = ("kNul", "kRefuseRuntime")

STRUCT_ORDER = ("kRangeOut", "kStyledOut", "kFindInOut", "kRefuseHandle")


def build_inout(features, shapes):
    """返回出入参族的 (id, name, kind) 列表 + 各桶条数。

    与 build_outbound 同构：唯一判定来源是两份**冻结名单**，双向完整性断言把
    "漏判"和"多判"都变成硬错误。

    额外一条**前提断言**：本族每条的参数必须恰好是 (string, stringresult) 且顺序
    如此。"入参串 → wParam、出参缓冲 → lParam"这个 wire 约定就是从这里**推**出来
    的，不是手写的 —— 形状一变（比如多一个参数、或顺序反了）必须重新设计 wire，
    不能沿用。这条断言就是那次复核的触发器。
    """
    kinds = {}
    for name in INOUT_NUL_NAMED:
        kinds[name] = "kNul"
    for name in INOUT_REFUSE_NAMED:
        assert name not in kinds, name
        kinds[name] = "kRefuseRuntime"

    inout_names = {n for n, sh in shapes.items() if sh == "kInOutStr"}
    missing = inout_names - set(kinds)
    extra = set(kinds) - inout_names
    if missing or extra:
        raise SystemExit(
            "ERROR: 出入参族名单与形状表不一致：漏判 %s，多判 %s。"
            " 形状表说是 kInOutStr 就必须在两份名单里占一个；反之亦然。"
            " 停下来复核（多半是 Scintilla 升级加了新的 (string, stringresult) 消息，"
            " 或者本脚本的 shape_of 退化了）。" % (sorted(missing), sorted(extra)))

    bad = []
    for n in sorted(inout_names):
        pt = tuple(features[n]["ptypes"])
        if pt != INOUT_PTYPES:
            bad.append((n, list(pt)))
    if bad:
        raise SystemExit(
            "ERROR: 出入参族的参数形状不再是 %s：%s。"
            " 本族的 wire 约定（入参串 → wParam、出参缓冲 → lParam）是从这个形状"
            " 推出来的；形状一变必须重新设计 wire，不能沿用。"
            % (list(INOUT_PTYPES), bad))

    entries = sorted((features[n]["id"], n, kinds[n]) for n in inout_names)
    counts = {k: 0 for k in INOUT_ORDER}
    for _, _, k in entries:
        counts[k] += 1
    return entries, counts


def build_struct(features, shapes):
    """返回结构族的 (id, name, kind, full) 列表 + 各桶条数。

    与 build_outbound / build_inout 同构：唯一判定来源是四份**冻结名单**，双向
    完整性断言把"漏判"与"多判"都变成硬错误。另加两条**前提断言**（形状一变就
    必须重新设计 wire，不能沿用）：
      * 每一类的参数形状恰好是 STRUCT_*_PTYPES 里那一种；
      * **isFull 的判据**：`参数类型以 full 结尾` ⟺ `名字以 Full 结尾`。两边都由
        权威源给出，所以这条断言能抓出"名字与类型对不上" —— 手写这个 bool 必错。
    """
    kinds = {}
    for name in STRUCT_RANGE_NAMED:
        kinds[name] = "kRangeOut"
    for name in STRUCT_STYLED_NAMED:
        assert name not in kinds, name
        kinds[name] = "kStyledOut"
    for name in STRUCT_FIND_NAMED:
        assert name not in kinds, name
        kinds[name] = "kFindInOut"
    for name in STRUCT_REFUSE_NAMED:
        assert name not in kinds, name
        kinds[name] = "kRefuseHandle"

    struct_names = {n for n, sh in shapes.items() if sh == "kStruct"}
    missing = struct_names - set(kinds)
    extra = set(kinds) - struct_names
    if missing or extra:
        raise SystemExit(
            "ERROR: 结构族名单与形状表不一致：漏判 %s，多判 %s。"
            " 形状表说是 kStruct 就必须在四份名单里占一个；反之亦然。"
            " 停下来复核（多半是 Scintilla 升级加了新的结构体指针消息，"
            " 或者本脚本的 shape_of 退化了）。" % (sorted(missing), sorted(extra)))

    expect = {"kRangeOut": STRUCT_RANGE_PTYPES,
              "kStyledOut": STRUCT_RANGE_PTYPES,
              "kFindInOut": STRUCT_FIND_PTYPES,
              "kRefuseHandle": STRUCT_REFUSE_PTYPES}
    bad = []
    for n in sorted(struct_names):
        pt = tuple(features[n]["ptypes"])
        if pt not in expect[kinds[n]]:
            bad.append((n, list(pt), kinds[n]))
    if bad:
        raise SystemExit(
            "ERROR: 结构族某一类的参数形状变了：%s。"
            " 本族的 wire 约定（展平 chrg + 内联 needle）是从这些形状推出来的；"
            " 形状一变必须重新设计 wire，不能沿用。" % bad)

    full_bad = []
    for n in sorted(struct_names):
        pt = features[n]["ptypes"]
        t_full = any(t.endswith("full") for t in pt)
        if t_full != n.endswith("Full"):
            full_bad.append((n, list(pt)))
    if full_bad:
        raise SystemExit(
            "ERROR: 结构族里「名字带 Full」与「参数类型带 full」不一致：%s。"
            " isFull 由参数类型推出（不是手写），两者不一致说明 Scintilla 改了命名"
            " 或本脚本解析退化。" % full_bad)

    def is_full(n):
        return any(t.endswith("full") for t in features[n]["ptypes"])

    entries = sorted((features[n]["id"], n, kinds[n], is_full(n))
                     for n in struct_names)
    counts = {k: 0 for k in STRUCT_ORDER}
    for _, _, k, _f in entries:
        counts[k] += 1
    return entries, counts


def build(root):
    features, deprecated, dup = parse_iface(root)
    ptr_map = parse_call_h(root)
    header = parse_msgs_h(root)

    problems = []
    if dup:
        problems.append("iface 里有重复特征名：%s" % sorted(set(dup))[:8])
    if len(features) != EXPECT_FEATURES:
        problems.append("iface 带编号特征 %d 条，期望 %d 条（Scintilla 升级了？）"
                        % (len(features), EXPECT_FEATURES))
    if len(header) != EXPECT_HEADER:
        problems.append("ScintillaMessages.h %d 条，期望 %d 条（Scintilla 升级了？）"
                        % (len(header), EXPECT_HEADER))
    if len(deprecated) != EXPECT_DEPRECATED:
        problems.append("iface Deprecated 段 %d 条，期望 %d 条"
                        % (len(deprecated), EXPECT_DEPRECATED))

    # C：id 集合对账
    only_iface = set(features) - set(header)
    if only_iface != deprecated:
        problems.append("iface 比 ScintillaMessages.h 多出的名字（%d 个，前 8）%s 不等于 Deprecated 段 %s"
                        % (len(only_iface), sorted(only_iface)[:8], sorted(deprecated)))
    only_header = set(header) - set(features)
    if only_header:
        problems.append("ScintillaMessages.h 比 iface 多出的名字：%s"
                        % sorted(only_header)[:8])
    mism = [n for n in set(features) & set(header) if features[n]["id"] != header[n]]
    if mism:
        problems.append("id 不一致：%s" % [(n, features[n]["id"], header[n])
                                          for n in sorted(mism)[:8]])

    # A/B 对撞 + 类型词表穷举
    unknown_types = set()
    matched = disagree = 0
    dis_names = []
    unmatched = set()
    shapes = {}
    over_params = []
    for name, f in features.items():
        for t in f["ptypes"] + [f["ret"]]:
            # 空串 = 空的参数槽（iface 的 "(null value)"），不是类型 ⇒ 跳过。
            if t and t not in PTR_TYPES and t not in VALUE_TYPES:
                unknown_types.add(t)
        # 布局判据的前提：SCI_* 至多两个参数（槽 0 → wParam，槽 1 → lParam）
        if len(f["ptypes"]) > 2:
            over_params.append((name, f["ptypes"]))
        a_ptr = (f["ret"] == "pointer") or any(t in PTR_TYPES for t in f["ptypes"])
        b_ptr = lookup_call(ptr_map, name)
        if b_ptr is None:
            unmatched.add(name)
        else:
            matched += 1
            if b_ptr != a_ptr:
                disagree += 1
                dis_names.append((name, a_ptr, b_ptr))
        shapes[name] = shape_of(f["ret"], f["ptypes"])

    if unknown_types:
        problems.append(
            "参数/返回类型词表里出现了没见过的类型：%s\n"
            "  ⇒ 停下来复核：它是指针类型就加进 PTR_TYPES，是整数/枚举就加进 VALUE_TYPES，\n"
            "     然后重跑本脚本。**不要**图省事默认成值类型 —— 那正是「危险地可用」的来源。"
            % sorted(unknown_types))
    if matched < EXPECT_MATCHED_MIN:
        problems.append("判据 B 只覆盖 %d 个名字（下限 %d）⇒ B 的解析可能退化了"
                        % (matched, EXPECT_MATCHED_MIN))
    if disagree:
        problems.append("判据 A 与 B 分歧 %d 处：%s" % (disagree, dis_names[:8]))
    if unmatched != UNMATCHED_EXPECTED:
        problems.append("B 未覆盖集合变了：多出 %s，少了 %s"
                        % (sorted(unmatched - UNMATCHED_EXPECTED),
                           sorted(UNMATCHED_EXPECTED - unmatched)))

    ptr_total = sum(1 for n in features if shapes[n] != "kValue")
    if ptr_total != EXPECT_PTR:
        problems.append("带指针条数 %d，期望 %d" % (ptr_total, EXPECT_PTR))

    if over_params:
        problems.append(
            "有特征带 3 个以上参数（%d 条，前 3）：%s\n"
            "  ⇒ 布局判据假定「第 1 个参数 → wParam、第 2 个 → lParam」，这个前提\n"
            "     被打破了。停下来复核 in_layout_of 与 SCI_* 的 wp/lp 映射。"
            % (len(over_params), over_params[:3]))

    in_entries, in_counts, in_len_named, in_both = build_inbound(features, shapes)
    if len(in_entries) != EXPECT_IN_TOTAL:
        problems.append("入参族 %d 条，期望 %d 条（= kInStr + kInBytes）"
                        % (len(in_entries), EXPECT_IN_TOTAL))
    for k in IN_LAYOUT_ORDER:
        if in_counts.get(k, 0) != EXPECT_IN_BUCKETS[k]:
            problems.append("布局 %s 有 %d 条，期望 %d 条"
                            % (k, in_counts.get(k, 0), EXPECT_IN_BUCKETS[k]))
    if in_len_named != EXPECT_LEN_NAMED:
        problems.append(
            "首参名精确为 length 的名单变了：多出 %s，少了 %s\n"
            "  ⇒ 复核 in_layout_of 的 == 判据（别改成前缀匹配：AutoCShow 的\n"
            "     lengthEntered 会被误分进长度桶，静默截断补全列表）。"
            % (sorted(in_len_named - EXPECT_LEN_NAMED),
               sorted(EXPECT_LEN_NAMED - in_len_named)))
    if in_both != EXPECT_BOTH_PTR:
        problems.append("双指针名单变了：多出 %s，少了 %s"
                        % (sorted(in_both - EXPECT_BOTH_PTR),
                           sorted(EXPECT_BOTH_PTR - in_both)))

    # ★ 三张族的"期望值"常量在这里**真正生效**。此前生成器声明了
    #   EXPECT_OUT_TOTAL / EXPECT_OUT_BUCKETS / EXPECT_INOUT_TOTAL /
    #   EXPECT_INOUT_BUCKETS 却**没有任何地方读它们** —— 等于没有守卫（规模与桶
    #   分布只有测试侧在数，生成器这边"看起来在校"其实没校）。现在接上：
    #   Scintilla 升级改变任何一族的规模或桶分布，这里立刻报错。
    #   build_struct() 自己还会做形状/名单/Full 一致性的前提断言（不一致会
    #   直接 SystemExit）。
    out_e, out_c = build_outbound(features, shapes)
    inout_e, inout_c = build_inout(features, shapes)
    struct_e, struct_c = build_struct(features, shapes)
    for label, ents, cnts, total, buckets, order in (
            ("出参族", out_e, out_c, EXPECT_OUT_TOTAL, EXPECT_OUT_BUCKETS, OUT_ORDER),
            ("出入参族", inout_e, inout_c, EXPECT_INOUT_TOTAL,
             EXPECT_INOUT_BUCKETS, INOUT_ORDER),
            ("结构族", struct_e, struct_c, EXPECT_STRUCT_TOTAL,
             EXPECT_STRUCT_BUCKETS, STRUCT_ORDER)):
        if len(ents) != total:
            problems.append("%s %d 条，期望 %d 条（Scintilla 升级了？）"
                            % (label, len(ents), total))
        for k in order:
            if cnts.get(k, 0) != buckets[k]:
                problems.append("%s 的 %s 有 %d 条，期望 %d 条"
                                % (label, k, cnts.get(k, 0), buckets[k]))

    if problems:
        for p in problems:
            print("ERROR: %s" % p, file=sys.stderr)
        raise SystemExit(1)

    entries = sorted(((f["id"], n, shapes[n]) for n, f in features.items()),
                     key=lambda e: e[0])
    counts = {s: 0 for s in SHAPE_ORDER}
    for _, _, s in entries:
        counts[s] += 1
    return entries, counts, {"matched": matched, "unmatched": len(unmatched),
                             "ptr": ptr_total}, \
        (in_entries, in_counts, len(in_len_named), len(in_both))


def render(entries, counts, stats):
    lines = []
    a = lines.append
    a("// " + "=" * 74)
    a("// SciMarshalTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。")
    a("//")
    a("// 源：third_party/scintilla/include/Scintilla.iface")
    a("//     （Scintilla 自己的机器可读接口定义；ScintillaMessages.h 由它渲染）")
    a("// 对撞：third_party/scintilla/include/ScintillaCall.h（C++ 签名，独立判据）")
    a("//       third_party/scintilla/include/ScintillaMessages.h（id 集合）")
    a("//")
    a("// 重新生成 / 校验：")
    a("//   python scripts/gen-sci-marshal.py            # 生成")
    a("//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）")
    a("// " + "=" * 74)
    a("")
    a("inline constexpr unsigned kSciMsgMin = %d;" % entries[0][0])
    a("inline constexpr unsigned kSciMsgMax = %d;" % entries[-1][0])
    a("inline constexpr unsigned kSciCount  = %d;" % len(entries))
    a("")
    a("// 形状分布（tests/test_sci_marshal.cpp 会逐条重数并与这里对账）")
    for s in SHAPE_ORDER:
        a("inline constexpr unsigned kSciCount%s = %d;"
          % (s[1:], counts.get(s, 0)))
    a("")
    a("// 按 msg 严格升序（SciShapeOf 二分查找的前提）")
    a("inline constexpr SciEntry kSciTable[kSciCount] = {")
    for msg, name, shape in entries:
        a(("    { %4d, SciShape::%-9s },   // %s" % (msg, shape, name)).rstrip())
    a("};")
    a("")
    return "\r\n".join(lines) + "\r\n"


def render_bridge(in_entries, in_counts, len_named_count, both_count):
    lines = []
    a = lines.append
    a("// " + "=" * 74)
    a("// SciBridgeTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。")
    a("//")
    a("// 入参指针族（kInStr + kInBytes）的**参数布局**：指针在 wp 还是 lp、")
    a("// 长度从哪来。形状表（SciMarshalTable.inc）只说「有没有指针」，这张表说")
    a("// 「怎么把它搬过来」。")
    a("//")
    a("// 源：third_party/scintilla/include/Scintilla.iface（参数**类型 + 名字**）")
    a("//")
    a("// 三种反直觉形态（手写必错，所以生成）：")
    a("//   * kStrInWp    —— 单参数消息把串放在 **wParam**（%d 条里的多数）"
      % in_counts.get("kStrInWp", 0))
    a("//   * kStrBoth    —— wp 与 lp 都是串，要内联两段（%d 条）" % both_count)
    a("//   * kStrInLp    —— 含 AutoCShow：首参 lengthEntered **不是**缓冲长度，")
    a("//                    必须 NUL 扫描（长度桶只有首参名精确等于 length 的 %d 条）"
      % len_named_count)
    a("//")
    a("// 重新生成 / 校验：")
    a("//   python scripts/gen-sci-marshal.py            # 生成")
    a("//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）")
    a("// " + "=" * 74)
    a("")
    a("inline constexpr unsigned kSciInCount = %d;" % len(in_entries))
    a("")
    a("// 布局分布（tests/test_sci_bridge.cpp 会逐条重数并与这里对账）")
    for k in IN_LAYOUT_ORDER:
        a("inline constexpr unsigned kSciInCount%s = %d;"
          % (k[1:], in_counts.get(k, 0)))
    a("")
    a("// 按 msg 严格升序（SciInLayoutOf 二分查找的前提）")
    a("inline constexpr SciInEntry kSciInTable[kSciInCount] = {")
    for msg, name, lay in in_entries:
        a(("    { %4d, SciInLayout::%-14s },   // %s" % (msg, lay, name)).rstrip())
    a("};")
    a("")
    return "\r\n".join(lines) + "\r\n"


def render_out(out_entries, out_counts):
    lines = []
    a = lines.append
    a("// " + "=" * 74)
    a("// SciOutTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。")
    a("//")
    a("// 出参指针族（kOutStr，%d 条）的**容量与 NUL 语义**：宿主该分配多大缓冲、"
      % len(out_entries))
    a("// 回传给插件多少字节。形状表说「有指针」，这张表说「出参怎么搬回来」。")
    a("//")
    a("// 源：Scintilla 5.6.6 的**实现**（third_party/scintilla/src/Editor.cxx、")
    a("//     ScintillaBase.cxx、CharClassify.cxx、win32/ScintillaWin.cxx）——")
    a("//     iface 只说「返回长度」，不说容量，容量规则只存在于实现里。")
    a("//")
    a("// need = 同一条消息 lParam==0 的返回值（Scintilla 出参协议自带的查长度调用）")
    a("//   kNul           -> 回传 need + 1          实现写 len+1（含结尾 NUL）")
    a("//   kNoNul         -> 回传 need              实现写 len（**不含** NUL）")
    a("//   kClampWpNul    -> 回传 min(need, wp) + 1 写之前被 wParam 钳制")
    a("//   kRefuseRuntime -> 拒答                   写入量随运行期状态变化")
    a("//")
    a("// ★ 差一个字节就是越界写 1 字节（堆破坏，而且多半不崩）⇒ 这份分类不能手写。")
    a("//   kNul 与 kNoNul 的边界只差一个字节，靠 iface 看不出来，必须读实现。")
    a("//   kRefuseRuntime 的两条：")
    a("//     · TargetAsUTF8 —— Unicode 模式走 GetCharRange（不写 NUL），非 Unicode")
    a("//       模式走 MultiByteFromWideChar 且写 text[len]=0 ⇒ 各错一半。")
    a("//     · GetTag —— 写入量取决于有没有配 regex（有 ⇒ need+1；无 ⇒ 只写 1 字节），")
    a("//       返回值恒是那个先写死的 length ⇒ probe 的返回值不是容量。")
    a("//       这条反例是 tests/test_sci_out.cpp 的**真 Scintilla 实证**抓出来的：")
    a("//       静态读实现只看到 memcpy 那一支，会把它错判成 kNul。")
    a("//")
    a("// 判据 A = 生成器里冻结的四份名单（附实现行号）；判据 B = tests/test_sci_out.cpp")
    a("//   的**真 Scintilla 实证**（哨兵字节测出实际写入量，必须等于本表的规则）。")
    a("//")
    a("// 重新生成 / 校验：")
    a("//   python scripts/gen-sci-marshal.py            # 生成")
    a("//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）")
    a("// " + "=" * 74)
    a("")
    a("inline constexpr unsigned kSciOutCount = %d;" % len(out_entries))
    a("")
    a("// 桶分布（tests/test_sci_out.cpp 会逐条重数并与这里对账）")
    for k in OUT_ORDER:
        a("inline constexpr unsigned kSciOutCount%s = %d;" % (k[1:], out_counts.get(k, 0)))
    a("")
    a("// 按 msg 严格升序（SciOutKindOf 二分查找的前提）")
    a("inline constexpr SciOutEntry kSciOutTable[kSciOutCount] = {")
    for msg, name, kind in out_entries:
        a(("    { %4d, SciOutKind::%-14s },   // %s" % (msg, kind, name)).rstrip())
    a("};")
    a("")
    return NL.join(lines) + NL


def render_inout(inout_entries, inout_counts):
    lines = []
    a = lines.append
    a("// " + "=" * 74)
    a("// SciInOutTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。")
    a("//")
    a("// 出入参族（kInOutStr，%d 条）：**入参串 + 出参缓冲**同时存在。"
      % len(inout_entries))
    a("// 形状是 (string, stringresult) ⇒ 入参串在 wParam（NUL 结尾）、出参缓冲在")
    a("// lParam。出参侧的容量规则与批次 114 的 kOutStr **完全一样**（同属")
    a("// SciOutKind 桶空间），本表只额外回答「这条消息要不要带入参串」。")
    a("//")
    a("// 源：Scintilla 5.6.6 的**实现**（src/ScintillaBase.cxx、src/Editor.cxx、")
    a("//     win32/ScintillaWin.cxx）。")
    a("//")
    a("// need = 同一条消息 lParam==0 的返回值（Scintilla 出参协议自带的查长度调用）")
    a("//   kNul           -> 回传 need + 1   实现走 Editor::StringResult（写 len+1）")
    a("//   kRefuseRuntime -> 拒答            容量/写入量静态分不了")
    a("//")
    a("// kNul 三条（全部经 Editor::StringResult）：")
    a("//   · GetProperty(4008) / GetPropertyExpanded(4009)")
    a("//       ScintillaBase.cxx:1079 / 1082 —— StringResult(lParam,")
    a("//       DocumentLexState()->PropGet(wParam))。PropGet 可能返回 nullptr")
    a("//       （没装词法器时 LexState::instance 为空，ScintillaBase.cxx:717），")
    a("//       StringResult 对 nullptr 写 1 个 NUL、返回 0 ⇒ 与「空串」同形，")
    a("//       所以仍然落在 need+1 里。")
    a("//   · DescribeProperty(4016)")
    a("//       ScintillaBase.cxx:1110 —— StringResult(lParam, DescribeProperty(wParam))。")
    a("//")
    a("// kRefuseRuntime 两条：")
    a("//   · EncodedFromUTF8(2449) —— 两个独立理由：")
    a("//     ① 入参长度取自成员 lengthForEncode，由**伴生消息**")
    a("//        SetLengthForEncode(2448) 设定（Editor.cxx:8520）⇒ 代理侧看不到；")
    a("//     ② NUL 语义随 IsUnicodeMode() 变：Unicode 模式 memcpy(encoded, utf8, n)")
    a("//        **不写 NUL**；非 Unicode 模式 MultiByteFromWideChar 后 encoded[n]='\\0'。")
    a("//   · GetRepresentation(2666) —— probe 分不了两种 0：")
    a("//       repr 不存在 ⇒ 直接 return 0，**0 字节**；")
    a("//       repr 存在但内容为空串 ⇒ StringResult，**1 个 NUL**。")
    a("//       两者 probe 都返回 0 ⇒ 容量不可知 ⇒ 拒答。")
    a("//       这条歧义由 tests/test_sci_inout.cpp 用真 Scintilla 直接演示。")
    a("//")
    a("// 判据 A = 生成器里冻结的两份名单（附实现行号）；判据 B =")
    a("//   tests/test_sci_inout.cpp 的**真 Scintilla 实证** + test_oop 场景 14 的 e2e。")
    a("//")
    a("// 重新生成 / 校验：")
    a("//   python scripts/gen-sci-marshal.py            # 生成")
    a("//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）")
    a("// " + "=" * 74)
    a("")
    a("inline constexpr unsigned kSciInOutCount = %d;" % len(inout_entries))
    a("")
    a("// 桶分布（tests/test_sci_inout.cpp 会逐条重数并与这里对账）")
    for k in INOUT_ORDER:
        a("inline constexpr unsigned kSciInOutCount%s = %d;"
          % (k[1:], inout_counts.get(k, 0)))
    a("")
    a("// 按 msg 严格升序（SciOutKindOf 二分查找的前提）")
    a("inline constexpr SciOutEntry kSciInOutTable[kSciInOutCount] = {")
    for msg, name, kind in inout_entries:
        a(("    { %4d, SciOutKind::%-14s },   // %s" % (msg, kind, name)).rstrip())
    a("};")
    a("")
    return NL.join(lines) + NL


def render_struct(struct_entries, struct_counts):
    lines = []
    a = lines.append
    a("// " + "=" * 74)
    a("// SciStructTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。")
    a("//")
    a("// 结构体指针族（kStruct，%d 条）：指针指向**结构体**，结构体里**还有指针**"
      % len(struct_entries))
    a("// ⇒ 必须把结构体**展平**进 wire、把里面的串**内联**。四类：")
    a("//")
    a("//   kRangeOut     出缓冲，容量 = len + 1       GetTextRange / GetTextRangeFull")
    a("//   kStyledOut    出缓冲，容量 = 2*len + 2     GetStyledText / GetStyledTextFull")
    a("//   kFindInOut    入串 + **条件**回写 chrgText FindText / FindTextFull")
    a("//   kRefuseHandle 拒答                         FormatRange / FormatRangeFull")
    a("//")
    a("// full = 该条用 64 位的 Sci_Position（Sci_CharacterRangeFull），否则用 32 位")
    a("//        的 Sci_PositionCR = long（Sci_CharacterRange）。**由参数类型推出**")
    a("//        （生成器里断言「类型以 full 结尾」⟺「名字以 Full 结尾」），不是手写。")
    a("//")
    a("// 源：Scintilla 5.6.6 的**实现**（src/Editor.cxx、src/CellBuffer.cxx）。")
    a("//")
    a("// 容量（★ 与批次 114 的 kOutStr **相反**：这里算得出，不需要探长度）")
    a("//   kRangeOut  len = (cpMax == -1 ? docLen : cpMax) - cpMin；写 len+1")
    a("//              Editor.cxx:6047 —— buffer[len] = '\\0'（含结尾 NUL）。")
    a("//              ⚠ CellBuffer.cxx:358 在越界时**一个字节都不写**就返回，而")
    a("//                Editor::GetTextRange 仍写 buffer[len]='\\0' 并返回 len")
    a("//                ⇒ 越界范围会得到「前面全是垃圾」。桥接**故意收窄**：")
    a("//                cpEnd > docLen 一律拒答。")
    a("//   kStyledOut len = cpMax - cpMin；写 2*len + 2")
    a("//              Editor.cxx:6036 —— 每字符 (char, style) 两字节，末尾两个 NUL。")
    a("//")
    a("// kFindInOut（Editor.cxx:4302 / 4333）")
    a("//   读 chrg.cpMin/cpMax + lpstrText（NUL 结尾的 needle），**只在 pos != -1 时**")
    a("//   写 chrgText（4318 / 4349）⇒ 回写是**有条件的**。桥接用 canary 判定")
    a("//   「到底写没写」，**不复刻 pos != -1 这条规则**（复刻 = 把产品逻辑抄一份")
    a("//   到桥上，两份会漂）。")
    a("//")
    a("// kRefuseHandle（Editor.cxx:1968）")
    a("//   AutoSurface(pfr->hdc, …) / AutoSurface(pfr->hdcTarget, …) 用的是**调用进程**")
    a("//   的 HDC —— GDI 句柄表进程私有；且 view.FormatRange 是**绘制**操作；公开结构")
    a("//   Sci_FormatRange 还带 HWND ⇒ 语义上**永久不可桥接**（不是「本批没做」）。")
    a("//")
    a("// 判据 A = 生成器里冻结的四份名单（附实现行号）；判据 B =")
    a("//   tests/test_sci_struct.cpp 的**真 Scintilla 实证**（与进程内直调逐字节对撞）")
    a("//   + test_oop 场景 15 的 e2e。")
    a("//")
    a("// 重新生成 / 校验：")
    a("//   python scripts/gen-sci-marshal.py            # 生成")
    a("//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）")
    a("// " + "=" * 74)
    a("")
    a("inline constexpr unsigned kSciStructCount = %d;" % len(struct_entries))
    a("")
    a("// 桶分布（tests/test_sci_struct.cpp 会逐条重数并与这里对账）")
    for k in STRUCT_ORDER:
        a("inline constexpr unsigned kSciStructCount%s = %d;"
          % (k[1:], struct_counts.get(k, 0)))
    a("")
    a("// 按 msg 严格升序（SciStructKindOf 二分查找的前提）")
    a("inline constexpr SciStructEntry kSciStructTable[kSciStructCount] = {")
    for msg, name, kind, full in struct_entries:
        a(("    { %4d, SciStructKind::%-14s, %-5s },   // %s"
           % (msg, kind, "true" if full else "false", name)).rstrip())
    a("};")
    a("")
    return NL.join(lines) + NL


def _write_atomic(path, text):
    """临时文件 + 原子替换。

    别用 open(path, "w") 直接写目标文件：open 会**先截断**再校验参数，参数一错
    就把源文件清成 0 字节（本轮实测踩过）。写字节而不是文本，也顺手避开任何
    newline 翻译（文本里已经是 CRLF）。
    """
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(text.encode("utf-8"))
    os.replace(tmp, path)


def write_or_check(path, text, label):
    """--check：一致返回 None；不一致打印首处差异并返回 1。"""
    try:
        with open(path, "rb") as fh:
            cur = fh.read()
    except OSError as e:
        print("RESULT: STALE —— 读不到 %s：%s" % (label, e))
        return 1
    data = text.encode("utf-8")
    if cur == data:
        return None
    cur_lines = cur.decode("utf-8", "replace").split("\r\n")
    new_lines = text.split("\r\n")
    first = next((i for i in range(min(len(cur_lines), len(new_lines)))
                  if cur_lines[i] != new_lines[i]), None)
    print("RESULT: STALE —— %s 与生成结果不一致（磁盘 %d 行 / 应为 %d 行）"
          % (label, len(cur_lines), len(new_lines)))
    if first is not None:
        print("  首处差异在第 %d 行：" % (first + 1))
        print("    磁盘: %s" % cur_lines[first].strip())
        print("    应为: %s" % new_lines[first].strip())
    return 1


def main():
    argv = sys.argv[1:]
    try:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    except OSError:
        root = "."
    os.chdir(root)

    if "--dump-types" in argv:
        feats, _dep, _dup = parse_iface(root)
        seen_p, seen_v = set(), set()
        for f in feats.values():
            for t in f["ptypes"] + [f["ret"]]:
                (seen_p if t in PTR_TYPES else seen_v).add(t)
        print("PTR_TYPES : %s" % sorted(seen_p))
        print("VALUE_TYPES (%d):" % len(seen_v))
        for t in sorted(seen_v):
            print('    "%s",' % t)
        return 0

    if "--dump-inbound" in argv:
        feats, _dep, _dup = parse_iface(root)
        shapes = {n: shape_of(f["ret"], f["ptypes"]) for n, f in feats.items()}
        ents, cnts, len_named, both = build_inbound(feats, shapes)
        print("入参族 %d 条：%s" % (len(ents), " / ".join(
            "%s %d" % (k, cnts[k]) for k in IN_LAYOUT_ORDER)))
        print("首参名 == length 的 %d 条（长度桶）：" % len(len_named))
        for n in sorted(len_named):
            print("    %s" % n)
        print("双指针 %d 条：%s" % (len(both), sorted(both)))
        for msg, name, lay in ents:
            print("%5d  %-30s %s" % (msg, name, lay))
        return 0

    if "--dump-outbound" in argv:
        feats, _dep, _dup = parse_iface(root)
        shapes = {n: shape_of(f["ret"], f["ptypes"]) for n, f in feats.items()}
        ents, cnts = build_outbound(feats, shapes)
        print("出参族 %d 条：%s" % (len(ents), " / ".join(
            "%s %d" % (k, cnts[k]) for k in OUT_ORDER)))
        for msg, name, kind in ents:
            print("%5d  %-32s %s" % (msg, name, kind))
        return 0

    if "--dump-inout" in argv:
        feats, _dep, _dup = parse_iface(root)
        shapes = {n: shape_of(f["ret"], f["ptypes"]) for n, f in feats.items()}
        ents, cnts = build_inout(feats, shapes)
        print("出入参族 %d 条：%s" % (len(ents), " / ".join(
            "%s %d" % (k, cnts[k]) for k in INOUT_ORDER)))
        for msg, name, kind in ents:
            print("%5d  %-32s %s" % (msg, name, kind))
        return 0

    if "--dump-struct" in argv:
        feats, _dep, _dup = parse_iface(root)
        shapes = {n: shape_of(f["ret"], f["ptypes"]) for n, f in feats.items()}
        ents, cnts = build_struct(feats, shapes)
        print("结构族 %d 条：%s" % (len(ents), " / ".join(
            "%s %d" % (k, cnts[k]) for k in STRUCT_ORDER)))
        for msg, name, kind, full in ents:
            print("%5d  %-32s %-14s full=%s" % (msg, name, kind,
                                                "yes" if full else "no"))
        return 0

    if "--show" in argv:
        i = argv.index("--show")
        want = argv[i + 1:]
        feats, _dep, _dup = parse_iface(root)
        for n in want:
            if n not in feats:
                print("%-28s <不在 iface 里>" % n)
                continue
            f = feats[n]
            sh = shape_of(f["ret"], f["ptypes"])
            # kOutStr 与 kInOutStr **共用**同一套 SciOutKind 桶空间（出参侧规则
            # 一样），所以这里也合并成一张查找表来显示。
            kinds = {}
            for nm in OUT_NUL_NAMED:
                kinds[nm] = "kNul"
            for nm in OUT_NONUL_NAMED:
                kinds[nm] = "kNoNul"
            for nm in OUT_CLAMP_NAMED:
                kinds[nm] = "kClampWpNul"
            for nm in OUT_REFUSE_NAMED:
                kinds[nm] = "kRefuseRuntime"
            for nm in INOUT_NUL_NAMED:
                kinds[nm] = "kNul"
            for nm in INOUT_REFUSE_NAMED:
                kinds[nm] = "kRefuseRuntime"
            for nm in STRUCT_RANGE_NAMED:
                kinds[nm] = "kRangeOut"
            for nm in STRUCT_STYLED_NAMED:
                kinds[nm] = "kStyledOut"
            for nm in STRUCT_FIND_NAMED:
                kinds[nm] = "kFindInOut"
            for nm in STRUCT_REFUSE_NAMED:
                kinds[nm] = "kRefuseHandle"
            print("%-28s id=%-5d shape=%-10s layout=%-14s out=%-14s inStr=%-3s "
                  "struct=%-13s ret=%-9s params=%s"
                  % (n, f["id"], sh, in_layout_of(f["ptypes"], f["pnames"]),
                     kinds.get(n, "-") if sh in ("kOutStr", "kInOutStr") else "-",
                     "yes" if sh == "kInOutStr" else "-",
                     kinds.get(n, "-") if sh == "kStruct" else "-",
                     f["ret"], list(zip(f["ptypes"], f["pnames"]))))
        return 0

    entries, counts, stats, inb = build(root)
    in_entries, in_counts, len_named_count, both_count = inb
    feats, _dep, _dup = parse_iface(root)
    shapes = {n: shape_of(f["ret"], f["ptypes"]) for n, f in feats.items()}
    out_entries, out_counts = build_outbound(feats, shapes)
    inout_entries, inout_counts = build_inout(feats, shapes)
    struct_entries, struct_counts = build_struct(feats, shapes)
    text = render(entries, counts, stats)
    text_bridge = render_bridge(in_entries, in_counts, len_named_count, both_count)
    text_out = render_out(out_entries, out_counts)
    text_inout = render_inout(inout_entries, inout_counts)
    text_struct = render_struct(struct_entries, struct_counts)
    path = os.path.join(root, OUT.replace("/", os.sep))
    path_bridge = os.path.join(root, OUT_BRIDGE.replace("/", os.sep))
    path_out = os.path.join(root, OUT_OUTBOUND.replace("/", os.sep))
    path_inout = os.path.join(root, OUT_INOUT.replace("/", os.sep))
    path_struct = os.path.join(root, OUT_STRUCT.replace("/", os.sep))

    if "--check" in argv:
        bad = 0
        bad |= write_or_check(path, text, OUT) or 0
        bad |= write_or_check(path_bridge, text_bridge, OUT_BRIDGE) or 0
        bad |= write_or_check(path_out, text_out, OUT_OUTBOUND) or 0
        bad |= write_or_check(path_inout, text_inout, OUT_INOUT) or 0
        bad |= write_or_check(path_struct, text_struct, OUT_STRUCT) or 0
        if bad:
            print("  修法：python scripts/gen-sci-marshal.py")
            return 1
        print("RESULT: CLEAN —— 五张表都与 Scintilla 接口定义一致")
        print("  %s：%d 条（kValue %d / 带指针 %d；A/B 对撞覆盖 %d 个名字，未覆盖 %d）"
              % (OUT, len(entries), counts.get("kValue", 0), stats["ptr"],
                 stats["matched"], stats["unmatched"]))
        print("  %s：入参族 %d 条（%s）"
              % (OUT_BRIDGE, len(in_entries), " / ".join(
                  "%s %d" % (k, in_counts[k]) for k in IN_LAYOUT_ORDER)))
        print("  %s：出参族 %d 条（%s）"
              % (OUT_OUTBOUND, len(out_entries), " / ".join(
                  "%s %d" % (k, out_counts[k]) for k in OUT_ORDER)))
        print("  %s：出入参族 %d 条（%s）"
              % (OUT_INOUT, len(inout_entries), " / ".join(
                  "%s %d" % (k, inout_counts[k]) for k in INOUT_ORDER)))
        print("  %s：结构族 %d 条（%s）"
              % (OUT_STRUCT, len(struct_entries), " / ".join(
                  "%s %d" % (k, struct_counts[k]) for k in STRUCT_ORDER)))
        return 0

    _write_atomic(path, text)
    _write_atomic(path_bridge, text_bridge)
    print("已生成 %s" % OUT)
    print("  %d 条：%s" % (len(entries),
                           " / ".join("%s %d" % (s, counts.get(s, 0))
                                      for s in SHAPE_ORDER)))
    print("  编号区间 %d..%d" % (entries[0][0], entries[-1][0]))
    print("  判据 A(iface) 带指针 %d 条；判据 B(ScintillaCall.h) 对撞覆盖 %d 个名字、"
          "未覆盖 %d 个（= 7 条 Deprecated + 9 条 Direct/Document）"
          % (stats["ptr"], stats["matched"], stats["unmatched"]))
    print("已生成 %s" % OUT_BRIDGE)
    print("  入参族 %d 条：%s" % (len(in_entries),
                                 " / ".join("%s %d" % (k, in_counts[k])
                                            for k in IN_LAYOUT_ORDER)))
    print("  长度桶名单 %d 条（首参名精确等于 length）/ 双指针 %d 条"
          % (len_named_count, both_count))
    _write_atomic(path_out, text_out)
    print("已生成 %s" % OUT_OUTBOUND)
    print("  出参族 %d 条：%s" % (len(out_entries),
                                 " / ".join("%s %d" % (k, out_counts[k])
                                            for k in OUT_ORDER)))
    _write_atomic(path_inout, text_inout)
    print("已生成 %s" % OUT_INOUT)
    print("  出入参族 %d 条：%s" % (len(inout_entries),
                                   " / ".join("%s %d" % (k, inout_counts[k])
                                              for k in INOUT_ORDER)))
    _write_atomic(path_struct, text_struct)
    print("已生成 %s" % OUT_STRUCT)
    print("  结构族 %d 条：%s" % (len(struct_entries),
                                  " / ".join("%s %d" % (k, struct_counts[k])
                                             for k in STRUCT_ORDER)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
