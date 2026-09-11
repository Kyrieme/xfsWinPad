#pragma once
// xfsWinPad - 行变换纯函数（批次 29）：Editor 行操作与单测共用。
// 无窗口/Scintilla 依赖；text 为文档字节串（任意 EOL 风格可解析），重建用 eol。
#include <string>
#include <vector>

namespace xfs {
namespace LineOps {

// 删除空白行（空行或全空白行）。尾部 EOL 状态保持：原文以 EOL 结尾则输出
// 也以 EOL 结尾（全删空 → 空串）。单行无尾 EOL 的原文输出同样无尾 EOL。
std::string RemoveEmptyLines(const std::string& text, const char* eol);

// 行序反转（尾部 EOL 保持在末尾；单行原文原样返回）。
std::string ReverseLines(const std::string& text, const char* eol);

// 行注释切换。全部非空行已带 prefix → 取消（连同 prefix 后紧随的一个空格）；
// 否则在最小缩进列插入 prefix。空行/全空白行不动。outCommented = 是否加了注释。
std::string ToggleComment(const std::string& text, const std::string& prefix,
                          const char* eol, bool* outCommented);

// 按行拆分（\n、\r\n、\r 都识别），输出不含行尾符。
void SplitLines(const std::string& text, std::vector<std::string>* lines,
                bool* endsEol);
// 行集合重组（endsEol=true 时末尾补 eol）。
std::string JoinLines(const std::vector<std::string>& lines, const char* eol,
                      bool endsEol);

} // namespace LineOps
} // namespace xfs
