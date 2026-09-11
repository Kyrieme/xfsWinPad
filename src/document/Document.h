#pragma once
// xfsWinPad - Document: one open file (editor control + file metadata)

#include "../editor/Editor.h"
#include "../encoding/Encoding.h"
#include <filesystem>
#include <string>

namespace xfs {

class Document {
public:
    Editor editor;

    std::filesystem::path path;          // empty => untitled
    encoding::EncodingType encoding = encoding::EncodingType::UTF8;
    bool readOnly = false;

    // Index into LanguageMenuCatalog(); -1 = auto-detect from the file name.
    int langIndex = -1;

    // Which split view owns this document. 0 = left/primary view,
    // 1 = right/other view (move-to-other-view).
    int view = 0;

    // 锁定标签：阻止关闭（Close/CloseAll/批量关闭全部跳过）+ 编辑器只读。
    // TabBar 绘制小锁标记替代关闭 ✕。
    bool locked = false;

    bool HasPath() const { return !path.empty(); }
    std::wstring DisplayName() const;
    std::wstring TitleForTab() const;    // includes dirty marker

    void SetUntitledName(const std::wstring& n) { untitledName_ = n; }

private:
    std::wstring untitledName_;
};

} // namespace xfs
