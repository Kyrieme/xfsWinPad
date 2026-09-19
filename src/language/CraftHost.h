#pragma once
// xfsWinPad - CRAFT 宿主适配层（批次 95，方向 D）
//
// 【这一层是干什么的】
//   CraftProject 是**纯函数**（零 IO）—— 好处是能拿厂商语料离线回归、CI 里不需要
//   真的装 CRAFT；代价是调用方得自己把 makefile 读出来喂进去、自己把环境变量读出来。
//   本层就是那段"读盘 + 读环境"的胶水。
//
// 【为什么单独一层，而不是直接写在 MainWindow 里】
//   因为**探针也要用**。`test_craftrunner <工程文件>` 存在的意义是"手工验证真实
//   工程能跑通"，如果它走的是自己那份读盘代码，那它验证的就不是 UI 的路径了 ——
//   两边会悄悄分叉，结果是"探针全绿、UI 不工作"。
//   这个项目已经栽过一次同类跟头（批次 94：手写的"结构等价样本"没写那个续行空格，
//   单测全绿而真实语料失败）。同一个坑不踩第二次。

#include <string>

#include "language/CraftProject.h"

namespace xfs {
namespace craft {

// 读盘 + 建工程模型。anyPath 可以是工程里任意一个文件（.pln/.dec/.pat 都行）。
// 内部会去 anyPath 所在目录找 makefile，找不到再向上一层找（现场常见结构：
// makefile 在工程根，源码在 PAT\ 子目录里）。
Project LoadProject(const std::wstring& anyPath);

// 从环境探测工具链：设置目录 → CRAFT_HOME\bin → PATH。
// settingsDir 由设置界面提供（可以为空 = 未配置）。
// craftHome 不传则读环境变量 CRAFT_HOME。
Toolchain DetectToolchainFromEnv(const std::wstring& settingsDir,
                                 const std::wstring& craftHomeOverride = std::wstring());

// 用 SearchPathW 实现的 PATH 查找，作为 DetectToolchain 的 findOnPath 回调。
bool FindOnPath(const std::wstring& candidate, std::wstring& full);

// 读环境变量（不存在时返回空串）。探针与 UI 都用它，省得两处各写一遍。
std::wstring ReadEnv(const wchar_t* name);

// 纯路径工具（不碰磁盘）。放在这里是因为宿主侧到处要用，而内核刻意不提供。
std::wstring DirOf(const std::wstring& path);      // 去掉最后一段
std::wstring ParentOf(const std::wstring& dir);    // 向上一层；到根返回空

} // namespace craft
} // namespace xfs
