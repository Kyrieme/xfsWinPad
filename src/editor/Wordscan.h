#pragma once
// xfsWinPad - 自动补全词汇扫描（批次 31，纯函数可单测，不依赖 Scintilla 控件）
//
// v1（批次 10）把全文按 ASCII 构词字符切段收录；v1.1 增加：
//  1) lexer 风格过滤表：注释/字符串里「只」出现的词不进候选
//     （Sci_TextRange + SCI_GETSTYLEDTEXT 的 (char, style) 交错字节对）；
//  2) 跨标签词汇：其它已开文档的词汇注入候选（Editor::ExtractWords）。

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace xfs {

// 注释/字符串风格位表（风格号 0..127；hypertext 系最大 117）。
// WordStyleFilterFor(lexerName) 返回 nullptr = 该语言不过滤
// （纯文本/markdown/diff/未知词法器——全文都是「内容」）。
struct WordStyleFilter {
    uint64_t bits[2] = {0, 0};   // bit i => 风格 i 被过滤（注释或字符串）

    bool Any() const { return (bits[0] | bits[1]) != 0; }
    bool Test(unsigned s) const {
        return s < 128 && ((bits[s >> 6] >> (s & 63)) & 1) != 0;
    }
};

const WordStyleFilter* WordStyleFilterFor(const char* lexerName);

// 词汇字节 = [A-Za-z0-9_]（与 v1 的 IsWordCharW 同口径，仅 ASCII）
bool IsWordByte(unsigned char c);

// 纯文本扫描：构词连续段长度 >= 3 收录（跨标签文档 / 无风格过滤路径）
void ScanWordsPlain(const char* text, size_t len, std::set<std::string>& out);

// SCI_GETSTYLEDTEXT 的 (char, style) 交错字节对扫描（cellCount = 字符数，
// cells 需要 cellCount*2 字节）。收录规则：词至少有一次出现在
// 非注释非字符串风格——注释里引用过的标识符不受牵连。
void ScanWordsStyled(const unsigned char* cells, size_t cellCount,
                     const WordStyleFilter& f, std::set<std::string>& out);

} // namespace xfs
