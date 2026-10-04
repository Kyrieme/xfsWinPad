#pragma once
// OopProtocol.h — 进程外插件桥协议 v2（原创实现，无 NPP 源码依赖）。
//
// 背景：个别 NPP 兼容插件会在 setInfo 里直接 exit()（实测 ComparePlus）或
// 在 beNotified/命令回调里访问违例——进程内加载时这些都会杀死宿主编辑器，
// SEH 无法拦截 exit()。本协议把插件 DLL 装进 xfsWinPadPluginHost.exe 代理
// 进程：插件死，编辑器活着。
//
// v2（批次 66）：一个代理进程承载多个插件（多槽位）。代理启动不带插件，
// 编辑器对 slot0 控制窗发 OOPM_ADD 动态装载；每个插件获得自己的消息窗，
// EXEC/NOTIFY/CMDIDS 按窗寻址（路由与 v1 完全一致）。装载失败经
// OOPM_REJECT 回执（进程不陪葬）；插件在 setInfo 里 exit() 仍会带走整个
// 代理进程——编辑器把死亡归因于正在装载的那个，幸存者撤销命令后重进新
// 代理。共享代理是内存换隔离粒度的取舍：运行期某插件把代理搞崩，同车
// 插件连坐记账（oop-died）。
//
// 传输：WM_COPYDATA（SendMessage 同步语义，跨进程内核拷贝，无共享内存）。
//   编辑器 → 插件宿主进程：OOPM_ADD / OOPM_EXEC / OOPM_NOTIFY / OOPM_SHUTDOWN
//                             OOPM_MSG（messageProc 桥，v2.1）
//   插件宿主进程 → 编辑器：  OOPM_HANDSHAKE（插件名 + FuncItem 表转录）
//                             OOPM_REJECT（装载拒绝回执）
//                             OOPM_MSGREPLY（messageProc 返回值，v2.1）
//                             OOPM_NPPMCALL（NPPM_* 过桥请求，v2.2）
//                             OOPM_SCICALL（SCI_* 入参指针过桥请求，v2.4）
//                             OOPM_SCIOUTCALL（SCI_* 出参指针过桥请求，v2.5）
//                             OOPM_SCISTRUCTCALL（SCI_* 结构体指针过桥请求，v2.7）
//                             OOPM_DMMRELAYREPLY（停靠中继的应答，v2.8）
//   编辑器 → 插件宿主进程：OOPM_ADD / OOPM_EXEC / OOPM_NOTIFY / OOPM_SHUTDOWN
//                             OOPM_MSG（messageProc 桥，v2.1）
//                             OOPM_DMMRELAY（停靠通知与动作请求，v2.8）
//   编辑器应答：            OOPM_CMDIDS（宿主分配的命令 id 回填）
//                             OOPM_NPPMREPLY（NPPM_* 结果 + 出参，v2.2）
//                             OOPM_SCIREPLY（SCI_* 结果，v2.4）
//                             OOPM_SCIOUTREPLY（SCI_* 出参结果 + 字节，v2.5）
//                             OOPM_SCISTRUCTREPLY（结构体结果 + 字节/chrgText，v2.7）
//
// ⚠ 停靠族（NPPM_DMM*）的**正向**调用走 OOPM_NPPMCALL/REPLY（形状表里加
//   kDmmReg/kDmmTwoStr 两条即可，载荷见 DmmMarshal.h），**只有反向的通知
//   与动作请求**另开 OOPM_DMMRELAY/DMMRELAYREPLY —— 原因见枚举处的注释
//   （跨进程 WM_NOTIFY 被拒 + 直发 DMM_* 的挂起风险）。
//
// v2.2（批次 111）：插件 setInfo 拿到的 nppHandle **不再是编辑器主窗口**，
// 而是代理进程内的**中转窗**。原因见 NppmMarshal.h 头注释：NPPM_* 在
// WM_USER 之上 ⇒ Windows 不封送参数 ⇒ 进程外插件把 NPPM_* 发给宿主主窗口
// 时 lp 是代理进程的地址，宿主解引用即访问违例（批次 110 已把"崩"变成
// "拒答"，本批把"拒答"变成"过桥"）。中转窗按 NppmMarshal.h 的分类把调用
// 封送过来，编辑器在**自己的地址空间**里准备入/出参缓冲再走既有的
// ForwardNppMessage —— 于是宿主看到的每个指针都是自己的。
// ⚠ 中转窗只转发 Windows **自身会封送**的消息（WM_COPYDATA / WM_GETTEXT /
//   WM_SETTEXT / WM_GETTEXTLENGTH）与两个 NPPM 区间；其余一律 DefWindowProc。
//   转发一个带外来指针的普通消息 = 把批次 110 修的崩溃重新引进来。
//
// v2.4（批次 113）：SCI_* 的**入参指针**族（kInStr 53 + kInBytes 1 = 54 条）
// 也走 wire（OOPM_SCICALL/SCIREPLY）。批次 112 只直发 716 条不带指针的消息、
// 其余拒答；本批把"入参串"这一族接上：中转窗在**本进程**把串读出来随载荷
// 内联，编辑器在自己的地址空间里重建指针再发给 Scintilla。参数布局（指针在
// wp 还是 lp、长度从哪来）由 scripts/gen-sci-marshal.py 从 Scintilla.iface
// 生成（SciBridgeTable.inc）—— 54 条里有 19 条的串在 wParam、2 条双指针、
// 1 条的首参名 lengthEntered **不是**缓冲长度，手写必错。
// 出参族（kOutStr/kInOutStr）、结构体族（kStruct）**当时**仍拒答（随后由
// v2.5–v2.7 逐族接上，见下）；kRawPtr/kPtrRet 语义上不可桥接（**永久**）。
//
// v2.5（批次 114）：SCI_* 的**出参指针**族（kOutStr 30 条）也走 wire
// （OOPM_SCIOUTCALL/SCIOUTREPLY）。方向与入参族相反，难点也不同 ——
// **容量不在消息里**：唯一的来源是 Scintilla 出参协议自带的 lParam==0 调用
// （不写缓冲、返回需要的字节数）。宿主探出 need 后在自己的缓冲里真调用一次，
// 把字节回带，中转窗再拷进插件给的缓冲。回传多少字节由实现的 NUL 语义决定，
// 而 StringResult 写 len+1、BytesResult 写 len —— **只差一个字节**，差一字节
// 就是越界写 1 字节（堆破坏且多半不崩）。分类由 scripts/gen-sci-marshal.py
// 从 Scintilla 实现生成（SciOutTable.inc）。
// kInOutStr(5)、kStruct(8) 此时仍拒答（v2.6/v2.7 接上，见下）；TargetAsUTF8 因
// NUL 语义随运行期文档编码变化而**必须**拒答。
//
// v2.6（批次 115）：出入参族（kInOutStr）—— 形状 = 入参族的一半 + 出参族的一半
// （wParam 是入参串、lp 是接收缓冲），复用 OOPM_SCIOUTCALL/REPLY 的扩展载荷。
// v2.7（批次 116）：结构体族（kStruct）—— 指针指向结构体、结构体里还有串/坐标，
// 新增 OOPM_SCISTRUCTCALL/SCISTRUCTREPLY（载荷展平结构 + 内联串）。
// ★ 至此带指针的 SCI_* 只剩 kRawPtr/kPtrRet 按语义**永久**拒答（返回的是文档内部
//   指针），外加各族点名的拒答桶。**各族的划分与"谁拒答"以 SciBridge.h 头注释为
//   唯一来源** —— 本文件与 PluginHostMain.cpp 只做索引、不复述计数。上面 v2.4/v2.5
//   两段末尾的"仍拒答"是**当时快照**却无人收回，正是"进度叙述抄三份就会漂移"的实例。
//
// 载荷布局 = 公开契约（与 插件系统设计笔记 §5.1 同规则：字段顺序即契约，
// 只能尾部追加）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {
namespace oop {

// WM_COPYDATA.dwData 魔数（与单实例转发的 'XFWP'=0x58465750 无冲突）
constexpr UINT_PTR kMagic = 0x4F4F5031;   // 'OOP1'

enum OopMsg : UINT_PTR {
    OOPM_HANDSHAKE = 1,   // host→editor：插件名 + FuncItem 表（ItemWire 数组）
    OOPM_CMDIDS    = 2,   // editor→host：逐项命令 id（宿主分配，回填 FuncItem.cmdID）
    OOPM_EXEC      = 3,   // editor→host：执行 items[index].func（SEH 包裹）
    OOPM_NOTIFY    = 4,   // editor→host：beNotified({code,idFrom})（SEH 包裹）
    OOPM_SHUTDOWN  = 5,   // editor→host：退出（SHUTDOWN 通知已由 Notify 广播送达，
                          //   本消息只令代理进程退出，绝不二次回调 beNotified）
    OOPM_ADD       = 6,   // editor→host(slot0)：动态装载一个插件 DLL（v2 尾部追加）
    OOPM_REJECT    = 7,   // host→editor：装载拒绝回执（v2 尾部追加；代理进程无恙，
                          //   reason 取 kExit* 约定值）
    OOPM_READY     = 8,   // host→editor：代理进程就绪（v2 尾部追加；wp=slot0 窗，
                          //   载荷仅 magic+msg）
    OOPM_MSG       = 9,   // editor→host：调用槽插件 messageProc(msg,wp,lp)，
                          //   结果经 OOPM_MSGREPLY 回带（v2.1 尾部追加）
    OOPM_MSGREPLY  = 10,  // host→editor：messageProc 的 LRESULT（v2.1 尾部追加）
    OOPM_NPPMCALL  = 11,  // host→editor：NPPM_* 过桥请求（v2.2 尾部追加）
    OOPM_NPPMREPLY = 12,  // editor→host：NPPM_* 结果 + 出参（v2.2 尾部追加）
    OOPM_SCICALL   = 13,  // host→editor：SCI_* **入参指针**过桥请求（v2.4 尾部追加）
    OOPM_SCIREPLY  = 14,  // editor→host：SCI_* 结果（v2.4 尾部追加）
    OOPM_SCIOUTCALL = 15, // host→editor：SCI_* **出参指针**过桥请求（v2.5 尾部追加）
    OOPM_SCIOUTREPLY = 16,// editor→host：SCI_* 出参结果 + 字节（v2.5 尾部追加）
    OOPM_SCISTRUCTCALL = 17,  // host→editor：SCI_* **结构体指针**过桥请求（v2.7）
    OOPM_SCISTRUCTREPLY = 18, // editor→host：结构体过桥结果 + 字节/chrgText（v2.7）
    // ---- v2.8（批次 117）：停靠族（NPPM_DMM*）的**反向**中继 ------------------
    // 正向（插件 → 编辑器：注册/显示/隐藏/刷新/切换/按名查句柄）复用
    // OOPM_NPPMCALL/REPLY —— 形状表新增 kDmmReg / kDmmTwoStr 两条即可，
    // 载荷布局见 DmmMarshal.h。这里只补**编辑器 → 插件**那一半，因为
    // DockManager 的通知（DMN_DOCK/DMN_SWITCHIN/可 veto 的 DMN_CLOSE）与
    // DMM_* 动作请求**必须由代理在它自己的进程里发**：
    //   · 跨进程 `WM_NOTIFY` 被系统拒绝（err=5 ACCESS_DENIED，实测；
    //     `lParam=0` 照样拒 ⇒ 与指针无关，也不是 UIPI）；
    //   · 直发 DMM_*（`WM_USER` 段，实测能送达）同样不行 —— 会让编辑器 UI
    //     线程去等一个可能不泵消息的插件线程（挂起风险）。
    //
    // ★ 通知与动作**合成一条**（kind 判别）而不是两条：两者的代理侧行为、
    //   归属校验、应答与记账完全相同，只差"发哪种消息"一个字段。合成后
    //   reqId/等待槽/计数器都只有一份（批次 114 的教训：平行参照要同步，
    //   少一份就少一处漂移）。
    OOPM_DMMRELAY      = 19,  // editor→host：请代理本进程内给 hClient 发一条消息
    OOPM_DMMRELAYREPLY = 20,  // host→editor：delivered + LRESULT（veto 用它）
};

// 单次过桥载荷上限（字节）：入参串与出参缓冲各自不得超过它。
// wire 是外来数据 ⇒ 宿主侧必须按此钳制 outCap（见 OopHost::HandleNppmCall）。
constexpr unsigned kNppmPayloadMax = 32768;

// SCI_* 入参过桥的单段上限（v2.4）。超限一律**拒答**而不是截断 ——
// 截断会静默改变语义（例如 SearchInTarget 的 pattern 被砍掉一半）。
// 与 kNppmPayloadMax 同量级：WM_COPYDATA 上一次内核拷贝的量级。
constexpr unsigned kSciPayloadMax = 32768;

// FuncItem 跨进程转录：内联名 + 快捷键。函数指针留在代理进程内，
// 编辑器侧命令以 index 寻址执行。
struct ItemWire {
    wchar_t      name[64];               // 与 FuncItem.itemName 同宽（128B）
    unsigned char ctrl, alt, shift, key; // ShortcutKey 展开；key==0 = 无快捷键
};

// OOPM_EXEC / OOPM_NOTIFY / OOPM_SHUTDOWN 的定长载荷（进入 OOPM_SHUTDOWN 时
// 仅 magic/msg 有效）
struct ExecWire {
    UINT_PTR   magic;
    UINT_PTR   msg;
    int        index;                    // EXEC：FuncItem 下标
    unsigned int code;                   // NOTIFY：NPPN_* 通知码
    UINT_PTR   idFrom;                   // NOTIFY：BufferID（编辑器侧 Document* 空间，
                                         //   对插件而言是不透明句柄，与 N++ 语义一致）
};

// OOPM_CMDIDS 载荷：头 + count 个 int 尾随数组
struct CmdIdsWire {
    UINT_PTR magic;
    UINT_PTR msg;
    int      count;
    // int ids[count]; —— 跟随在结构体之后
};

// 握手载荷（host→editor）。cookie = 编辑器 ADD 请求携带的对账值，原样回带；
// v2 起必填（代理与编辑器同包发布，无跨版本兼容负担）。
struct HandshakeWire {
    UINT_PTR magic;
    UINT_PTR msg;
    int      itemCount;
    wchar_t  pluginName[64];             // getName() 结果（NUL 结尾）
    ItemWire items[128];                 // FuncItem 上限（NppExec 实测 24）
    UINT_PTR cookie;                     // 尾部追加（v2）
    UINT_PTR slotWnd;                    // 尾部追加（v2）：插件槽窗（CMDIDS/EXEC 目标）
};
constexpr int kHandshakeItemsMax = 128;

// OOPM_ADD 载荷（editor→host slot0）：装载指定 DLL 并握手。
// path 为 NUL 结尾绝对路径（DLL 最大路径 260 内，含结尾符）。
constexpr int kAddPathMax = 260;
struct AddWire {
    UINT_PTR   magic;
    UINT_PTR   msg;
    UINT_PTR   cookie;
    wchar_t    path[kAddPathMax];
    UINT_PTR   deadlineMs;               // 本次装载的看门狗时限（0=代理默认）
};

// OOPM_REJECT 载荷（host→editor）：装载失败回执，reason 取 kExit* 约定值。
struct RejectWire {
    UINT_PTR magic;
    UINT_PTR msg;
    UINT_PTR cookie;
    UINT_PTR reason;
};

// OOPM_MSG 载荷（editor→host 槽窗）：messageProc 桥（v2.1）。安全红线：
// 只允许 wp/lp 均为值类型（整数/HWND/HMENU）的消息过桥——编辑器侧白名单
// 把关，任何指针参数（如 WM_SETTINGCHANGE 的 LPWSTR）一律不过桥。
// reqId = 编辑器请求序号，回带对账；超时的回包按 reqId 失配丢弃。
struct MsgWire {
    UINT_PTR magic;
    UINT_PTR msg;                        // 恒 OOPM_MSG（wire[1] 惯例不变）
    UINT_PTR reqId;
    UINT_PTR wndMsg;                     // 转发给 messageProc 的窗口消息（WM_*）
    UINT_PTR wParam;
    LONG_PTR lParam;
};

// OOPM_MSGREPLY 载荷（host→editor）：插件 messageProc 的返回值。
// slotWnd = 应答槽窗（编辑器对账归属）；插件未导出 messageProc 时
// 代理以 result=0 应答（NPP 语义：未处理）。
struct MsgReplyWire {
    UINT_PTR magic;
    UINT_PTR msg;                        // 恒 OOPM_MSGREPLY
    UINT_PTR reqId;
    UINT_PTR wndMsg;                     // 被应答的窗口消息（诊断）
    LONG_PTR result;
    UINT_PTR slotWnd;
};

// OOPM_NPPMCALL 载荷（代理 slot0 → editor）：一次 NPPM_* 过桥请求（v2.2）。
// 入参字符串**内联在结构体之后**（不跨进程传指针），字节数见 inBytes。
// replyTo 指向代理侧的接收窗（slot0 控制窗）—— 见 PluginHostMain.cpp 的
// 注释：回包必须落在代理的**主线程窗**上，因为插件命令回调跑在代理主线程，
// 而它此刻正阻塞在自己的 SendMessage 里（Windows 会把 incoming sent message
// 投递到阻塞线程的栈上，CMDIDS 走的也是这条语义）。
//
// ⚠ 字段顺序是**协议契约**：magic 必须是 kMagic、msg 必须是本消息 id ——
//   两侧 WndProc 都在**分发之前**先校验 wire[0]==kMagic，再按 wire[1] 分派
//   （wire = (UINT_PTR*)载荷）。把 NPPM_* 编号放在 msg 上会让 wire[1] 变成
//   0x0834 之类 ⇒ 落到 default 分支被静默丢弃。症状极具误导性：插件的
//   WM_COPYDATA 标记照常送达（那条路不经过这层校验），而所有 NPPM_* 静默
//   返回 0 —— 看起来像"中转窗没建起来"。真正的 NPPM_* 编号在 nppm 字段。
struct NppmCallWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_NPPMCALL
    unsigned nppm;       // NPPM_* 编号
    UINT_PTR wParam;     // 原样透传的 wParam（值语义）
    UINT_PTR reqId;      // 代理侧单调序号；回包对账，失配即丢弃
    UINT_PTR replyTo;    // 代理侧接收窗 HWND
    unsigned inBytes;    // 紧随其后的入参字节数（≤ kNppmPayloadMax）
    unsigned outCap;     // 请求的出参字节数（0 = 只要返回值；≤ kNppmPayloadMax）
    // ---- v2.8（批次 117）尾部追加：lParam 的**值语义**部分 -------------------
    // ★ 为什么必须补这一格：NppmMarshal.h 把 kValue 定义为"wp/lp 都是值 ⇒
    //   原样过桥"，但本 wire 原来只有 wParam ⇒ 宿主侧拿到的是 lp == 0。
    //   而 kValue 里**真有 4 条读 lp**：
    //     · NPPM_SETMENUITEMCHECK   lp = 勾选位 ⇒ 恒被当成"取消勾选"（用户可见）
    //     · NPPM_GETNBOPENFILES     lp = 视图类型 ⇒ 副视图查询答成主视图
    //     · NPPM_RELOADBUFFERID     lp = 强制位   ⇒ 恒按"不强制"重载
    //     · NPPM_GETBUFFERIDFROMPOS lp = 视图     ⇒ 副视图查询被错误接受
    //   症状一律是**静默错**（不是崩），所以只有"答案取决于 lp"的断言能抓住。
    // 规则是"字段顺序即契约，只能尾部追加" ⇒ 不插在 wParam 旁边。
    // 指针槽一律填 0（外来地址绝不上 wire）；只有 kValue 两个槽都是值。
    UINT_PTR lParam;
    // unsigned char payload[inBytes];
};

// OOPM_NPPMREPLY 载荷（editor → 代理接收窗）：过桥结果（v2.2）。
// outBytes 为紧随其后的出参字节数；payload 一律是**宿主本地缓冲的内容**，
// 由代理侧拷进插件给的 lp（拷贝长度按插件声明的容量再钳一次）。
struct NppmReplyWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_NPPMREPLY
    unsigned nppm;       // 回显 NPPM_* 编号（诊断）
    UINT_PTR reqId;
    LONG_PTR result;     // ForwardNppMessage 的 LRESULT
    unsigned outBytes;
    // unsigned char payload[outBytes];
};

// OOPM_SCICALL 载荷（代理 SCI 中转窗 → editor）：一次 SCI_* **入参**过桥请求
// （v2.4）。入参字节内联在结构体之后（不跨进程传指针），段数见 inBytes /
// inBytes2（只有 kStrBoth 那条形态才有第二段）。
//
// argWp / argLp 装的是**值语义**的那部分：指针所在的槽填 0，由宿主按布局换成
// 本地缓冲地址。这样宿主侧不需要知道任何来自代理的地址。
//
// ⚠ 字段顺序是**协议契约**（与 NppmCallWire 同规则）：magic 必须是 kMagic、
//   msg 必须是本消息 id —— 两侧 WndProc 都在**分发之前**校验 wire[0]==kMagic
//   再按 wire[1] 分派。SCI_* 编号在 sciMsg 字段，**不是** msg。
struct SciCallWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_SCICALL
    unsigned sciMsg;     // SCI_* 编号
    UINT_PTR argWp;      // 值语义 wParam（若 wp 是指针槽则填 0）
    UINT_PTR argLp;      // 值语义 lParam（若 lp 是指针槽则填 0）
    UINT_PTR reqId;      // 代理侧单调序号；回包对账，失配即丢弃
    UINT_PTR replyTo;    // 代理侧接收窗 HWND
    unsigned inBytes;    // 第一段字节数（≤ kSciPayloadMax）
    unsigned inBytes2;   // 第二段字节数（kStrBoth；否则 0）
    // unsigned char payload[inBytes + inBytes2];
};

// OOPM_SCIREPLY 载荷（editor → 代理接收窗）：入参过桥的结果（v2.4）。
// 入参族没有出参缓冲（出参族是下一批）⇒ 只有返回值。
//
// handled 是**必要的**区分位：宿主拒答时 result 也是 0，而 Scintilla 真返回 0
// 时 result 同样是 0 —— 两者同形。没有它，代理侧就分不清"过桥成功但结果就是
// 0"与"宿主根本没转过去"（批次 112 的教训：判据要落在"对方是否收到"上）。
struct SciReplyWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_SCIREPLY
    unsigned sciMsg;     // 回显 SCI_* 编号（诊断）
    UINT_PTR reqId;
    LONG_PTR result;     // SendMessage 给宿主 Scintilla 的 LRESULT
    unsigned handled;    // 1 = 真的转给了宿主 Scintilla；0 = 宿主侧拒答
};

// OOPM_SCIOUTCALL 载荷（代理 SCI 中转窗 → editor）：一次 SCI_* **出参**过桥
// 请求（v2.5）。与入参族方向相反：这里**没有**要带过来的字节，只有"插件想要
// 多少"这个问题 —— 而容量不在消息里（见 SciBridge.h 的说明）。
//
// writeBack = 0 表示插件自己在查长度（它用 lParam==0 调进来的），宿主只需回
// 一个 need；= 1 表示插件给了接收缓冲，宿主探长度 → 真调用 → 把字节回带。
// 出参族的 wp 都是**值**（出参族里没有"串在 wp"的形态），所以 argWp 一律原样
// 透传，不需要 SciInPtrSlots 那套。
//
// ⚠ 字段顺序是**协议契约**（与 NppmCallWire / SciCallWire 同规则）：magic 必须
//   是 kMagic、msg 必须是本消息 id —— 两侧 WndProc 都在**分发之前**校验
//   wire[0]==kMagic 再按 wire[1] 分派。SCI_* 编号在 sciMsg 字段，**不是** msg。
struct SciOutCallWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_SCIOUTCALL
    unsigned sciMsg;     // SCI_* 编号
    UINT_PTR argWp;      // 值语义 wParam（**不带**入参串的族用它；带了就必须是 0）
    UINT_PTR reqId;      // 代理侧单调序号；回包对账，失配即丢弃
    UINT_PTR replyTo;    // 代理侧接收窗 HWND
    unsigned writeBack;  // 1 = 回带字节；0 = 只要 need
    // ---- v2.6（批次 115）：出入参族（kInOutStr）的**入参串** ----------------
    // 0 = 本消息不带入参串（kOutStr 族，wParam 是 argWp 那个值）。
    // >0 = 紧随本结构之后有 inBytes 个字节，**最后一个必须是 '\0'**；宿主侧
    //      用它重建 wParam（一个宿主本地指针），不用 argWp。
    // 两侧共用 SciOutNeedsInStr(msg) 判定"该不该带"，所以这个字段是**被推出来的**
    // 契约：带串的族 inBytes 必须 >0 **且 argWp 必须为 0**，不带串的族 inBytes
    // 必须 ==0 —— 三头都硬校验（见 SciBridge.cpp 的五道校验）。
    unsigned inBytes;
    // payload[inBytes] 紧随其后（cbData == sizeof(SciOutCallWire) + inBytes）
};

// OOPM_SCIOUTREPLY 载荷（editor → 代理接收窗）：出参过桥的结果（v2.5）。
// payload 紧随其后，长度见 copied（**不是** need —— 见 SciBridge.h 的
// "回传多少字节"一节：kNul 比 kNoNul 多一个结尾 NUL）。
//
// handled 与 SciReplyWire 同理且同样必要：宿主拒答时 result 也是 0，而
// Scintilla 真返回 0 时 result 同样是 0 —— 两者同形。
// need 是探长度那次的返回值（诊断 + 代理侧对账用），copied 是实际回带字节数。
struct SciOutReplyWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_SCIOUTREPLY
    unsigned sciMsg;     // 回显 SCI_* 编号（诊断）
    UINT_PTR reqId;
    LONG_PTR result;     // 真调用（或查长度那次）的 LRESULT
    unsigned handled;    // 1 = 真的转给了宿主 Scintilla；0 = 宿主侧拒答
    unsigned need;       // lParam==0 探到的容量（handled==0 时无意义）
    unsigned copied;     // 紧随其后的字节数（≤ kSciPayloadMax + 1）
    // unsigned char payload[copied];
};

// OOPM_SCISTRUCTCALL 载荷（代理 SCI 中转窗 → editor）：一次 SCI_* **结构体指针**
// 过桥请求（v2.7，批次 116）。
//
// 与前三族的关键差别：这里**没有外来指针随 wire 走** —— 插件那个结构体已经被
// **展平**成字段（cpMin / cpMax）与一段可选的串（FindText 的 needle）。宿主侧按
// sciMsg 重建一个**自己的**结构体（TextRange / TextToFind …），调用完再把结果回带。
//
// cpMin/cpMax 用 INT_PTR 而不是 long：非 Full 变体的 Sci_PositionCR 是 long
// （4 字节），Full 变体的 Sci_Position 是 intptr_t（8 字节）—— wire 一律按最宽的
// 那个走，由**表里的 full 标志**决定宿主该建哪种结构体（不靠名字猜）。
//
// ⚠ 字段顺序是**协议契约**（与 NppmCallWire / SciCallWire / SciOutCallWire 同规则）：
//   magic 必须是 kMagic、msg 必须是本消息 id —— 两侧 WndProc 都在**分发之前**
//   校验 wire[0]==kMagic 再按 wire[1] 分派。SCI_* 编号在 sciMsg 字段，**不是** msg。
struct SciStructCallWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_SCISTRUCTCALL
    unsigned sciMsg;     // SCI_* 编号
    UINT_PTR argWp;      // 插件给的 wParam 原样透传（FindText 的 searchFlags；
                         //   Get*Range 族不用它，原生也忽略）
    UINT_PTR reqId;      // 代理侧单调序号；回包对账，失配即丢弃
    UINT_PTR replyTo;    // 代理侧接收窗 HWND
    INT_PTR  cpMin;      // 展平出来的 chrg.cpMin
    INT_PTR  cpMax;      // 展平出来的 chrg.cpMax（kRangeOut 里 -1 = 「到文档末尾」）
    unsigned inBytes;    // kFindInOut 的 needle 字节数（**含结尾 NUL**）；其余族必须为 0
    // payload[inBytes] 紧随其后（cbData == sizeof(SciStructCallWire) + inBytes）
};

// OOPM_SCISTRUCTREPLY 载荷（editor → 代理接收窗）：结构体过桥的结果（v2.7）。
//
// 回带两样东西（按族二选一，另一路为 0）：
//   * copied 字节 payload      —— kRangeOut / kStyledOut 的**出缓冲内容**；
//   * hasChrg + chrgMin/chrgMax —— kFindInOut 的 **chrgText**（**只在被写时才有效**）。
//
// ★ hasChrg 是**必要的**区分位，理由与 handled 同源：FindText 只在 pos != -1 时写
//   chrgText（Editor.cxx:4318/4349），所以"没找到"（一个字节都没写）与"找到了
//   (0,0)"必须能分开 —— 否则代理侧会把插件结构体里**原有的** chrgText 当成结果。
//   宿主侧用 canary 判定"到底写没写"，**不复刻 pos != -1**。
//   ★ 两个变体的 canary **必须分开给**：Full 用 INT_PTR 的最小值
//   （kSciStructChrgCanary），非 Full 的 chrgText 字段是 **long**，要用
//   kSciStructChrgCanaryCR —— 把 INT_PTR 的 canary **截断**成 long 会得到 0，
//   而 0 是**合法的命中位置** ⇒ 非 Full 的 FindText 在位置 0 命中会被读成"没写"
//   （批次 116 负控③ 抓到的真缺陷）。
//
// handled 与 SciReplyWire / SciOutReplyWire 同理且同样必要：宿主拒答时 result 也是 0，
// 而 Scintilla 真返回 0 时 result 同样是 0 —— 两者同形。
struct SciStructReplyWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_SCISTRUCTREPLY
    unsigned sciMsg;     // 回显 SCI_* 编号（诊断）
    UINT_PTR reqId;
    LONG_PTR result;     // 真调用的 LRESULT
    unsigned handled;    // 1 = 真的转给了宿主 Scintilla；0 = 宿主侧拒答
    unsigned copied;     // 紧随其后的字节数（≤ kSciPayloadMax；Find 族恒 0）
    unsigned hasChrg;    // 1 = chrgText 有效（kFindInOut 且 Scintilla 真的写了）
    INT_PTR  chrgMin;    // chrgText.cpMin
    INT_PTR  chrgMax;    // chrgText.cpMax
    // unsigned char payload[copied];
};

// OOPM_DMMRELAY 载荷（editor → 代理 slot 窗）：请代理在**它自己的进程里**
// 给插件对话框发一条消息（v2.8）。
//
// kind 判别两种：
//   * `kDmmRelayNotify` —— 发 `WM_NOTIFY`，`NMHDR{idFrom, code}`。
//     `code` 取 `DMN_*`（`DMN_DOCK` / `DMN_SWITCHIN` / 可 veto 的 `DMN_CLOSE`）。
//   * `kDmmRelayAction` —— 发 `DMM_*` 动作请求（`action`），wp/lp 恒 0。
//
// ★ 为什么必须绕这一圈：跨进程 `WM_NOTIFY` 被系统拒绝（err=5 ACCESS_DENIED，
//   实测；`lParam=0` 照样拒 ⇒ 与指针无关）；而直发 `DMM_*`（`WM_USER` 段，
//   实测能送达）会让编辑器 UI 线程去等一个可能不泵消息的插件线程。
//
// ★ 安全红线：`hClient` 是**外来值**。给它发 `WM_NOTIFY` 等于把一个**本进程
//   栈上**的 NMHDR 指针交出去 —— 若它不是本进程的窗口，那就是批次 110 修掉的
//   那类崩溃。所以代理侧先验归属（`IsOwnedWindow`），验不过**什么都不发**，
//   并在应答里把 `delivered` 置 0（让"没送到"与"插件答了 0"可分）。
//   `hwndFrom` **不上 wire**：代理自己填（= 它的中转窗，插件眼里的"编辑器
//   窗口"，见 PluginHostMain.cpp 的 `NppHandleForPlugins()`）。
//
// ⚠ 字段顺序是**协议契约**（与 NppmCallWire 同规则）：magic 必须是 kMagic、
//   msg 必须是本消息 id。
constexpr unsigned kDmmRelayNotify = 0;   // kind：WM_NOTIFY
constexpr unsigned kDmmRelayAction = 1;   // kind：DMM_*

struct DmmRelayWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_DMMRELAY
    UINT_PTR hClient;    // 插件对话框 HWND（值；代理侧要验归属）
    unsigned kind;       // kDmmRelayNotify / kDmmRelayAction
    UINT_PTR idFrom;     // kind==Notify：NMHDR.idFrom（= DockedWidgetData.dlgID）
    int      code;       // kind==Notify：DMN_* 通知码
    unsigned action;     // kind==Action：DMM_* 消息号
    UINT_PTR reqId;      // 单调序号；应答对账，失配即丢弃
};

// OOPM_DMMRELAYREPLY 载荷（代理 slot 窗 → editor）。
// ★ `delivered` 与 `result` 必须**分开**（批次 112 的教训：判据要落在"对方是否
//   收到"上）：`DMN_CLOSE` 的 veto 语义是"插件把 WM_NOTIFY 的结果置 TRUE ⇒
//   别关"，而"代理没送到"时 result 也是 0 —— 两者同形，只有 delivered 能分开。
struct DmmRelayReplyWire {
    UINT_PTR magic;      // 恒 kMagic
    UINT_PTR msg;        // 恒 OOPM_DMMRELAYREPLY
    UINT_PTR reqId;
    unsigned delivered;  // 1 = 真的发给了本进程的 hClient；0 = 归属验不过
    LONG_PTR result;     // 插件对 WM_NOTIFY 的返回值（kind==Action 时恒 0）
};

// 代理进程退出码约定（编辑器日志 / 不兼容页展示用）：
//   0  正常（收到 OOPM_SHUTDOWN 后退出）
//   2  LoadLibrary 失败（坏 DLL / 非 PE）
//   3  isUnicode() != TRUE（ANSI 插件拒绝，与进程内策略一致）
//   4  缺关键导出（setInfo / getName / getFuncsArray / isUnicode；
//      messageProc 可选，v2.1 起经 OOPM_MSG 桥接）
//   5  启动看门狗超时（setInfo/getFuncsArray 卡死，主动自杀）
//   6  插件代码访问违例（SEH 捕获后退出，绝不让 WER 弹窗拖累后台）
//   其他任意值 = 插件自身调用 exit(n)（如 ComparePlus 的 exit(1)）——
//   编辑器原样记录，这正是进程外隔离的核心价值。
constexpr int kExitClean = 0;
constexpr int kExitLoadFail = 2;
constexpr int kExitAnsi = 3;
constexpr int kExitExport = 4;
constexpr int kExitWatchdog = 5;
constexpr int kExitFault = 6;

} // namespace oop
} // namespace xfs
