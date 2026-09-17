#pragma once
// xfsWinPad - ATE Log (.log) 词法器（批次 72）

#include "XfsLexer.h"

namespace xfs {

class AteLogLexer : public XfsLexerBase {
public:
    AteLogLexer();

    int LexSpan(const char* text, std::size_t len, Sci_Position basePos,
                int initStyle, XfsStyleSink& sink) override;
};

} // namespace xfs
