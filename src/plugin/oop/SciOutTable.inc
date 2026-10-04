// ==========================================================================
// SciOutTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。
//
// 出参指针族（kOutStr，30 条）的**容量与 NUL 语义**：宿主该分配多大缓冲、
// 回传给插件多少字节。形状表说「有指针」，这张表说「出参怎么搬回来」。
//
// 源：Scintilla 5.6.6 的**实现**（third_party/scintilla/src/Editor.cxx、
//     ScintillaBase.cxx、CharClassify.cxx、win32/ScintillaWin.cxx）——
//     iface 只说「返回长度」，不说容量，容量规则只存在于实现里。
//
// need = 同一条消息 lParam==0 的返回值（Scintilla 出参协议自带的查长度调用）
//   kNul           -> 回传 need + 1          实现写 len+1（含结尾 NUL）
//   kNoNul         -> 回传 need              实现写 len（**不含** NUL）
//   kClampWpNul    -> 回传 min(need, wp) + 1 写之前被 wParam 钳制
//   kRefuseRuntime -> 拒答                   写入量随运行期状态变化
//
// ★ 差一个字节就是越界写 1 字节（堆破坏，而且多半不崩）⇒ 这份分类不能手写。
//   kNul 与 kNoNul 的边界只差一个字节，靠 iface 看不出来，必须读实现。
//   kRefuseRuntime 的两条：
//     · TargetAsUTF8 —— Unicode 模式走 GetCharRange（不写 NUL），非 Unicode
//       模式走 MultiByteFromWideChar 且写 text[len]=0 ⇒ 各错一半。
//     · GetTag —— 写入量取决于有没有配 regex（有 ⇒ need+1；无 ⇒ 只写 1 字节），
//       返回值恒是那个先写死的 length ⇒ probe 的返回值不是容量。
//       这条反例是 tests/test_sci_out.cpp 的**真 Scintilla 实证**抓出来的：
//       静态读实现只看到 memcpy 那一支，会把它错判成 kNul。
//
// 判据 A = 生成器里冻结的四份名单（附实现行号）；判据 B = tests/test_sci_out.cpp
//   的**真 Scintilla 实证**（哨兵字节测出实际写入量，必须等于本表的规则）。
//
// 重新生成 / 校验：
//   python scripts/gen-sci-marshal.py            # 生成
//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）
// ==========================================================================

inline constexpr unsigned kSciOutCount = 30;

// 桶分布（tests/test_sci_out.cpp 会逐条重数并与这里对账）
inline constexpr unsigned kSciOutCountNul = 14;
inline constexpr unsigned kSciOutCountNoNul = 12;
inline constexpr unsigned kSciOutCountClampWpNul = 2;
inline constexpr unsigned kSciOutCountRefuseRuntime = 2;

// 按 msg 严格升序（SciOutKindOf 二分查找的前提）
inline constexpr SciOutEntry kSciOutTable[kSciOutCount] = {
    { 2027, SciOutKind::kClampWpNul    },   // GetCurLine
    { 2153, SciOutKind::kNoNul         },   // GetLine
    { 2161, SciOutKind::kNul           },   // GetSelText
    { 2182, SciOutKind::kClampWpNul    },   // GetText
    { 2257, SciOutKind::kNul           },   // StyleGetInvisibleRepresentation
    { 2447, SciOutKind::kRefuseRuntime },   // TargetAsUTF8
    { 2486, SciOutKind::kNul           },   // StyleGetFont
    { 2531, SciOutKind::kNoNul         },   // MarginGetText
    { 2535, SciOutKind::kNoNul         },   // MarginGetStyles
    { 2541, SciOutKind::kNoNul         },   // AnnotationGetText
    { 2545, SciOutKind::kNoNul         },   // AnnotationGetStyles
    { 2610, SciOutKind::kNul           },   // AutoCGetCurrentText
    { 2616, SciOutKind::kRefuseRuntime },   // GetTag
    { 2646, SciOutKind::kNoNul         },   // GetWordChars
    { 2647, SciOutKind::kNoNul         },   // GetWhitespaceChars
    { 2649, SciOutKind::kNoNul         },   // GetPunctuationChars
    { 2687, SciOutKind::kNoNul         },   // GetTargetText
    { 2723, SciOutKind::kNul           },   // GetDefaultFoldDisplayText
    { 2741, SciOutKind::kNoNul         },   // EOLAnnotationGetText
    { 2761, SciOutKind::kNul           },   // GetFontLocale
    { 2785, SciOutKind::kNoNul         },   // GetSelectionSerialized
    { 2804, SciOutKind::kNoNul         },   // GetUndoActionText
    { 2812, SciOutKind::kNul           },   // GetCopySeparator
    { 4012, SciOutKind::kNul           },   // GetLexerLanguage
    { 4014, SciOutKind::kNul           },   // PropertyNames
    { 4017, SciOutKind::kNul           },   // DescribeKeyWordSets
    { 4026, SciOutKind::kNul           },   // GetSubStyleBases
    { 4030, SciOutKind::kNul           },   // NameOfStyle
    { 4031, SciOutKind::kNul           },   // TagsOfStyle
    { 4032, SciOutKind::kNul           },   // DescriptionOfStyle
};

