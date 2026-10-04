// ==========================================================================
// SciBridgeTable.inc —— 由 scripts/gen-sci-marshal.py 生成，**请勿手改**。
//
// 入参指针族（kInStr + kInBytes）的**参数布局**：指针在 wp 还是 lp、
// 长度从哪来。形状表（SciMarshalTable.inc）只说「有没有指针」，这张表说
// 「怎么把它搬过来」。
//
// 源：third_party/scintilla/include/Scintilla.iface（参数**类型 + 名字**）
//
// 三种反直觉形态（手写必错，所以生成）：
//   * kStrInWp    —— 单参数消息把串放在 **wParam**（7 条里的多数）
//   * kStrBoth    —— wp 与 lp 都是串，要内联两段（2 条）
//   * kStrInLp    —— 含 AutoCShow：首参 lengthEntered **不是**缓冲长度，
//                    必须 NUL 扫描（长度桶只有首参名精确等于 length 的 12 条）
//
// 重新生成 / 校验：
//   python scripts/gen-sci-marshal.py            # 生成
//   python scripts/gen-sci-marshal.py --check    # 校验（CI 守卫）
// ==========================================================================

inline constexpr unsigned kSciInCount = 54;

// 布局分布（tests/test_sci_bridge.cpp 会逐条重数并与这里对账）
inline constexpr unsigned kSciInCountStrInLp = 33;
inline constexpr unsigned kSciInCountStrInLpLen = 11;
inline constexpr unsigned kSciInCountStrInWp = 7;
inline constexpr unsigned kSciInCountStrBoth = 2;
inline constexpr unsigned kSciInCountBytesInLpLen = 1;

// 按 msg 严格升序（SciInLayoutOf 二分查找的前提）
inline constexpr SciInEntry kSciInTable[kSciInCount] = {
    { 2001, SciInLayout::kStrInLpLen    },   // AddText
    { 2002, SciInLayout::kBytesInLpLen  },   // AddStyledText
    { 2003, SciInLayout::kStrInLp       },   // InsertText
    { 2049, SciInLayout::kStrInLp       },   // MarkerDefinePixmap
    { 2056, SciInLayout::kStrInLp       },   // StyleSetFont
    { 2073, SciInLayout::kStrInLpLen    },   // SetStylingEx
    { 2077, SciInLayout::kStrInLp       },   // SetWordChars
    { 2100, SciInLayout::kStrInLp       },   // AutoCShow
    { 2105, SciInLayout::kStrInLp       },   // AutoCStops
    { 2108, SciInLayout::kStrInLp       },   // AutoCSelect
    { 2112, SciInLayout::kStrInLp       },   // AutoCSetFillUps
    { 2117, SciInLayout::kStrInLp       },   // UserListShow
    { 2170, SciInLayout::kStrInLp       },   // ReplaceSel
    { 2181, SciInLayout::kStrInLp       },   // SetText
    { 2194, SciInLayout::kStrInLpLen    },   // ReplaceTarget
    { 2195, SciInLayout::kStrInLpLen    },   // ReplaceTargetRE
    { 2197, SciInLayout::kStrInLpLen    },   // SearchInTarget
    { 2200, SciInLayout::kStrInLp       },   // CallTipShow
    { 2256, SciInLayout::kStrInLp       },   // StyleSetInvisibleRepresentation
    { 2276, SciInLayout::kStrInLp       },   // TextWidth
    { 2282, SciInLayout::kStrInLpLen    },   // AppendText
    { 2367, SciInLayout::kStrInLp       },   // SearchNext
    { 2368, SciInLayout::kStrInLp       },   // SearchPrev
    { 2405, SciInLayout::kStrInLp       },   // RegisterImage
    { 2420, SciInLayout::kStrInLpLen    },   // CopyText
    { 2443, SciInLayout::kStrInLp       },   // SetWhitespaceChars
    { 2530, SciInLayout::kStrInLp       },   // MarginSetText
    { 2534, SciInLayout::kStrInLp       },   // MarginSetStyles
    { 2540, SciInLayout::kStrInLp       },   // AnnotationSetText
    { 2544, SciInLayout::kStrInLp       },   // AnnotationSetStyles
    { 2626, SciInLayout::kStrInLp       },   // MarkerDefineRGBAImage
    { 2627, SciInLayout::kStrInLp       },   // RegisterRGBAImage
    { 2648, SciInLayout::kStrInLp       },   // SetPunctuationChars
    { 2665, SciInLayout::kStrBoth       },   // SetRepresentation
    { 2667, SciInLayout::kStrInWp       },   // ClearRepresentation
    { 2672, SciInLayout::kStrInLpLen    },   // ChangeInsertion
    { 2700, SciInLayout::kStrInLp       },   // ToggleFoldShowText
    { 2722, SciInLayout::kStrInLp       },   // SetDefaultFoldDisplayText
    { 2740, SciInLayout::kStrInLp       },   // EOLAnnotationSetText
    { 2760, SciInLayout::kStrInLp       },   // SetFontLocale
    { 2766, SciInLayout::kStrInWp       },   // SetRepresentationAppearance
    { 2767, SciInLayout::kStrInWp       },   // GetRepresentationAppearance
    { 2768, SciInLayout::kStrInWp       },   // SetRepresentationColour
    { 2769, SciInLayout::kStrInWp       },   // GetRepresentationColour
    { 2771, SciInLayout::kStrInLpLen    },   // ReplaceRectangular
    { 2779, SciInLayout::kStrInLpLen    },   // ReplaceTargetMinimal
    { 2784, SciInLayout::kStrInLp       },   // SetSelectionSerialized
    { 2801, SciInLayout::kStrInLpLen    },   // ChangeLastUndoActionText
    { 2811, SciInLayout::kStrInLp       },   // SetCopySeparator
    { 4004, SciInLayout::kStrBoth       },   // SetProperty
    { 4005, SciInLayout::kStrInLp       },   // SetKeyWords
    { 4010, SciInLayout::kStrInWp       },   // GetPropertyInt
    { 4015, SciInLayout::kStrInWp       },   // PropertyType
    { 4024, SciInLayout::kStrInLp       },   // SetIdentifiers
};

