#include "LineOps.h"

#include <algorithm>

namespace xfs {
namespace LineOps {

void SplitLines(const std::string& text, std::vector<std::string>* lines,
                bool* endsEol) {
    lines->clear();
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            std::string line = text.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines->push_back(std::move(line));
            start = i + 1;
        } else if (text[i] == '\r') {
            // 单 '\r' 也作行界；"\r\n" 交给 '\n' 分支整体消费
            if (i + 1 < text.size() && text[i + 1] == '\n') continue;
            lines->push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        std::string line = text.substr(start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines->push_back(std::move(line));
        *endsEol = false;
    } else {
        // 恰好以 EOL 结尾（或原文为空）
        *endsEol = !text.empty();
    }
}

std::string JoinLines(const std::vector<std::string>& lines, const char* eol,
                      bool endsEol) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += eol;
    }
    if (endsEol && !lines.empty()) out += eol;
    return out;
}

std::string RemoveEmptyLines(const std::string& text, const char* eol) {
    std::vector<std::string> lines;
    bool endsEol = false;
    SplitLines(text, &lines, &endsEol);
    std::vector<std::string> keep;
    keep.reserve(lines.size());
    for (auto& s : lines) {
        bool blank = true;
        for (char c : s) {
            if (c != ' ' && c != '\t') { blank = false; break; }
        }
        if (!blank) keep.push_back(std::move(s));
    }
    return JoinLines(keep, eol, endsEol && !keep.empty());
}

std::string ReverseLines(const std::string& text, const char* eol) {
    std::vector<std::string> lines;
    bool endsEol = false;
    SplitLines(text, &lines, &endsEol);
    std::reverse(lines.begin(), lines.end());
    return JoinLines(lines, eol, endsEol);
}

std::string ToggleComment(const std::string& text, const std::string& prefix,
                          const char* eol, bool* outCommented) {
    std::vector<std::string> lines;
    bool endsEol = false;
    SplitLines(text, &lines, &endsEol);

    auto isWs = [](char c) { return c == ' ' || c == '\t'; };
    auto firstNonWs = [&](const std::string& s) -> ptrdiff_t {
        for (size_t i = 0; i < s.size(); ++i)
            if (!isWs(s[i])) return (ptrdiff_t)i;
        return -1;
    };
    auto hasPrefix = [&](const std::string& s) -> bool {
        ptrdiff_t p = firstNonWs(s);
        return p >= 0 && s.compare((size_t)p, prefix.size(), prefix) == 0;
    };

    // 判定：全部非空行已带 prefix → 取消；否则逐行补（已带的行不动）。
    // 全空行选区：无目标，不动作，commented=false。
    size_t nonEmpty = 0, with = 0;
    for (const auto& s : lines) {
        if (firstNonWs(s) < 0) continue;
        ++nonEmpty;
        if (hasPrefix(s)) ++with;
    }
    bool uncomment = nonEmpty > 0 && with == nonEmpty;
    if (outCommented) *outCommented = nonEmpty > 0 && !uncomment;

    if (uncomment) {
        for (auto& s : lines) {
            if (!hasPrefix(s)) continue;
            ptrdiff_t p = firstNonWs(s);
            s.erase((size_t)p, prefix.size());
            if ((size_t)p < s.size() && s[(size_t)p] == ' ')
                s.erase((size_t)p, 1);
        }
    } else if (nonEmpty > 0) {
        for (auto& s : lines) {
            ptrdiff_t p = firstNonWs(s);
            if (p < 0) continue;          // 空行不动
            if (hasPrefix(s)) continue;   // 已注释行不动
            s.insert((size_t)p, prefix);
        }
    }
    return JoinLines(lines, eol, endsEol);
}

} // namespace LineOps
} // namespace xfs
