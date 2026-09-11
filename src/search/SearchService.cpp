#include "SearchService.h"
#include <Scintilla.h>
#include <string>

namespace xfs {
namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(65001, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    ::WideCharToMultiByte(65001, 0, w.c_str(), (int)w.size(),
                          out.data(), n, nullptr, nullptr);
    return out;
}

unsigned int FlagsOf(const FindState& st) {
    unsigned int flags = 0;
    if (st.matchCase) flags |= SCFIND_MATCHCASE;
    if (st.wholeWord) flags |= SCFIND_WHOLEWORD;
    if (st.regexp)    flags |= SCFIND_REGEXP;
    return flags;
}

// Searches [tStart, tEnd) with the state's flags; returns match start or -1.
sptr_t SearchRange(Editor& ed, const std::string& needle, unsigned int flags,
                   sptr_t tStart, sptr_t tEnd) {
    ed.Send(SCI_SETSEARCHFLAGS, flags);
    ed.Send(SCI_SETTARGETSTART, tStart);
    ed.Send(SCI_SETTARGETEND, tEnd);
    return ed.Send(SCI_SEARCHINTARGET, needle.size(), (LPARAM)needle.c_str());
}

} // namespace

bool FindInEditor(Editor& ed, const FindState& st, FindDirection dir) {
    if (!ed.Valid() || st.text.empty()) return false;
    std::string needle = ToUtf8(st.text);
    if (needle.empty()) return false;

    unsigned int flags = FlagsOf(st);
    sptr_t docLen = ed.Send(SCI_GETLENGTH);
    sptr_t selStart = ed.Send(SCI_GETSELECTIONSTART);
    sptr_t selEnd = ed.Send(SCI_GETSELECTIONEND);
    sptr_t anchor = (dir == FindDirection::Forward) ? selEnd : selStart;

    sptr_t tStart, tEnd;
    bool wrap = false;
    if (dir == FindDirection::Forward) {
        tStart = anchor;
        tEnd = docLen;
        if (tStart >= docLen) { tStart = 0; wrap = true; }
    } else {
        tStart = 0;
        tEnd = anchor;
        if (tEnd <= 0) { tEnd = docLen; wrap = true; }
    }

    auto search = [&]() {
        return SearchRange(ed, needle, flags, tStart, tEnd);
    };

    sptr_t pos = search();
    if (pos < 0 && !wrap) {
        if (dir == FindDirection::Forward) { tStart = 0; }
        else { tEnd = docLen; }
        wrap = true;
        pos = search();
    }
    if (pos < 0) return false;

    sptr_t found = ed.Send(SCI_GETTARGETSTART);
    sptr_t endPos = ed.Send(SCI_GETTARGETEND);
    ed.Send(SCI_SETSELECTION, endPos, found);
    ed.Send(SCI_SCROLLCARET);
    return true;
}

bool ReplaceCurrentInEditor(Editor& ed, const FindState& st) {
    if (!ed.Valid() || st.text.empty()) return false;
    std::string needle = ToUtf8(st.text);
    std::string repl = ToUtf8(st.replace);
    unsigned int flags = FlagsOf(st);

    // If the current selection is exactly a match, replace it in place.
    sptr_t selStart = ed.Send(SCI_GETSELECTIONSTART);
    sptr_t selEnd = ed.Send(SCI_GETSELECTIONEND);
    if (selEnd > selStart) {
        sptr_t pos = SearchRange(ed, needle, flags, selStart, selEnd);
        if (pos == selStart) {
            ed.Send(SCI_SETTARGETSTART, selStart);
            ed.Send(SCI_SETTARGETEND, selEnd);
            ed.Send(SCI_REPLACETARGET, (uptr_t)repl.size(), (LPARAM)repl.c_str());
            ed.Send(SCI_SETSELECTION, selStart + (sptr_t)repl.size(), selStart);
            ed.Send(SCI_SCROLLCARET);
            return true;
        }
    }
    // Otherwise move to the next match.
    return FindInEditor(ed, st, FindDirection::Forward);
}

int ReplaceAllInEditor(Editor& ed, const FindState& st) {
    if (!ed.Valid() || st.text.empty()) return 0;
    std::string needle = ToUtf8(st.text);
    std::string repl = ToUtf8(st.replace);
    if (needle.empty()) return 0;

    unsigned int flags = FlagsOf(st);
    int count = 0;
    sptr_t docLen = ed.Send(SCI_GETLENGTH);
    sptr_t searchPos = 0;

    ed.Send(SCI_BEGINUNDOACTION);
    while (searchPos <= docLen) {
        sptr_t pos = SearchRange(ed, needle, flags, searchPos, docLen);
        if (pos < 0) break;
        ed.Send(SCI_SETTARGETSTART, pos);
        sptr_t matchEnd = ed.Send(SCI_GETTARGETEND);
        ed.Send(SCI_SETTARGETEND, matchEnd);
        sptr_t newLen = ed.Send(SCI_REPLACETARGET,
                                (uptr_t)repl.size(), (LPARAM)repl.c_str());
        ++count;
        sptr_t next = pos + newLen;
        if (next <= searchPos) next = searchPos + 1; // paranoia: always advance
        searchPos = next;
        docLen = ed.Send(SCI_GETLENGTH);
    }
    ed.Send(SCI_ENDUNDOACTION);
    return count;
}

} // namespace xfs
