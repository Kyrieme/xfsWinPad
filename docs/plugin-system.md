# xfsWinPad 插件系统设计文档

> **文档目的**：本文件是插件子系统的唯一权威设计记录。任何开发者（人或模型）接手前
> 必读；每次改动插件子系统后必须回写"实施状态"一节并随代码提交。
> 配套阅读：`ARCHITECTURE.md`（模块地图）、`TODO.md`（任务队列）。

---

## 0. 快速接手指引

| 你想继续的工作 | 直接看哪节 | 主要文件 |
|---|---|---|
| 加新的宿主回调给原生插件 | §2.3 ABI 扩展规则 | `src/plugin/xfs_plugin_api.h` |
| 继续 NPP 兼容层（4b/4c/4d） | §4 路线图 + §5 现状 | `src/plugin/npp/*` |
| 插件管理框（四标签/搜索/安装） | §5.9 | `src/plugin/PluginAdminDialog.cpp` |
| 排查插件加载失败 | §6 调试 | `%LOCALAPPDATA%\xfsWinPad\logs\xfsWinPad.log` |

---

## 1. 总体架构（两层插件形态）

```
                    ┌────────────────────────────┐
   插件 DLL 目录     │      PluginManager          │
 %APPDATA%\xfsWinPad │                             │
     plugins\       │  LoadOne():                 │
        │           │  ├─ 有 xfsPlugin_getInfo    │──► 原生通道 (ABI v3)
        ├── *.dll ──┤  │    (我们自己的 C ABI)      │    xfs_plugin_host 函数指针表
        │           │  └─ 否则探测 setInfo 等     │──► NPP 兼容通道 (适配器)
        │           │     6 个导出函数             │    FuncItem→PluginCommand 包装
                    │                             │
                    │  统一出口：                   │
                    │   commands_[] 命令表(id≥10000)│→ Plugins 菜单 / 命令面板
                    │   hooks_[]   事件钩子         │← Workspace 槽位 + 节流定时器
                    │   Raise()    事件派发         │
                    └────────────────────────────┘
```

**核心不变式**
1. 插件命令 id 由宿主分配，区间 `[10000,10500]`（`core/CommandIds.h` 内建命令不重叠）。
2. 所有宿主回调只在 UI 线程发生；事件文本一律 UTF-8（有界拷贝见 §2.3 规则③）。
3. 编辑器访问 = 对活动文档的 Scintilla 子窗口直接发原生 `SCI_*` 消息。
   **不链接 Editor.cpp**（单元测试因此无需真实控件，只补 `tests/stub_workspace.cpp` 符号桩）。

---

## 2. 原生插件 API（xfs_plugin_abi）

### 2.1 演进史
| 版本 | 能力 | 关键提交 |
|---|---|---|
| v1 | 仅 addCommand/log（骨架，getActiveFile 是空串桩） | ac36c68 |
| v2 | 编辑器/文档访问回调；**修复 g_active 生命周期缺陷** | d88cba7 |
| v3 | 事件钩子系统 + getConfigDir | 5f413ce |

### 2.2 宿主回调面（v3 全量）
- 命令注册：`addCommand(label,category,cb,user)` / `removeCommand`
- 文档：`getDocumentCount` `getActiveDocumentIndex` `getDocumentPath(i)` `openFile`(去重到已开标签)
  `saveActive`
- 文本（全部有界 UTF-8）：`getTextLength` `getText(cap)` `getSelectedText(cap)`
  `setTextUndoable`（TARGETWHOLEDOCUMENT 包 BEGIN/ENDUNDOACTION=单步撤销）
  `insertTextAtCaret`
- 光标/状态：`getCurrentLine/Column`（1-based）`gotoLine(1based)` `isModified` `isReadOnly`
- 事件钩子：`addEventHook(cb,user,eventMask)` 返回 hookId≥1，`removeEventHook(id)`
- 其他：`getActiveFile(out,cap)`、`log(msg)`、`getConfigDir`
  （=`%APPDATA%\xfsWinPad\plugins\config`，即建即用）

### 2.3 三条硬性规则（违反即埋雷）
① **版本严格匹配**：`abi->abiVersion != XFS_PLUGIN_ABI_VERSION` 直接拒绝加载。
   host 结构体只能尾部追加指针；改字段顺序/删字段必须再 bump 版本。
② **g_active 生命周期**：`PluginManager.cpp` 的匿名命名空间持有 `g_active`，
   在 LoadAllFrom 安装后**保持有效直到 UnloadAll 清除**。
   历史 bug：v1/v2 曾在 DLL 注册完就清空它 → 命令执行期所有桥接拿不到宿主
   （这正是旧版 getActiveFile 永远返回 "" 的根因）。不要回退这一点。
③ **有界字符串协议**：`(char* out, uint32_t cap)` 模式 = 最多拷 cap-1 字节 +
   强制 NUL 收尾；返回 true 表示完整放下、false=截断或无文档/参数错。
   新增任何返回路径的 API 必须遵守此协议（对应 Win32 习惯，调用方两段式扩容）。

### 2.4 事件系统语义
- 位定义：APP_READY/DOC_OPENED/DOC_ACTIVATED/DOC_SAVED/DOC_CLOSED/
  TEXT_MODIFIED/CURSOR_MOVED/CURSOR_MOVED 参 `xfs_plugin_api.h` 头注释；
  单次派发 `event` 只带一个位，`utf8Arg` 按每一位的文档约定携带
  路径（""=未命名）或 `"line,column"` 或 NULL。
- 派发源：
  - 文档生命周期 ← `Workspace::onDocumentOpened/onDocumentActivated/onDocumentSaved`
    （MainWindow::Init 中以 lambda 接线到 Raise）
  - TEXT/CURSOR ← WM_NOTIFY 里 `SCN_MODIFIED`（真编辑）/`SCN_UPDATEUI`（光标/滚动）
    置 pending 位后由 250ms 定时器(kPluginEvtTimerId=2)统一冲刷 —— 高频合并，
    保证不在编辑热路径上同步派发。
- **Raise 用快照迭代**：钩子可在自己被调用的同时注销自己或其它钩子，安全。
- 无工作区时（单元测试环境）所有回调降级为 0/-1/false 不崩溃——
  该性质被 test_plugin 以 15 位"无文档位图"精确断言，破坏它会挂测试。

---

## 3. 法律红线（对整个 §4 有效）
依据需求文档 §二十八 与 PROJECT.md 版权原则：
- ❌ 不复制 Notepad++ 源码/头文件/资源；不做反编译。
- ✅ 只从公开文档研究其**接口契约**（调用序列、消息号、结构布局这些是事实而非表达），
  自行原创书写我们的声明与实现；每个对外声明注明公开来源要点。
- ✅ 无法兼容的插件输出 Compatibility Report（4b 起，落在 log + 将来的报告页）。

---

## 4. NPP 兼容层路线图（当前主线）

### 结论先行
NPP 插件本质是普通 DLL：仅导出六个 C 入口，用 SendMessage 与宿主通信，
不导入 notepad++.exe 的符号 → 二进制装载对我们零障碍；难度全在**语义映射**。

### 分阶段
| 阶段 | 内容 | 状态 |
|---|---|---|
| **4a** | 双分支加载器：探测 setInfo 系列 → FuncItem→原生命令包装；beNotified/messageProc 先存不用 | ✅ 2026-08-27 完成（见 §5.2/§5.3） |
| 4b | NPPM_\* 消息垫片：GETCURRENTSCINTILLA→活动文档 HWND 动态解析、DOOPEN/RELOADFILE/SAVEALLFILES/buffer-id 族。入口提示：插件 messageProc_ 已保存在 adapter，可在 PluginManager 层加一个"把窗口消息转给各 NPP 插件"的转发函数供 WndProc 调用 | ✅ 2026-08-27 完成（见 §5.6：常量已联机核实 + 单测全绿） |
| 4c | beNotified 桥：SCNotification 转发 + 在 §2.4 事件源上合成 NPPN_READY/FILOPEN/…/SHUTDOWN | ✅ 2026-08-27 完成（见 §5.7） |
| 4d | 可停靠对话框管理器(tTbData 类似物)：把插件 HWND 宿主进 dock 体系 → 解锁 NppExec 类 | ✅ 2026-08-27 完成（见 §5.8：含真实插件对话框端到端测试，ctest 8/8 全绿） |
| 之后 | Marketplace/.xfpplugin 打包、权限/沙箱分级、Compatibility Report UI | 远期 |

### 兼容预期（用户可见口径）
第 1 批可运行 ≈ 无 UI 面板的纯文本增强类；4d 后才能谈 NppExec 这类控制台型重型插件;
Compare 类依赖双视图同步模型的短期内明确不支持(报告里说明原因)。

---

## 5. 实施状态（§4 详设 + 已落代码）

> ⚠️ 接手者：完成任何子项后更新此节勾选状态并随 commit 提交。

### 5.1 NPP 侧 ABI 事实卡（4a 需要，其余供 4b/4c 参考）

调用序列（公开文档结论）：
```
LoadLibrary → GetProcAddress 六导出：
  setInfo(NppData*)              必需
  getName() -> wchar_t*          必需（Unicode 版；isUnicode()==TRUE 才收）
  getFuncsArray(int* nbF)        必需 → FuncItem[nb]，宿主随后逐项回填 _cmdID
  beNotified(SCNotification*)    必需（4a 存指针不调用）
  messageProc(UINT,WPARAM,LPARAM)必需（同上）
  isUnicode() -> BOOL            必须 TRUE，否则拒绝（N++ 自 4.6 同样强制）
执行期：菜单项点击 → WM_COMMAND(cmdID) 到宿主 → 查表回调 _pFunc()
```

结构布局（字段名自定，偏移必须一致——这是二进制契约）：
```cpp
struct XfNppData { HWND nppHandle, scintillaMain, scintillaSecond; }; // 3×HWND
struct XfShortcutKey { unsigned char key; bool ctrl, alt, shift; };
struct XfFuncItem {
    wchar_t* name; void (*func)(void);
    int cmdID;                 // ★ register 后由宿主写回，之后不再变
    bool initCheck; XfShortcutKey* shortcut;   // shortcut 可空；登记暂缓(4b)
};
// 依赖链注意：全 DLL 进程级单例是常态（setInfo 把句柄抄进自己的静态区）
```

4a 时传什么句柄？
- nppHandle := 主窗口 HWND（消息路由起点，虽 4a 还不处理发来的 NPPM_*）
- scintillaMain := 注册时刻活动文档的 Sci 子窗口 HWND；scintillaSecond := NULL。
  ⚠️ 已知语义差：NPP 两视图是常驻双控件，我们是每文档一个 + 单视图。
  结果=缓存了主句柄的插件在切标签后会打到旧控件 —— 正解在 4b 的
  GETCURRENTSCINTILLA 动态解析 + (远期)把第二个等价物做成真分屏。

### 5.2 已落地代码

| 文件 | 职责 |
|---|---|
| `src/plugin/npp/NppCompat.h/.cpp` | 契约结构原创声明 + `NppAdapter`：Resolve(六导出解析/isUnicode 校验/名字拷贝)、CallSetInfo、FetchItems(getFuncsArray+槽位映射)、Thunk(无参回调→(cb,user) 桥) |
| `PluginManager.h/.cpp` | `LoadOne` 双分支探测分流 → `LoadNative` / `LoadNppStyle`；`Loaded.kind`+`unique_ptr<npp::NppAdapter>`；`SetHostWindow(hwnd_)`(MainWindow 注入)；`CommandIdOfHandle`(cmdID 回填查询)；`CommandAt(handle)` |
| `PluginCommand 扩展` | `grouped`(NPP 形态→菜单子分组)、`hasKey/fVirt/vk`(NPP ShortcutKey 转录，FillShortcut 纯转录无协议依赖) |
| `MainWindow.cpp` | `RebuildPluginMenu`=按 category 聚类子菜单(N++ 视觉惯例)+`pluginPopups_` 生命周期防泄漏；`BuildAccelerators()` 可重入(内部销毁旧表)，插件加载后再次调用并入命令快捷键，与内建冲突时让位并记日志 |
| `tests/npp_dll/test_npp_core.cpp` | 六导出最小合规 NPP 插件，**自带镜像契约结构**（不 include 宿主头——正如真实第三方作者），两个 FuncItem 带 Ctrl+Shift+7/8 快捷键，导出观察口：fires/setInfoSeen/cmdId0/cmdId1 等 |

**调用序列实现位置速查**
```
LoadOne           : GetProcAddress("xfsPlugin_getInfo") 缺位 ⇒ LoadNppStyle
LoadNppStyle      : Resolve → CallSetInfo({hostWnd_, 活动Sci, NULL})
                    → FetchItems → for-each: HostAddCommand("名::项",
                      category=名, Thunk, &slots[i]) → items[i].cmdID =
                      CommandIdOfHandle(handle)   ← 公开契约回填点
UnloadAll         : native unregister；NPP 无卸载协议 → FreeLibrary 后 npp.reset()
                    （顺序安全依据：FuncSlot 随 adapter 死亡，命令 impl 析构不 Dereference user）
```

**验证记录（2026-08-27）**
- 单元测试（混合目录 loaded==2、CommandCount==3、label/category 断言、
  两 NPP 命令经 mgr.Execute 分别 +1/+10、回填 cmdID 与宿主 id 相等、
  setInfoSeen==1）：`ALL PLUGIN TESTS PASSED`。
- 真实 GUI：test_npp_core.dll 放入 %APPDATA%\xfsWinPad\plugins\ 启动后日志
  `loaded native ...` + `loaded NPP-plugin 'npp-test' with 2
  FuncItem(s)`；Plugins 菜单出现 npp-test:: 两项（验证后测试 dll 已移除）。
  ctest 全绿。
- 插件崩溃隔离（2026-09-01）：NppAdapter 的 setInfo/beNotified 用
  __try/__except 包裹（SafeCallSetInfo/SafeCallBeNotified），插件访问违例
  在宿主内拦截并置 crashed_ 停发通知——否则异常上抛到 UnhandledExceptionFilter
  → WER(WerpLaunchAeDebug) 死挂（实测 BetterMultiSelection 1.5 在
  beNotified(BUFFERACTIVATED) 抛 0xC0000005，Debug 崩 / Release 挂）。
- 会话中热载（2026-09-01）：PluginManager::LoadNew 扫描插件目录跳过已加载
  DLL，onChanged_ 回调触发 MainWindow 实时重建插件菜单（插件管理器安装后
  无需重启即可见新命令）。

### 5.3 后续接手要点（4b 开工前的坑位提示）
- NPPM_\* 处理器的宿主侧挂点建议：消息直接发到 hostWnd_（主框架），在
  MainWindow::WndProc 的 WM_MESSAGE 分支前加一个
  `if (IsFromNppPlugin(msg)) { plugins_->ForwardNppMessage(...); return 0; }`
  形态的拦截层；GETCURRENTSCINTILLA 必须**每次动态取**活动文档 HWND（§5.1 已知差）。
- buffer-id 概念映射建议：以 DocumentAt(index) 为 buffer 空间，
  `NPPM_GETBUFFERIDFROMPOS`=返回 index+1 之类的稳定句柄表需新建
  （考虑放 PluginManager 或独立 BufferTable）。
- 快捷键（ShortcutKey\*）在 4a 中保存未接线：接手时在 FuncItem 里还有引用，
  做 4b 加速表注入时一并消费；勿提前释放 items_ 内存（指向插件静态区）。

### 5.6 4b：NPPM_* 消息垫片（✅ 2026-08-27 完成）

**协议数值核实（红线：禁 Fake API）**：消息常量在动手前已联机核对权威事实源
`notepad-plus-plus/notepad-plus` 的
`PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h`
（本会话经 gitee 镜像 `mirrors/notepad-plus-plus @ master` 拉取原文比对；
raw.githubusercontent 直连当时不可达）。数值与语义逐条比对，全部一致后写入
`src/plugin/npp/NppMessages.h`（原创注释，标注来源 URL 与抓取日期；不拷贝
GPL 头文件进仓库，只抄接口数值——见 §3 法律边界）。核实结果：
- 基值 `NPPMSG = WM_USER + 1000`；`RUNCOMMAND_USER = WM_USER + 3000`。
- NPPMSG 族：GETCURRENTSCINTILLA=+4、GETNBOPENFILES=+7、GETOPENFILENAMES_DEPRECATED=+8、
  GETOPENFILENAMESPRIMARY_DEPRECATED=+17、GETOPENFILENAMESSECOND_DEPRECATED=+18、
  GETMENUHANDLE=+25、RELOADFILE=+36、SWITCHTOFILE=+37、SAVECURRENTFILE=+38、SAVEALLFILES=+39、
  SETMENUITEMCHECK=+40、GETPLUGINSCONFIGDIR=+46、GETMENUBAR=+52、GETPOSFROMBUFFERID=+57、
  GETFULLPATHFROMBUFFERID=+58、GETBUFFERIDFROMPOS=+59、GETCURRENTBUFFERID=+60、
  RELOADBUFFERID=+61、GETSHORTCUTBYCMDID=+76、DOOPEN=+77、ALLOCATECMDID=+81。
- RUNCOMMAND 族偏移：FULL_CURRENT_PATH=1、CURRENT_DIRECTORY=2、FILE_NAME=3、
  NAME_PART=4、EXT_PART=5、CURRENT_WORD=6。

**已实现子集与语义要点**（`PluginManager::ForwardNppMessage`）：
- 入口：MainWindow::Handle 的 switch 前拦截转发，`ForwardNppMessage` 内部做
  区间检查——两个基值区间（NPPMSG+128 / RUNCOMMAND+64）内 handled=true，
  区间外 handled=false 放行默认流程，绝不吞未知消息。
- 文档集抽象 `NppDocSource`（§5.6 测试方案）：生产绑定 `WorkspaceSink`，
  单测注入 FakeDocs，完全绕开真实控件。
- GETCURRENTSCINTILLA：单视图模型恒返回 0（Main View）——解决 §5.1 已知的
  "缓存旧 Sci 句柄"问题（插件每次 SendMessage 现取，不再依赖 setInfo 快照）。
- GETNBOPENFILES：SECOND_VIEW 恒 0；PRIMARY/ALL 返回文档数。
- GETOPENFILENAMES{_PRIMARY,_SECOND}_DEPRECATED：主视图路径表拷贝，副视图恒空。
- SWITCHTOFILE：已打开则 Activate（精确路径匹配），否则 FALSE。
- DOOPEN：OpenPath 语义（已打开→激活；否则新建）。
- SAVECURRENTFILE / SAVEALLFILES：转发 Workspace::Save/SaveAll；无保存动作
  时 SAVEALLFILES 返回 FALSE（官方契约：没有任何文件需要保存返回 FALSE）。
- RELOADBUFFERID / RELOADFILE：`Workspace::ReloadDocument(index, alert)`（新增），
  alert 语义=脏文档先弹确认，false 则静默丢弃未保存修改并记日志。
- buffer-id 族：以 Document\* 为 bufferID 空间（GETCURRENTBUFFERID 返回
  活动文档指针；GETBUFFERIDFROMPOS=index→指针；GETPOSFROMBUFFERID=高 2 位
  VIEW+低 30 位 INDEX 编码，单视图恒 MAIN_VIEW=0，无效返回 -1；
  GETFULLPATHFROMBUFFERID=两段式，无效 -1）。
- GETPLUGINSCONFIGDIR + RUNCOMMAND 字符串族（GETFULLCURRENTPATH/
  GETCURRENTDIRECTORY/GETFILENAME/GETNAMEPART/GETEXTPART/GETCURRENTWORD）：
  两段式（首调 lp==NULL 返回所需 wchar_t 数；二调按返回值+1 分配传入，成功
  TRUE/缓冲不足 FALSE）。EXTPART 语义不含点。
- ALLOCATECMDID：独立动态池从 10501 起（静态插件池 [10000,10500] 之后、
  WORD 菜单 id 安全区内，池上限 16000），超池返回 FALSE。
- 菜单/命令句柄族：GETMENUHANDLE 按 wParam 返回 Plugins 子菜单（0）或主菜单栏
  （1）句柄，未知取值拒答；GETMENUBAR 返回主菜单栏。句柄由 MainWindow 在
  BuildMenus() 之后经 `SetPluginMenus` 注入（NppExec 拿主菜单栏句柄后自行
  ModifyMenu/CheckMenuItem，MF_BYCOMMAND 递归命中 Plugins 子菜单里的命令项，
  分隔符改写即此路径）。SETMENUITEMCHECK 按命令 id 勾选/取消勾选：插件命令
  （≥PluginCmdFirst）落插件菜单，内建命令落主菜单栏。GETSHORTCUTBYCMDID 对
  已注册插件命令回填快捷键（`ShortcutKey{ctrl,alt,shift,key}` 布局），无快捷键/
  未知命令/空指针返回 FALSE。
- 未列出的区间内编号：明确拒答（返回 0）且 handled=true，不吞不猜。

**已验证**：`tests/test_npp_msgs.cpp`（ctest `npp_msgs`）——假文档集双文档，
覆盖无源降级、区间外放行、视图/计数、GETOPENFILENAMES 缓冲契约、buffer-id
族编码、GETFULLPATHFROMBUFFERID 两段式、RUNCOMMAND 字符串族两段式、
SWITCH/DOOPEN/SAVE 族、ALLOCATECMDID 池序、RELOAD 注入源无后端→FALSE、
菜单/命令句柄族（GETMENUHANDLE/GETMENUBAR 句柄按取值、SETMENUITEMCHECK
真实菜单勾选态、GETSHORTCUTBYCMDID 无快捷键/未知/空指针→FALSE）。
全量 `ctest -C Release` 8/8 绿（含 npp_msgs）。

**过程中的坑**：GETFULLPATHFROMBUFFERID 用 wcsncpy_s 声明尺寸曾硬编码 260，
Debug CRT 按声明尺寸校验写并破坏堆（0xC0000409）——改为按真实路径长度+1
声明；GETOPENFILENAMES 测试若给空指针数组同样触发 CRT 崩溃，测试须提供
真缓冲。详见 TODO.md Lessons Learned。

### 5.7 4c：beNotified 桥（✅ 2026-08-27 完成）

**通知码**：`NppNotify` 枚举定义于 `NppMessages.h`（kNppnBase=1000 对应上游
NPPN_FIRST，32 个通知码逐条标注语义；未合成的仍注明"未合成"）。

**转发链**（三层，各层职责收窄）：
```
MainWindow 事件源 ──► PluginManager::EmitNppNotification(code, idFrom)
                            │ 构造 SCNotification：hwndFrom=主窗口、idFrom=BufferID
                            ▼
                    PluginManager::BroadcastNppNotification(scn)
                            │ 只发 NPP 形态插件（kLoadedNppCompat），快照迭代
                            ▼
                    NppAdapter::Notify(scn) ──► beNotified_(scn)  （六导出之一）
```
- `SCNotification` 布局直接取 `third_party/scintilla/include/Scintilla.h` 的
  原始定义（nmhdr 兼容头打头）——与 NPP 插件预期的二进制布局一致。
- `BroadcastNppNotification` 只发给 NPP 形态插件；原生 ABI 插件无 beNotified
  概念。快照语义：广播期间不增删 `loaded_`，插件卸载不在调用路径内。
- `EmitNppNotification` 是便捷合成器：除 code/idFrom/hwndFrom 外其余字段清零。
  nullptr 安全；无 NPP 插件时为空操作。

**事件合成映射**（§2.4 宿主事件源 → NPPN_*；BufferID=4b 的 Document* 空间）：
| 宿主事件 | 通知 | idFrom | 位置 |
|---|---|---|---|
| 插件加载完成 | NPPN_READY | 0 | MainWindow::Init（LoadAll 之后） |
| onDocumentOpened | NPPN_FILEOPENED | Document* | MainWindow::Init lambda |
| onDocumentActivated | NPPN_BUFFERACTIVATED | Document* | 同上 |
| onDocumentSaved | NPPN_FILESAVED | Document* | 同上 |
| onDocumentClosed | NPPN_FILECLOSED | Document*（悬空仅作 ID） | 同上 |
| WM_CLOSE | NPPN_SHUTDOWN | 0 | MainWindow::WndProc（文件关闭前） |

> ⚠️ onDocumentClosed 在 Workspace::RemoveAt 之后触发，`d` 已悬空：只把它当
> 不透明 BufferID 传，绝不 dereference（BufferID 复用语义与 NPP 的 bufferID
> 回收一致）。

**已验证**：`tests/test_plugin.cpp` —— test_npp_core.dll 新增 beNotified 观察口
（镜像头三成员 NotifyHdr{hwndFrom,idFrom,code}，导出 count/code/id/hwnd）。
断言：EmitNppNotification(READY,0)+FILEOPENED(0x1234) 后插件恰好收到 2 条、
code 分别为 1001/1004、idFrom 透传、无宿主窗口时 hwndFrom 为空（降级）、
BroadcastNppNotification(nullptr) 不崩不计数。全量 `ctest -C Release` 7/7 绿。

### 5.8 4d：可停靠对话框管理器（✅ 2026-08-27 完成）

**目标**：让 NPP 插件用 `NPPM_DMMREGASDCKDLG` 注册的 modeless 对话框变成 xfsWinPad
底部停靠面板——像 LogPanel/TerminalPanel 一样参与布局、可关闭/显示/隐藏。

**二进制契约**（`src/plugin/npp/NppDocking.h`，数值抄录自官方 Docking.h /
dockingResource.h，实现原创）：
- `DockedWidgetData`（tTbData）：字段顺序/宽度即 ABI。`hClient`（插件对话框
  HWND，必填）、`pszName`、`dlgID`（打开它的 FuncItem 命令 id）、`uMask`、
  `hIconTab`、`pszAddInfo`、`rcFloat`、`iPrevCont`、`pszModuleName`（2025 新增，
  重启恢复 dock 位置用，一并保留）。`uMask` 低 4 位为 DWS_* 特性位、高 4 位
  左移 28 为默认容器；`DWS_DF_FLOATING`=0x80000000。
- `DMN_*`（0x41A 起，kDmnFirst=1050）：宿主以 `WM_NOTIFY{idFrom=dlgID,
  lParam=&NMHDR{code=DMN_*}}` 发给插件对话框。`DMN_CLOSE` 插件可 veto（返回
  TRUE 保持打开）；`DMN_DOCK` 注册成功；`DMN_SWITCHIN` 显示。
- `DMM_*`（0x5000 起）：宿主直接 `SendMessage` 给插件对话框的动作请求；
  `DMM_CLOSE`=0x5001（宿主退出）、`DMM_UPDATEDISPINFO`=0x5007。

**转发链**（NPPM_DMM* → DockHost 抽象 → DockManager）：
```
插件 → SendMessage(NPPM_DMM*, ...)
        │ PluginManager::ForwardNppMessage 只做参数解码与转发
        ▼
DockHost 抽象（PluginManager.h，纯虚）：
  DockWidget/Show/Hide/UpdateDisplayInfo/ShowByName/FindHwndByName
        │ MainWindow::Init 注入 dockMgr_（DockManager）
        ▼
DockManager（src/plugin/DockManager.cpp）：
  为插件 hClient 包一层 wrapper（标题条+关闭钮），重挂 hClient，
  参与 MainWindow::LayoutChildren 底部 dock 链（排在 Terminal 之后）
```
- 无宿主（单测）时 NPPM_DMM* 明确拒答：DMMREGASDCKDLG/SHOW/HIDE 等返回
  FALSE，GETPLUGINHWNDBYNAME 返回 0，且 handled=true（不吞默认流程）。
- `DMMGETPLUGINHWNDBYNAME` 的 wParam=窗口名、lParam=模块名；窗口名为空时
  按模块名取首个匹配面板（官方口径）。
- 关闭协商：wrapper 关闭钮 → `DMN_CLOSE` → 插件返回 TRUE 则保持打开（veto），
  否则 `SetVisible(false)`（只隐藏不销毁，插件可随时再 Show）。
  ⚠ 对话框管理器不回传 DlgProc 的返回值：`SendMessage(WM_NOTIFY)` 的结果取
  `DWLP_MSGRESULT`（缺省 0）——要 veto 的插件必须显式
  `SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE)`（真实 NPP 插件的标准做法，
  端到端测试已实证：裸 `return TRUE` 会被吞成 0）。
- 宿主退出：`DockManager::Destroy()` 先对每个面板发 `DMM_CLOSE`，再销毁 wrapper
  （MainWindow WM_DESTROY 中先于 UnloadAll 执行）。
- 布局：`TotalHeight(dpi)` 累加可见面板高度（默认 220 逻辑像素，MulDiv 折算），
  `Layout(x,y,w,dpi)` 自 y 起逐块布置；`hostH` 已扣除 dockH，避免覆盖编辑区。
- 设计取舍：只实现底部容器；`DWS_DF_FLOATING`/左/右/上按底部处理并记日志
  （兼容报告口径，不伪造浮动 UI）。`DWS_ICONTAB`/`DWS_ICONBAR` 未启用。

**已验证**：两层。
- 单测层 `tests/test_npp_msgs.cpp`：FakeDockHost + DMM 段 —— 常量/结构布局
  断言（DMN_*=1051..1056、DMM_*=0x5001..、kDwsDfFloating、DockedWidgetData
  各关键字段 offsetof）；无宿主拒答；注册/显示/隐藏/刷新/按名切换/按名查句柄
  的参数透传与失败路径。
- 端到端层 `tests/npp_dock_dll/test_npp_dock.dll` + `tests/test_dock.cpp`
  （ctest `dock`）：真实 NPP 形态插件用 `.rc` 资源 + `CreateDialogParamW` 建
  modeless 对话框，向真实宿主窗口 `SendMessage(NPPM_DMMREGASDCKDLG)` 注册 →
  真实 DockManager 建 wrapper、重挂 hClient、发 DMN_DOCK/DMN_SWITCHIN；
  断言父窗口类名、IsWindowVisible、TotalHeight/Layout 布局坐标、
  FindHwndByName；关闭协商（无 veto 隐藏 / 置 DWLP_MSGRESULT 后 veto 保持
  打开）；宿主退出 DMM_CLOSE 让插件自毁对话框；同一 DLL 命令二次注册得到
  全新面板（资源无泄漏）。全量 `ctest -C Release` 8/8 绿。

### 5.9 插件管理（Plugins Admin，✅ 2026-08-28 完成）

**目标**：仿照 Notepad++ Plugin Admin 的四标签结构，让用户在 `Plugins → 插件管理…`
中浏览可用插件、实时搜索、勾选安装/更新、查看已安装并移除、预留不兼容页。

**与 Notepad++ 对齐的契约**（只取公开事实，实现原创）：
- 清单布局沿用 nppPluginList 的顶层对象
  `{ "name","version","arch", "npp-plugins":[ {plugin…} ] }`，每个插件条目取
  N++ 的 `folder-name`/`display-name`/`version`/`repository`/`description`/
  `author`/`homepage` 字段（[PluginCatalog.h](file:///d:/AI_Work/codex/xfsPad/src/plugin/PluginCatalog.h)）。
  `folder-name` 是安装目录名，兼作"已安装"匹配键——与 N++ `plugins\<folder>\*.dll`
  惯例一致。
- 已安装判定与 N++ 一致：`plugins` 目录真实存在的 DLL（根目录 `*.dll` 以 base 名记，
  一级子目录内有 DLL 则以子目录名记）。
- 四标签语义：
  - **可用(Available)**：清单中有而尚未安装 → 勾选 +「安装」。
  - **更新(Updates)**：已安装且清单版本更高 → 显示 `旧版本 → 新版本`，勾选 +「更新」。
  - **已安装(Installed)**：当前已安装，选中 +「移除」。
  - **不兼容(Incompatible)**：预留页，暂不启用，仅占位（按钮禁用、列表隐藏，
    状态栏提示将列入因宿主版本/位数不匹配而无法加载的插件）。
- 实时搜索：输入即时在当前页定位（选中 + 滚动到）首个名称/目录前缀匹配项，
  清空恢复全量列表。

**下载 / 安装 / 更新（真实落盘）**：勾选「安装」或「更新」后，宿主在后台线程编排
`下载 ZIP → miniz 解压 → 规整内容根目录 → 拷入 plugins 目录`，完成后把成功项写进
本地记录 `installed.json`（`PluginRegistry::MarkInstalled`）。安装源优先级：
1) 本地已有 `<pluginsDir>\downloads\<folder>.zip`（离线安装，直接使用）；
2) catalog `repository` 字段（http/https，WinHTTP 下载；本地路径/file:// 直拷）；
3) 都没有 → 报「无下载地址」。
解压后按 N++ 打包惯例规整：ZIP 自带 `plugins\` 根 → 内容直接并入宿主 `plugins\`
（`plugins\Foo\NppExec.dll` 落 `plugins\Foo\NppExec.dll`）；否则（单子目录/平铺）
落进 `plugins\<folder>\`。zip-slip 防护：拒绝 `..` 段 / 绝对路径 / 盘符条目。
已安装页的"移除"仍只清本地记录并弹窗提示手动删真实文件（不替用户删 DLL——
目标 DLL 可能正被宿主加载）。

**组件与文件**：
| 文件 | 职责 |
|---|---|
| `src/plugin/PluginCatalog.h/.cpp` | 清单数据模型 + 极简 JSON 走读器（`ParsePluginList`）；`LoadPluginCatalog` 磁盘优先、缺失回落内嵌默认清单（`DefaultPluginListJson` 三种子插件，零配置可演示）；`ComparePluginVersions` 按 '.' 分段数值比较 |
| `src/plugin/PluginRegistry.h/.cpp` | 已安装判定：磁盘扫描（根/一级子目录）+ 本地记录并集；`ReadDllFileVersion` 读 DLL 版本资源（link `version` 库）；`installed.json` 读写 |
| `src/plugin/PluginInstaller.h/.cpp` | 下载/解压/落盘安装·更新编排：`WinHttpDownloader`（http/https/本地/file:// 直拷，进度回调）、`MinizZipExtractor`（读入内存逐条解压，zip-slip 拒绝）、`NormalizeExtractedRoot` 规整根目录；`Install(folder, version, url, progress)` 可后台线程调用 |
| `src/plugin/PluginAdminDialog.h/.cpp` | 模态对话框：TabControl 四页 + 搜索框 + ListView（勾选列）+ 信息/状态栏 + 主动作按钮/关闭；DPI 自适应；后台安装线程 + `WM_INSTALL_PROGRESS`/`WM_INSTALL_DONE` 进度/完成刷新（安装期间冻结换页/勾选/关闭） |
| `src/app/MainWindow.cpp` | `RebuildPluginMenu` 固定挂「插件管理(&A)…」入口（始终可见）；`Cmd::PluginAdmin=535` → `RunPluginAdmin` |
| `tests/test_plugin_catalog.cpp` | 单测：清单解析/版本比较/注册表安装·更新·持久化·移除（`ctest plugin_catalog`） |
| `tests/test_plugin_installer.cpp` | 单测：注入假下载器/解压器覆盖成功·离线·无 URL·下载失败·解压失败 + 真实 miniz ZIP 回环/zip-slip 拒绝/本地直拷（`ctest plugin_installer`） |

**已验证**：`ctest -C Release` 10/10 全绿（含新增 `plugin_installer`）；Release 构建通过。
`plugin_installer` 覆盖：NormalizeExtractedRoot 三种启发式、假下载+假解压成功落盘并清临时目录、
离线安装跳过下载器、无地址且无本地包报错、下载失败/解压失败均上报 `kFailed` 并传播原因、
真实 miniz 构造 ZIP 回环（`plugins\` 根并入宿主）、`../` zip-slip 条目被拒且不越界写盘、
WinHttpDownloader 非 http 分支本地直拷。

**过程中的坑**：
- 默认清单 raw string 内含 `"Toolbox Utils (示例)"`，`(示例)` 后紧跟 `"` 构成 `)"`
  序列 —— 恰好是 C++ raw string 的终止符 `)"`，导致字符串被提前截断、后续全行报语法错。
  修法：给 raw string 加自定义定界符 `R"PLUGINS( ... )PLUGINS"`。
- `PluginRegistry::LoadRecords` 曾用 `find(L'"')` 找键值分隔，末条记录（无尾逗号）的
  值被误判为空串；改为按 `':'` 定位分隔、再取开/闭引号。
- 应用目标 `src/CMakeLists.txt` 补了 `/utf-8`（源文件为 UTF-8 中文，测试目标早已有，
  应用目标缺失属不一致）。
- `State` 因含 `PluginRegistry`（无默认构造）被隐式删除默认构造，改显式
  `State(pluginsDir)` 构造。
- zip-slip 归一化陷阱：`NormalizeZipPath` 若用"跳过前导 `.` "的方式清洗路径，
  会把 `../evil.dll` 洗成 `evil.dll`，从而绕过 `IsUnsafeRelativePath` 的 `..` 检测。
  修法：只精确吞掉前导 `./`（两个字符），`..` 原样保留交给安全检测拦截。

### 5.4 决策记录
- **为什么 FuncItem 映射为我们的 PluginCommand 而非另起菜单管道**：palette/
  ExecuteCommand/测试全复用，一条分发路径少一类 bug；cmdID 回填写的是我们自己
  的 [10000..] 区间值（插件把它当不透明数）。
- **为什么 category 放插件名**：原生插件走扁平 label；NPP 形态天然按名字分组,
  平铺期用「名::项」保住归属可读性，未来换子菜单只是显示层替换。
- **isUnicode==FALSE 拒载**：与 N++ 现代行为一致；兼容报告记一行原因即可。

### 5.5 测试锚点
- `tests/npp_dll/test_npp_core.cpp`：最小合规 NPP 插件（六导出），两个 FuncItem
  可被 invoke 后由导出查询函数观察；断言 setInfo 收到的三句柄非零/特定值。
- `tests/test_plugin.cpp`：混合目录装载 → loaded==2、CommandCount==3、
  NPP 命令经 mgr.Execute 派发命中 _pFunc 计数器 —— 即"双分支装载+统一命令表"
  的回归网。
- 手工验证：把任一真实 NPP 简单插件放入 %APPDATA%\xfsWinPad\plugins\ 启动看菜单
  （能出菜单/能点响即为 4a 达标；NPPM_* 相关行为属 4b）。

---

## 6. 调试速查
- 加载链路日志均打在 `%LOCALAPPDATA%\xfsWinPad\logs\xfsWinPad.log`
  （PluginManager/插件 前缀）。ABI mismatch / 缺导出 / Unicode 断言都有显式日志。
- e2e 脚本：`scripts/plugin-v2-e2e.ps1`（WM_COMMAND 触发+WM_GETTEXT 回读）。
- 全量重建纪律：给 Manager/Workspace/MainWindow 加过成员后必须
  `cmake --build build --config Release --clean-first`，否则陈旧 obj 会伪装成逻辑 bug
  （历史教训见 LESSONS.md）。

## 7. Changelog
- 2026-08-27 初版：native v3 固化 + 4a 方案与协议事实卡（写于编码之前）。
- 2026-08-27 回填：4a 完成（双分支装载、混合测试、真实 GUI 验证记录）；
  补 §5.3 接手坑位提示（4b 入口建议 / buffer-id 映射建议 / 快捷键暂存说明）。
- 2026-08-27 第二批：NPP 形态 UI 对齐——分组子菜单 + ShortcutKey→主加速表
  （均含回归断言）；§5.6 记录 4b 挂起原因与五分钟恢复配方。
- 2026-08-27 第三批：4b 完成——协议常量经 gitee 镜像联机核实写定
  `NppMessages.h`；`ForwardNppMessage` 垫片子集 + `NppDocSource` 抽象 +
  `Workspace::ReloadDocument`；MainWindow 拦截转发；`test_npp_msgs` 单测
  （ctest 7/7 全绿）；修复 wcsncpy_s 声明尺寸导致的 Debug 堆损坏。
- 2026-08-27 第四批：4c 完成——`NppNotify` 通知码（NPPN_FIRST=1000）+ 三层
  转发链（EmitNppNotification/BroadcastNppNotification/NppAdapter::Notify）；
  MainWindow 六处事件合成（READY/FILEOPENED/BUFFERACTIVATED/FILESAVED/
  FILECLOSED/SHUTDOWN）；test_npp_core.dll beNotified 观察口 + test_plugin
  契约断言（ctest 7/7 全绿）。
- 2026-08-27 第五批：4d 完成——`NppDocking.h` 抄录 tTbData 二进制契约与
  DMN_*/DMM_*/DWS_* 常量；`DockHost` 抽象 + PluginManager 的 NPPM_DMM*
  转发（无宿主明确拒答）；`DockManager` wrapper 宿主（注册/显示/隐藏/关闭
  协商/查询）+ MainWindow 底部 dock 链集成；test_npp_msgs 新增 DMM 契约断言
  （ctest 7/7 全绿）。
- 2026-08-27 第六批：4d 端到端——`tests/npp_dock_dll/test_npp_dock.dll`
  （`.rc` 资源对话框 + `NPPM_DMMREGASDCKDLG` 注册 + DMN_* 观察口）与
  `tests/test_dock.cpp`（真实 DockManager/PluginManager 全链路：wrapper/
  reparent/DMN 通知/布局/关闭协商/veto/宿主退出/二次注册）；实证对话框
  veto 必须显式置 `DWLP_MSGRESULT=TRUE` 才会回传（真实 NPP 插件惯例，
  裸 `return TRUE` 被对话框管理器吞成 0）；ctest 8/8 全绿。
- 2026-08-28 第七批：NppExec 子菜单 20 项实测 + 宿主修复——
  ① `DockManager::AskClose`/`NotifyDialog` 的 `DMN_CLOSE` 通知 `nm.hwndFrom`
  由面板 wrapper 句柄改为主窗口句柄（NppExec 只认 npp 主窗口句柄才处理），
  修复"手动关闭 NppExec Console 后 Show NppExec Console 仍勾选"问题。
  ② 逐条核实问题 1–4 均为 NppExec 自身设计、非宿主机 bug（结论记录于
  ChangeLog 上一条的验证纪要）：Execute NppExec Script 对话框不预填选中文本
  （要执行选中文本请用 Execute Selected Text，已实测正常）；Toggle Console
  默认只切换焦点（`HideToggled` 默认关）；Go to next/prev error 须启用错误
  过滤器才亮起；Console Commands History 只切换"记录历史"选项不弹框。
  ③ 插件模态对话框（PluginDialogBox 路径，如 Console Output Filters...）
  实测正常弹出，宿主对话框通道成立。
- 2026-08-28 第八批：插件管理（§5.9）——`PluginCatalog`（nppPluginList 布局清单
  解析 + 内嵌默认清单 + 版本比较）、`PluginRegistry`（磁盘扫描∪本地记录、
  installed.json 持久化）、`PluginAdminDialog`（四标签 可用/更新/已安装/不兼容 +
  实时搜索 + 安装/更新/移除）、MainWindow「插件管理」菜单入口（Cmd::PluginAdmin=535）；
  `test_plugin_catalog` 单测，ctest 9/9 全绿。修三个真 bug：默认清单 raw string 被
  `)"` 提前终止（改 `R"PLUGINS(...)PLUGINS"` 定界）、LoadRecords 末条值解析为空
  （按 ':' 定位分隔）、应用目标补 `/utf-8`。实现边界：安装/更新/移除仅流转本地记录，
  不下载不落盘（repository 预留）。
- 2026-08-28 第九批：插件下载/安装/更新（真实落盘）——
  ① 内嵌 miniz 3.1.2（ZIP 读/写，静态库，`third_party/miniz`，根 CMake 加 `C` 语言）。
  ② `PluginInstaller`：`WinHttpDownloader`（http/https + 本地路径/file:// 直拷，进度
  回调）、`MinizZipExtractor`（整体读入内存逐条解压，zip-slip 拒绝）、
  `NormalizeExtractedRoot` 规整内容根（ZIP 自带 `plugins\` 根并入宿主，否则落
  `plugins\<folder>\`）、安装源优先级 本地包→repository→报错。
  ③ `PluginAdminDialog` 后台线程安装：勾选 → worker 下载+解压+落盘 →
  `WM_INSTALL_PROGRESS`/`WM_INSTALL_DONE` 驱动状态栏与完成提示；安装期间冻结
  换页/勾选/关闭；成功项写 `installed.json`。
  ④ `test_plugin_installer` 单测（注入假下载器/解压器 + 真实 miniz ZIP 回环 +
  zip-slip 拒绝 + 本地直拷），ctest 10/10 全绿。修复 zip-slip 归一化会吞 `..` 的隐患。
