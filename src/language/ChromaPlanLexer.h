#pragma once
// xfsWinPad - Chroma 3380 .pln 测试计划词法器（批次 73）
// 设计说明见 .cpp。

#include "XfsLexer.h"

namespace xfs {

class ChromaPlanLexer : public XfsLexerBase {
public:
    ChromaPlanLexer();

    int LexSpan(const char* text, std::size_t len, Sci_Position basePos,
                int initStyle, XfsStyleSink& sink) override;
    void FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                  Sci_Position last) override;
};

} // namespace xfs
