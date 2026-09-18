// NppCompat.cpp — NPP 形态插件的装载与调用包装。设计依据见
// 插件系统设计笔记 §4/§5（路线图、ABI 事实卡、决策记录）。
//
// 职责划分（故意收窄）：本文件只负责「解析六导出、校验 Unicode、保存名字、
// 薄包装调用」；who-when 调用 setInfo/getFuncsArray、cmdID 回填、命令表生成
// 全部由 PluginManager 编排 —— 宿主数据结构只有一个真相源。
#include "NppCompat.h"
#include "../../core/Log.h"
#include "../../core/Util.h"

#include <cstdio>

namespace xfs {
namespace npp {

namespace {

// SEH 崩溃隔离：真实 Notepad++ 也用 __try/__except 把插件回调包起来，避免
// 插件自身的访问违例一路抛到顶层 UnhandledExceptionFilter -> WER
// (WerpLaunchAeDebug -> WaitForMultipleObjectsEx)，让宿主表现为无限挂起。
// 实测 BetterMultiSelection 1.5 在 beNotified(NPPN_BUFFERACTIVATED) 里
// CreateWindowExW 触发访问违例，Debug 下崩 0xC0000005、Release 下挂死。
// 这里把异常拦下：记日志并置 crashed_，跳过后续通知，宿主继续运行。
//
// 注意：__try/__except 所在函数不能含有需要对象展开的 C++ 对象（C2712），
// 所以隔离块放在独立的无状态 helper 里，而不是有 std::string 的成员函数里。
DWORD SafeCallBeNotified(void (*fn)(void*), void* scn) {
    DWORD ec = 0;
    __try {
        fn(scn);
    } __except ((ec = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
    }
    return ec;
}

DWORD SafeCallSetInfo(void (*fn)(NppData*), NppData* data) {
    DWORD ec = 0;
    __try {
        fn(data);
    } __except ((ec = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
    }
    return ec;
}

void LogCrash(const char* where, const std::string& pluginName, DWORD ec) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "0x%08X", ec);
    Logger::Error(std::string("NppCompat '") + pluginName + "' " + where +
                  " raised " + buf + "; plugin disabled");
}

} // namespace

void NppAdapter::Thunk(void* user) {
    // 统一命令表回调 (cb,user) → 无参插件函数
    auto* slot = static_cast<FuncSlot*>(user);
    if (slot && slot->fn) slot->fn();
}

bool NppAdapter::Resolve(HMODULE dll) {
    dll_ = dll;
    failReason_ = "export";
    setInfo_ = reinterpret_cast<void (*)(NppData*)>(::GetProcAddress(dll, "setInfo"));
    getName_ = reinterpret_cast<const wchar_t* (*)(void)>(::GetProcAddress(dll, "getName"));
    getFuncsArray_ = reinterpret_cast<FuncItem* (*)(int*)>(::GetProcAddress(dll, "getFuncsArray"));
    beNotified_ = reinterpret_cast<void (*)(void*)>(::GetProcAddress(dll, "beNotified"));
    messageProc_ = reinterpret_cast<LRESULT (*)(UINT, WPARAM, LPARAM)>(::GetProcAddress(dll, "messageProc"));
    isUnicode_ = reinterpret_cast<BOOL (*)(void)>(::GetProcAddress(dll, "isUnicode"));

    // 两个阶段各有专属日志，缺失时能直接判断该 DLL 到底是不是 NPP 形态。
    if (!setInfo_ || !getName_ || !getFuncsArray_) {
        Logger::Error("NppCompat: missing setInfo/getName/getFuncsArray "
                      "(neither xfsPlugin_getInfo nor NPP-style; refused)");
        return false;
    }
    if (!beNotified_ || !messageProc_ || !isUnicode_) {
        Logger::Error("NppCompat: has setInfo but missing "
                      "beNotified/messageProc/isUnicode; refused");
        return false;
    }
    if (!isUnicode_()) {
        Logger::Info("NppCompat: ANSI plugin rejected (isUnicode()==FALSE); "
                     "compatibility report will list it as unsupported");
        failReason_ = "ansi";
        return false;
    }

    const wchar_t* nm = getName_();
    if (!nm || !nm[0]) {
        Logger::Error("NppCompat: getName returned empty/NULL; refused");
        return false;
    }
    name_.assign(nm);            // 立即拷贝：指针指向插件静态区，随卸载失效
    failReason_.clear();
    return true;
}

const std::wstring& NppAdapter::Name() const { return name_; }

void NppAdapter::CallSetInfo(const NppData& data) {
    Logger::Debug("CallSetInfo enter: " + WideToUtf8(name_));
    if (setInfo_) {
        DWORD ec = SafeCallSetInfo(setInfo_, const_cast<NppData*>(&data));
        if (ec) {
            crashed_ = true;
            LogCrash("setInfo", WideToUtf8(name_), ec);
        }
    }
    Logger::Debug("CallSetInfo return: " + WideToUtf8(name_));
}

FuncItem* NppAdapter::FetchItems(int& count) {
    int n = 0;
    items_ = getFuncsArray_ ? getFuncsArray_(&n) : nullptr;
    if (!items_ || n <= 0) {
        Logger::Error("NppCompat '" + WideToUtf8(name_) +
                      "': getFuncsArray gave no items");
        itemCount_ = 0;
        count = 0;
        return nullptr;
    }
    itemCount_ = n;
    slots_.clear();
    slots_.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        slots_[static_cast<size_t>(i)] = {items_[i].func};
    count = n;
    return items_;
}

void NppAdapter::Notify(const void* scn) {
    // beNotified 的 C 签名收非 const 指针；我们只是透传宿主构造的通知对象，
    // 插件理论上可改写其字段，这里保持与签名一致的非常量传递。
    // SEH 隔离见文件头的 SafeCallBeNotified：插件在通知里访问违例时，
    // 拦截并记日志跳过，避免上泄到 UnhandledExceptionFilter/WER 死挂。
    if (crashed_ || !beNotified_ || !scn) return;
    DWORD ec = SafeCallBeNotified(beNotified_, const_cast<void*>(scn));
    if (ec) {
        crashed_ = true;
        LogCrash("beNotified", WideToUtf8(name_), ec);
    }
}

} // namespace npp
} // namespace xfs
