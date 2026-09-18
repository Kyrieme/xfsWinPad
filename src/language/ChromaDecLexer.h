#pragma once
// xfsWinPad - Chroma 3380 .dec 设备定义文件词法器（批次 73）
// 设计说明见 .cpp。

#include "XfsLexer.h"

namespace xfs {

class ChromaDecLexer : public XfsLexerBase {
public:
    ChromaDecLexer();

    int LexSpan(const char* text, std::size_t len, Sci_Position basePos,
                int initStyle, XfsStyleSink& sink) override;
    void FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                  Sci_Position last) override;
};

} // namespace xfs
