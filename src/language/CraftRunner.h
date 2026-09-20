#pragma once
// xfsWinPad - CRAFT 编译命令执行器（批次 95，方向 D 第二刀）
//
// 【这一层要解决什么】
//   CraftProject 回答"该跑哪几条命令"，本模块负责**真的把它们跑起来**并把输出收回来。
//   官方工作流里这一步是 TextPad 的 `[Tools]\make`（操作手册 §2.5.2）。
//
// 【为什么不用 make.exe】
//   现场机器不一定装了 make，但**一定装了 CRAFT**。所以按 CraftProject 解析出的
//   步骤自己顺序执行 —— 少一个外部依赖，也少一层 make 的转义问题。
//
// 【失败即停】
//   与 make 的默认行为一致：**某一步失败就停，后面的不跑**。理由不是"学 make"，
//   而是链接步骤（`patcmp … -f makefile_pdt0.lst`）依赖前面的 `.pdt` 产物 ——
//   编译都没过就链接，只会得到一串"文件不存在"的二次噪声，把真正的错误埋掉。
//
// 【超时与子进程】
//   plncmp 会**自己调 C++ 编译器**（它先生成 `.cpp` 再编成 `.obj`/`.dll`），
//   所以超时强杀时必须把整棵进程树带走，否则会留下孤儿的 cl.exe 占着文件句柄，
//   下一次编译直接失败。做法：**Job Object + KILL_ON_JOB_CLOSE**，进程一关，
//   Job 里所有后代一起被系统杀掉。
//
// 【输出编码】
//   按字节收，不做解码。批次 97/99 拿到真机失败输出之后，这件事**已经有实证**了
//   （不再是"没有样本可验证"）：CRAFT 与它调起来的 cl.exe 都按**系统 ANSI 代码页**
//   输出（中文 Windows 上是 GBK），而且同一份输出里 `\r\n` 与 `\r\r\n` 混用。
//   所以这里依旧原样交给上层 —— 解码与行尾归 CraftProject 的解析器管，它按字节判断，
//   猜错也比猜了之后丢字节强。本层只保证**一个字节都不丢、顺序不变**。
//
// 【零 UI】
//   本模块不碰窗口、不弹框、不写状态栏。它只回答"跑完了没有、退出码多少、
//   输出是什么"。UI 侧（MainWindow）负责在**后台线程**调它、把结果 PostMessage 回来。

#include <windows.h>

#include <string>
#include <vector>

#include "language/CraftProject.h"   // BuildStep

namespace xfs {
namespace craft {

struct StepResult {
    std::wstring exe;             // 实际启动的可执行文件（含路径）
    std::wstring args;
    std::wstring cwd;
    std::wstring label;           // 来自 BuildStep::label
    bool         spawnFailed = false;  // 可执行文件找不到 / 无法启动
    bool         timedOut = false;
    int          exitCode = -1;
    unsigned     elapsedMs = 0;        // 32 位足够：单步超时上限 15 分钟
    std::string  output;          // stdout + stderr 合并（原样字节）
};

struct BuildResult {
    bool                    launched = false;   // 至少启动过一步
    bool                    allOk = false;      // 全部成功
    int                     firstFailedStep = -1;
    std::vector<StepResult> steps;
    std::wstring            note;   // 工具链缺失等情况的说明（空 = 正常）
};

// 单步执行（**阻塞**）。exePath 为空时返回 spawnFailed=true。
// timeoutMs 到点强杀整棵进程树。
StepResult RunStep(const std::wstring& exePath, const BuildStep& step, DWORD timeoutMs);

// 顺序执行整个计划：遇到第一个失败就停。plncmpPath / patcmpPath 由 CraftProject
// 的 DetectToolchain 给出（哪个步骤用哪个工具，按 `step.exe` 的名字分派）。
BuildResult RunPlan(const std::vector<BuildStep>& steps,
                    const std::wstring& plncmpPath,
                    const std::wstring& patcmpPath,
                    DWORD timeoutMsPerStep);

// 单步默认超时：15 分钟。plncmp 要调 C++ 编译器，大工程（几万行向量）不快；
// 但也不能无限等 —— 卡住的编译会让 UI 一直显示"正在编译"。
constexpr DWORD kDefaultStepTimeoutMs = 15 * 60 * 1000;

} // namespace craft
} // namespace xfs
