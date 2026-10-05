#pragma once
// xfsWinPad - Document: one open file (editor control + file metadata)

#include "../editor/Editor.h"
#include "../encoding/Encoding.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

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

    // 批次 89：被引用 .dec 的符号缓存（pin / 组 / 时序名），跨文件补全的词源。
    // 由宿主 MainWindow 在打开/切换/编辑防抖时刷新（与静态检查开关无关）；
    // Workspace 的词汇补全 provider 只读。dec 缺席或非 Chroma 文件时为空。
    std::vector<std::string> decSymbols;

    // 批次 146：跨文件补全第二段 —— 被引用 .pat 的 label 缓存（.pln 的 JUDGE_PAT
    // 实参词源）。与 decSymbols 同一批刷新（同一个 RefreshDecSymbols）、同样由宿主
    // 写、Workspace 的词汇补全 provider 只读。词源两条路（.rpt 优先 / .pat 兜底）
    // 的取证与取舍见 Chroma3380Diagnostics.h 的 PatFileRef 一节。
    std::vector<std::string> patLabels;

    // 批次 147：跨文件补全第三段 —— `module:label` 的**位置感知**词源。
    // key = SPM/APM/RPM_PATTERN 模块名（源码原样），value = 该块内的 label（保序去重）。
    // 与上面两批同一轮刷新（同一个 RefreshDecSymbols）、宿主写、Workspace 只读。
    // 词源取证见 Chroma3380Diagnostics.h 的 ExtractPatModules 一节。
    std::map<std::string, std::vector<std::string>> patModuleLabels;

    bool HasPath() const { return !path.empty(); }
    std::wstring DisplayName() const;
    std::wstring TitleForTab() const;    // includes dirty marker

    void SetUntitledName(const std::wstring& n) { untitledName_ = n; }

private:
    std::wstring untitledName_;
};

} // namespace xfs
