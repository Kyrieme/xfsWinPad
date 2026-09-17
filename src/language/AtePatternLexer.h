#pragma once
// xfsWinPad - ATE Pattern (.pat) 词法器（批次 72）
// 设计说明见 .cpp。

#include "XfsLexer.h"

namespace xfs {

class AtePatternLexer : public XfsLexerBase {
public:
    AtePatternLexer();

    int LexSpan(const char* text, std::size_t len, Sci_Position basePos,
                int initStyle, XfsStyleSink& sink) override;
    void FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                  Sci_Position last) override;
};

} // namespace xfs
