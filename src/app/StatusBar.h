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
    // 批次 87：[7] 静态检查结果（最右）。Chroma 文件传 "3 错 1 警"/"未发现问题"，
    // 非 Chroma 文件或功能关闭时传空串。
    void SetDiagnostics(const std::wstring& text);
    void SetPart(int index, const std::wstring& text);

private:

    HWND hwnd_ = nullptr;
    // 段序号：[0]位置 [1]文档 [2]二进制/只读 [3]EOL [4]编码 [5]语言
    //        [6]模型来源 [7]静态检查
    int parts_[8] = {0};
};

} // namespace xfs
