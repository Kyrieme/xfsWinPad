#pragma once
// xfsWinPad - STIL (.stil, IEEE 1450) 词法器（批次 72）

#include "XfsLexer.h"

namespace xfs {

class StilLexer : public XfsLexerBase {
public:
    StilLexer();

    int LexSpan(const char* text, std::size_t len, Sci_Position basePos,
                int initStyle, XfsStyleSink& sink) override;
    void FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                  Sci_Position last) override;
};

} // namespace xfs
