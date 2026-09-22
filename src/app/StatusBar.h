#pragma once
// xfsWinPad - StatusBar: document state display (Ln/Col/Sel, EOL, encoding, language)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace xfs {

class StatusBar {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    void Layout(int width, int dpi);

    void SetPosition(int line, int col, int selection);
    void SetDocInfo(long long lengthBytes, int lines, int words = -1);
    void SetBinary(bool isBinary);
    void SetEol(const std::wstring& eol);
    void SetEncoding(const std::wstring& enc);
    void SetLanguage(const std::wstring& lang);
    void SetReadOnly(bool ro);
    // 批次 73：[6] 模型来源标注。Chroma 3380 族的语法高亮/签名提示/未来的诊断
    // 都建在我们从语言手册抽取的数据模型上，**不是 CRAFT 编译器的输出**
    // （Chroma 未公开错误码表）。非 Chroma 文件传空串即可。
    void SetModelNote(const std::wstring& note);
    // 批次 87：[7]→[8] 静态检查结果（最右）。Chroma 文件传 "3 错 1 警"/"未发现问题"，
    // 非 Chroma 文件或功能关闭时传空串。
    // 【批次 106 索引改动】新增 [7] 定义提示后，静态检查从 [7] 移到 [8] —— 让它
    // **继续留在最右**：那是用户已经形成习惯的位置（一列扫描到头的"结论列"），
    // 为了加一段而把它挤走是白白的视觉改动。段数组与 SB_SETPARTS 计数同步 +1。
    void SetDiagnostics(const std::wstring& text);
    // 批次 106：[7] 「定义」提示 —— 光标下的符号声明在哪（被引用 .dec 的文件名 +
    // 行号 + 那一行原文）。转到定义（批次 104）是一次**单向**的旅程：F12 之前
    // 没有任何东西告诉用户"脚下这个词是可以跳的"，这个段就是那个提示。
    // 不在符号上 / 非 Chroma 文件 / 大文件时传空串（清空该段）。
    void SetDefinitionHint(const std::wstring& text);
    void SetPart(int index, const std::wstring& text);

private:

    HWND hwnd_ = nullptr;
    // 段序号：[0]位置 [1]文档 [2]二进制/只读 [3]EOL [4]编码 [5]语言
    //        [6]模型来源 [7]定义提示（批次 106） [8]静态检查
    int parts_[9] = {0};
};

} // namespace xfs
