// ==========================================================================
// SciInOutTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。
//
// 出入参族（kInOutStr，5 条）：**入参串 + 出参缓冲**同时存在。
// 形状是 (string, stringresult) ⇒ 入参串在 wParam（NUL 结尾）、出参缓冲在
// lParam。出参侧的容量规则与批次 114 的 kOutStr **完全一样**（同属
// SciOutKind 桶空间），本表只额外回答「这条消息要不要带入参串」。
//
// 源：Scintilla 5.6.6 的**实现**（src/ScintillaBase.cxx、src/Editor.cxx、
//     win32/ScintillaWin.cxx）。
//
// need = 同一条消息 lParam==0 的返回值（Scintilla 出参协议自带的查长度调用）
//   kNul           -> 回传 need + 1   实现走 Editor::StringResult（写 len+1）
//   kRefuseRuntime -> 拒答            容量/写入量静态分不了
//
// kNul 三条（全部经 Editor::StringResult）：
//   · GetProperty(4008) / GetPropertyExpanded(4009)
//       ScintillaBase.cxx:1079 / 1082 —— StringResult(lParam,
//       DocumentLexState()->PropGet(wParam))。PropGet 可能返回 nullptr
//       （没装词法器时 LexState::instance 为空，ScintillaBase.cxx:717），
//       StringResult 对 nullptr 写 1 个 NUL、返回 0 ⇒ 与「空串」同形，
//       所以仍然落在 need+1 里。
//   · DescribeProperty(4016)
//       ScintillaBase.cxx:1110 —— StringResult(lParam, DescribeProperty(wParam))。
//
// kRefuseRuntime 两条：
//   · EncodedFromUTF8(2449) —— 两个独立理由：
//     ① 入参长度取自成员 lengthForEncode，由**伴生消息**
//        SetLengthForEncode(2448) 设定（Editor.cxx:8520）⇒ 代理侧看不到；
//     ② NUL 语义随 IsUnicodeMode() 变：Unicode 模式 memcpy(encoded, utf8, n)
//        **不写 NUL**；非 Unicode 模式 MultiByteFromWideChar 后 encoded[n]='\0'。
//   · GetRepresentation(2666) —— probe 分不了两种 0：
//       repr 不存在 ⇒ 直接 return 0，**0 字节**；
//       repr 存在但内容为空串 ⇒ StringResult，**1 个 NUL**。
//       两者 probe 都返回 0 ⇒ 容量不可知 ⇒ 拒答。
//       这条歧义由 tests/test_sci_inout.cpp 用真 Scintilla 直接演示。
//
// 判据 A = 生成器里冻结的两份名单（附实现行号）；判据 B =
//   tests/test_sci_inout.cpp 的**真 Scintilla 实证** + test_oop 场景 14 的 e2e。
//
// 重新生成 / 校验：
//   python scripts/gen-sci-marshal.py            # 生成
//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）
// ==========================================================================

inline constexpr unsigned kSciInOutCount = 5;

// 桶分布（tests/test_sci_inout.cpp 会逐条重数并与这里对账）
inline constexpr unsigned kSciInOutCountNul = 3;
inline constexpr unsigned kSciInOutCountRefuseRuntime = 2;

// 按 msg 严格升序（SciOutKindOf 二分查找的前提）
inline constexpr SciOutEntry kSciInOutTable[kSciInOutCount] = {
    { 2449, SciOutKind::kRefuseRuntime },   // EncodedFromUTF8
    { 2666, SciOutKind::kRefuseRuntime },   // GetRepresentation
    { 4008, SciOutKind::kNul           },   // GetProperty
    { 4009, SciOutKind::kNul           },   // GetPropertyExpanded
    { 4016, SciOutKind::kNul           },   // DescribeProperty
};

