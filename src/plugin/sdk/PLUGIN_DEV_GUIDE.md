# xfsWinPad 插件开发标准（面向 AI 代码生成）

本文档是 xfsWinPad 插件的**唯一权威规范**。按此文档生成的插件保证可编译、
可加载、可通过 xfsWinPad 的 AI 工场（Workshop）自动安装运行。

## 1. 形态与生命周期

- 插件 = 一个 x64 DLL，用 C 或 C++（MSVC，`extern "C"` 导出）。
- 必须导出三个函数：

```c
// 返回静态结构体（const，返回其地址）；abiVersion 必须等于 XFS_PLUGIN_ABI_VERSION
const xfs_plugin_abi* xfsPlugin_getInfo(void);
// 宿主加载时调用；host 指针在插件生命周期内有效，缓存它
void xfsPlugin_register(const xfs_plugin_host* host);
void xfsPlugin_unregister(void);   // 卸载前调用；此后不得再调用任何 host 函数
```

- `xfs_plugin_abi` 字段：`abiVersion` / `name` / `version` / `description` /
  `registerPlugin` / `unregisterPlugin`（见 `xfs_plugin_api.h`，与你的源文件
  同目录）。**不要**自己猜字段，include 该头文件即可。
- 名称规范：`name` 短英文小写连字符（如 `insert-timestamp`）；命令 label
  用人类可读文本。

## 2. host API（全部在 UI 线程回调内调用）

宿主在 `register` 时传入 `xfs_plugin_host*`。常用能力（完整定义见头文件）：

| 能力 | 函数 |
|---|---|
| 注册命令 | `addCommand(label, category, cb, user)` |
| 移除命令 | `removeCommand(cmd)` |
| 活跃文档路径 | `getActiveFile(out, cap)` |
| 文本读写 | `getTextLength` / `getText` / `setTextUndoable` / `insertTextAtCaret` |
| 选区 | `getSelectedText(out, cap)` |
| 光标 | `getCurrentLine/Column` / `gotoLine` |
| 文档管理 | `getDocumentCount/Path` / `openFile` / `saveActive` |
| 事件订阅 | `addEventHook(cb, user, mask)` / `removeEventHook` |
| 日志 | `log("...")`（UTF-8） |
| 配置目录 | `getConfigDir(out, cap)`（每插件持久化配置，自行管理文件） |

**硬规则**：
- 事件/命令回调都在 UI 线程——回调里**不要**做任何耗时操作（>10ms）；
  不要阻塞、不要 Sleep、不要同步网络。需要重活时自己开线程，回 UI 线程
  用 `PostMessage` 到你自己创建的隐藏窗口（`ClassName` 前缀 `xfsP_`）。
- 所有字符串 UTF-8。`char` 缓冲区按 API 说明传 cap，返回值判断截断。
- 不进程退出不要死锁：`unregister` 里停掉自己的线程再返回。

## 3. 命令注册模式（标准骨架）

```c
static const xfs_plugin_host* g_host;
static xfs_plugin_command* g_cmd;

static void OnCommand(void* user) {
    (void)user;
    g_host->insertTextAtCaret("Hello");
}

void xfsPlugin_register(const xfs_plugin_host* host) {
    g_host = host;
    g_cmd = host->addCommand("Insert Hello", "Plugins", OnCommand, NULL);
}
void xfsPlugin_unregister(void) {
    if (g_host && g_cmd) g_host->removeCommand(g_cmd);
    g_cmd = NULL;
    g_host = NULL;
}
```

- `addCommand` 失败返回 NULL——保存指针前判空。
- 一个插件可注册多个命令；每个都记下 handle，unregister 时逐个移除。

## 4. 事件钩子（可选）

事件位：`XFS_EVT_DOC_OPENED/ACTIVATED/SAVED/CLOSED`（utf8Arg=文件路径）、
`XFS_EVT_TEXT_MODIFIED`（约 250ms 合并）、`XFS_EVT_CURSOR_MOVED`
（utf8Arg="line,column" 1-based）、`XFS_EVT_APP_READY`。

```c
static void OnEvent(uint32_t evt, const char* arg, void* user) {
    if (evt == XFS_EVT_DOC_OPENED && arg) g_host->log(arg);
}
// register 里：
host->addEventHook(OnEvent, NULL, XFS_EVT_DOC_OPENED | XFS_EVT_DOC_SAVED);
```

钩子 id 保存下来，unregister 时 `removeEventHook(id)`（幂等）。

## 5. 构建约定（AI 工场固定管线，不要自作主张）

- 项目布局（AI 工场脚手架已建好）：
  ```
  <name>/
    plugin.json     # 元数据 {"name","version","description","entry"}
    src/plugin.c    # 你的全部实现（单文件，保持简单）
    build.cmd       # 固定构建脚本（勿改）
    build/          # 产物 output.dll 落这里
  ```
- **只写 `src/plugin.c`（必要时可加 `src/*.h`）**，不要改 build.cmd、不要
  引入第三方库、不要 manifest、不要 def 文件（`__declspec(dllexport)`
  即可）、不要 C++ 异常/Rtti（按 C 编译最稳；C++ 也可但导出必须 extern "C"）。
- 允许的标准库：CRT（string/stdio/stdlib/math/time…）、windows.h、
  shlwapi.h、shellapi.h。其余一律不引。
- 编译告警即失败标准：尽量零告警（/W3）。
- 不允许：注册表写入、进程注入、网络监听、修改其他插件、修改 xfsWinPad
  自身文件。配置只写 `getConfigDir` 给的目录。

## 6. 错误处理

- 所有 host 调用判返回值（int 0/1 或 NULL）。
- 无活跃文档时文本类调用安全返回 0/false——不要自己崩溃。
- 永远不要 `exit()` / `TerminateThread` / 抛异常穿 DLL 边界。

## 7. 质量清单（生成完自查）

1. 三个导出函数签名与头文件逐字一致？
2. 缓存的 host 指针在 unregister 里清空？
3. 每个 addCommand/addEventHook 的返回值都保存并在 unregister 里对称移除？
4. 回调内无耗时/阻塞操作？
5. 字符串全部 UTF-8、缓冲区带 cap 判断？
6. `plugin.json` 的 name/version/description 与 getInfo 一致？
