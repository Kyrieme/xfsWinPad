// ==========================================================================
// SciStructTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。
//
// 结构体指针族（kStruct，8 条）：指针指向**结构体**，结构体里**还有指针**
// ⇒ 必须把结构体**展平**进 wire、把里面的串**内联**。四类：
//
//   kRangeOut     出缓冲，容量 = len + 1       GetTextRange / GetTextRangeFull
//   kStyledOut    出缓冲，容量 = 2*len + 2     GetStyledText / GetStyledTextFull
//   kFindInOut    入串 + **条件**回写 chrgText FindText / FindTextFull
//   kRefuseHandle 拒答                         FormatRange / FormatRangeFull
//
// full = 该条用 64 位的 Sci_Position（Sci_CharacterRangeFull），否则用 32 位
//        的 Sci_PositionCR = long（Sci_CharacterRange）。**由参数类型推出**
//        （生成器里断言「类型以 full 结尾」⟺「名字以 Full 结尾」），不是手写。
//
// 源：Scintilla 5.6.6 的**实现**（src/Editor.cxx、src/CellBuffer.cxx）。
//
// 容量（★ 与批次 114 的 kOutStr **相反**：这里算得出，不需要探长度）
//   kRangeOut  len = (cpMax == -1 ? docLen : cpMax) - cpMin；写 len+1
//              Editor.cxx:6047 —— buffer[len] = '\0'（含结尾 NUL）。
//              ⚠ CellBuffer.cxx:358 在越界时**一个字节都不写**就返回，而
//                Editor::GetTextRange 仍写 buffer[len]='\0' 并返回 len
//                ⇒ 越界范围会得到「前面全是垃圾」。桥接**故意收窄**：
//                cpEnd > docLen 一律拒答。
//   kStyledOut len = cpMax - cpMin；写 2*len + 2
//              Editor.cxx:6036 —— 每字符 (char, style) 两字节，末尾两个 NUL。
//
// kFindInOut（Editor.cxx:4302 / 4333）
//   读 chrg.cpMin/cpMax + lpstrText（NUL 结尾的 needle），**只在 pos != -1 时**
//   写 chrgText（4318 / 4349）⇒ 回写是**有条件的**。桥接用 canary 判定
//   「到底写没写」，**不复刻 pos != -1 这条规则**（复刻 = 把产品逻辑抄一份
//   到桥上，两份会漂）。
//
// kRefuseHandle（Editor.cxx:1968）
//   AutoSurface(pfr->hdc, …) / AutoSurface(pfr->hdcTarget, …) 用的是**调用进程**
//   的 HDC —— GDI 句柄表进程私有；且 view.FormatRange 是**绘制**操作；公开结构
//   Sci_FormatRange 还带 HWND ⇒ 语义上**永久不可桥接**（不是「本批没做」）。
//
// 判据 A = 生成器里冻结的四份名单（附实现行号）；判据 B =
//   tests/test_sci_struct.cpp 的**真 Scintilla 实证**（与进程内直调逐字节对撞）
//   + test_oop 场景 15 的 e2e。
//
// 重新生成 / 校验：
//   python scripts/gen-sci-marshal.py            # 生成
//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）
// ==========================================================================

inline constexpr unsigned kSciStructCount = 8;

// 桶分布（tests/test_sci_struct.cpp 会逐条重数并与这里对账）
inline constexpr unsigned kSciStructCountRangeOut = 2;
inline constexpr unsigned kSciStructCountStyledOut = 2;
inline constexpr unsigned kSciStructCountFindInOut = 2;
inline constexpr unsigned kSciStructCountRefuseHandle = 2;

// 按 msg 严格升序（SciStructKindOf 二分查找的前提）
inline constexpr SciStructEntry kSciStructTable[kSciStructCount] = {
    { 2015, SciStructKind::kStyledOut    , false },   // GetStyledText
    { 2039, SciStructKind::kRangeOut     , true  },   // GetTextRangeFull
    { 2150, SciStructKind::kFindInOut    , false },   // FindText
    { 2151, SciStructKind::kRefuseHandle , false },   // FormatRange
    { 2162, SciStructKind::kRangeOut     , false },   // GetTextRange
    { 2196, SciStructKind::kFindInOut    , true  },   // FindTextFull
    { 2777, SciStructKind::kRefuseHandle , true  },   // FormatRangeFull
    { 2778, SciStructKind::kStyledOut    , true  },   // GetStyledTextFull
};

