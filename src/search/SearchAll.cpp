#include "SearchAll.h"
#include "../core/Util.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Scintilla.h>

namespace xfs {

void CollectHitsInText(const std::string& utf8, const std::wstring& fileLabel,
                       const std::wstring& fullPath,
                       const FindState& st, std::vector<SearchHit>* out) {
    if (utf8.empty() || st.text.empty() || !out) return;

    int n = ::WideCharToMultiByte(65001, 0, st.text.c_str(), (int)st.text.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string needle((size_t)n, '\0');
    ::WideCharToMultiByte(65001, 0, st.text.c_str(), (int)st.text.size(),
                          needle.data(), n, nullptr, nullptr);
    if (needle.empty()) return;

    std::string haystack = utf8;
    std::string hayLower, needleLower;
    if (!st.matchCase) {
        hayLower.resize(haystack.size());
        for (size_t i = 0; i < haystack.size(); ++i)
            hayLower[i] = (char)tolower((unsigned char)haystack[i]);
        needleLower.resize(needle.size());
        for (size_t i = 0; i < needle.size(); ++i)
            needleLower[i] = (char)tolower((unsigned char)needle[i]);
    }
    const std::string& hay = st.matchCase ? haystack : hayLower;
    const std::string& ndl = st.matchCase ? needle : needleLower;

    auto isWord = [](char c) {
        unsigned char u = (unsigned char)c;
        return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
               (u >= '0' && u <= '9') || u == '_';
    };

    // walk lines so we can report line numbers and line content
    size_t lineBegin = 0;
    int lineNo = 1;
    size_t pos = 0;
    while (pos <= hay.size()) {
        size_t eol = hay.find('\n', pos);
        size_t lineEnd = (eol == std::string::npos) ? hay.size() : eol;
        size_t contentEnd = lineEnd;
        if (contentEnd > lineBegin && hay[contentEnd - 1] == '\r') --contentEnd;

        size_t from = pos;
        while (true) {
            size_t at = hay.find(ndl, from);
            if (at == std::string::npos || at >= contentEnd) break;
            if (st.wholeWord) {
                bool okBefore = (at == 0) || !isWord(hay[at - 1]);
                size_t after = at + ndl.size();
                bool okAfter = (after >= contentEnd) || !isWord(hay[after]);
                if (!okBefore || !okAfter) { from = at + 1; continue; }
            }
            SearchHit hit;
            hit.file = fileLabel;
            hit.path = fullPath;
            hit.docIndex = -1;
            hit.line = lineNo;
            hit.start = (sptr_t)at;
            hit.end = (sptr_t)(at + ndl.size());
            std::string lineRaw = haystack.substr(lineBegin, contentEnd - lineBegin);
            hit.lineText = Utf8ToWide(lineRaw);
            out->push_back(std::move(hit));
            if (out->size() > 20000) return;   // hard safety cap
            from = at + ndl.size();
            if (ndl.empty()) break;
        }

        if (eol == std::string::npos) break;
        lineBegin = eol + 1;
        pos = lineBegin;
        ++lineNo;
    }
}

void CollectHitsInEditor(Editor& ed, const std::wstring& fileLabel,
                         const FindState& st, std::vector<SearchHit>* out,
                         int docIndex) {
    if (!ed.Valid() || st.text.empty() || !out) return;

    // needle utf8
    int n = ::WideCharToMultiByte(65001, 0, st.text.c_str(), (int)st.text.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string needle((size_t)n, '\0');
    ::WideCharToMultiByte(65001, 0, st.text.c_str(), (int)st.text.size(),
                          needle.data(), n, nullptr, nullptr);

    unsigned int flags = 0;
    if (st.matchCase) flags |= SCFIND_MATCHCASE;
    if (st.wholeWord) flags |= SCFIND_WHOLEWORD;
    if (st.regexp)    flags |= SCFIND_REGEXP;

    size_t before = out->size();
    sptr_t docLen = ed.Send(SCI_GETLENGTH);
    if (docLen <= 0) return;
    ed.Send(SCI_SETSEARCHFLAGS, flags);
    ed.Send(SCI_SETTARGETSTART, 0);
    ed.Send(SCI_SETTARGETEND, docLen);

    while (docLen > 0) {
        sptr_t pos = ed.Send(SCI_SEARCHINTARGET, needle.size(), (LPARAM)needle.c_str());
        if (pos < 0) break;
        sptr_t s = ed.Send(SCI_GETTARGETSTART);
        sptr_t e = ed.Send(SCI_GETTARGETEND);
        int line = (int)ed.Send(SCI_LINEFROMPOSITION, s);

        SearchHit hit;
        hit.file = fileLabel;
        hit.docIndex = docIndex;   // caller-provided workspace index (or -1)
        hit.line = line + 1;
        hit.start = s;
        hit.end = e;

        sptr_t ls = ed.Send(SCI_POSITIONFROMLINE, line);
        sptr_t le = ed.Send(SCI_GETLINEENDPOSITION, line);
        if (le > ls && le - ls < 4096) {
            std::vector<char> buf((size_t)(le - ls) + 1, '\0');
            ed.Send(SCI_SETTARGETSTART, ls);
            ed.Send(SCI_SETTARGETEND, le);
            sptr_t got = ed.Send(SCI_TARGETASUTF8, 0, (LPARAM)buf.data());
            if (got > 0) hit.lineText = Utf8ToWide(std::string(buf.data(), (size_t)got));
        }
        out->push_back(std::move(hit));

        sptr_t next = e;
        if (next <= s) next = s + 1;          // zero-width guard
        if (next >= docLen) break;
        ed.Send(SCI_SETTARGETSTART, next);
        ed.Send(SCI_SETTARGETEND, docLen);
        docLen = ed.Send(SCI_GETLENGTH);      // refresh in case of edits elsewhere
    }
    (void)before;
}

int ReplaceInText(std::string* utf8, const FindState& st) {
    if (!utf8 || st.text.empty() || st.replace == st.text) return 0;

    int n = ::WideCharToMultiByte(65001, 0, st.text.c_str(), (int)st.text.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return 0;
    std::string needle((size_t)n, '\0');
    ::WideCharToMultiByte(65001, 0, st.text.c_str(), (int)st.text.size(),
                          needle.data(), n, nullptr, nullptr);

    n = ::WideCharToMultiByte(65001, 0, st.replace.c_str(), (int)st.replace.size(),
                              nullptr, 0, nullptr, nullptr);
    if (n < 0) return 0;
    std::string repl((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        ::WideCharToMultiByte(65001, 0, st.replace.c_str(), (int)st.replace.size(),
                              repl.data(), n, nullptr, nullptr);

    int count = 0;
    if (!st.regexp && !st.matchCase && !st.wholeWord) {
        size_t pos = 0;
        while ((pos = utf8->find(needle, pos)) != std::string::npos) {
            utf8->replace(pos, needle.size(), repl);
            pos += repl.size();
            ++count;
        }
        return count;
    }

    // general path: collect hits then replace from end
    std::vector<SearchHit> hits;
    CollectHitsInText(*utf8, L"", L"", st, &hits);
    if (hits.empty()) return 0;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        if (it->end > (sptr_t)utf8->size() || it->start >= it->end) continue;
        utf8->replace((size_t)it->start, (size_t)(it->end - it->start), repl);
        ++count;
    }
    return count;
}

} // namespace xfs
