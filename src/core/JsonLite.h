#pragma once
// xfsWinPad - JsonLite: 极简容错 JSON 解析 + 颜色工具。
// 只服务于设置/主题/样式这类小型人类可读 JSON（非通用库）：
//   * 支持对象/数组/字符串/数/布尔/null，解析到内存值模型
//   * 容错策略：结构损坏时由调用方决定回退（解析失败返回 false）
//   * 颜色统一 "#RRGGBB" 字符串 ↔ COLORREF

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <map>
#include <vector>

namespace xfs {
namespace json {

struct Value {
    enum Kind { Obj, Arr, Str, Num, Bool, Null } kind = Null;
    std::wstring str;
    double num = 0;
    bool b = false;
    std::map<std::wstring, Value> obj;    // Obj
    std::vector<Value> arr;               // Arr
};

// 解析整个文档；成功后 root.kind == Obj（顶层必须是对象）
bool Parse(const std::wstring& text, Value* root);
const Value* Find(const Value& obj, const wchar_t* key);   // 不存在返回 nullptr

// 颜色工具（"#RRGGBB"；非法输入返回 fallback）
COLORREF ParseColor(const std::wstring& s, COLORREF fallback);
std::wstring ColorToWstr(COLORREF c);

} // namespace json
} // namespace xfs
