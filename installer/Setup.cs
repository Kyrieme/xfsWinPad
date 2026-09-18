using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;

// xfsWinPad 单文件自解压安装器
// 应用文件作为嵌入资源打包进本 exe；运行时释放到
// %LOCALAPPDATA%\Programs\xfsWinPad，注册 HKCU 右键菜单
// 「以 xfsWinPad 打开」（文件 + 文件夹）、.xfm 关联、文本/代码文件类型
// 关联（安装时勾选，批次 30）、开始菜单/桌面快捷方式、卸载项与卸载脚本。
// 版本号从释放出的 xfsWinPad.exe 的 FileVersion 读取（单一数据源 = CMake
// project(VERSION) → resources/version.rc.in），不再硬编码。
//
// AI 后端（opencode CLI）随装随下：解压主程序后自动从 GitHub Release
// 下载 opencode-windows-x64.zip（约 60MB）到安装目录。失败（无网/超时）
// 静默跳过，不影响编辑器本体——AI 面板首次打开时仍会给出手动安装指引。
//
// 命令行/环境钩子：
//   setup.exe /silent              无对话框安装，文件关联取默认集（测试/CI 用）
//   XFSWINPAD_SETUP_DEST=<dir>     覆盖安装目标目录（开发测试钩子：跳过
//                                  快捷方式与 opencode 下载，卸载脚本不含 lnk 清理）
class Setup {
    [DllImport("shell32.dll")]
    static extern void SHChangeNotify(int wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);

    // ---- 文件类型关联候选集（批次 30）--------------------------------------
    // 顺序即对话框展示顺序；preChecked 默认集保持克制（纯文本三类）。
    //
    // 【csv 为何不在候选里】(2026-09-18 用户要求)
    //   CSV 交给表格软件（Excel / WPS）打开才是用户的实际习惯；被编辑器接管
    //   会破坏双击打开表格的默认体验。所以连候选都不给，免得手滑勾上。
    //   升级时的旧关联由 UndoLegacyCsvAssociation() 撤销。
    static readonly string[] AssocCandidates = {
        "txt", "log", "md", "tsv", "ini", "cfg", "conf",
        "json", "xml", "yaml", "yml", "toml",
        "cpp", "c", "cc", "h", "hpp", "hxx",
        "py", "js", "ts", "java", "cs", "go", "rs", "rb", "lua",
        "sql", "sh", "bat", "cmd", "ps1", "cmake", "mk",
        "diff", "patch", "asm", "php", "html", "htm", "css",
    };
    static readonly string[] AssocDefault = { "txt", "log", "md" };
    const string DocProgId = "xfsWinPad.Document";

    static int Main(string[] args) {
        bool silent = false;
        foreach (string a in args)
            if (a.Equals("/silent", StringComparison.OrdinalIgnoreCase)) silent = true;
        try {
            string dest = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Programs", "xfsWinPad");
            // 开发/测试钩子：安装到指定目录（跳过快捷方式与 opencode 下载，
            // 卸载脚本不含 lnk 清理——避免动到真实安装的产物）
            string destOverride = Environment.GetEnvironmentVariable("XFSWINPAD_SETUP_DEST");
            bool testMode = !string.IsNullOrEmpty(destOverride);
            if (testMode) dest = destOverride;

            // ---- 安装前检测：运行中的 xfsWinPad / 插件宿主会锁住 exe/dll ----
            // 检测方式 = 枚举本用户进程名；命中则提示，用户关闭后可重试。
            string[] running = Array.FindAll(
                new string[] { "xfsWinPad", "xfsWinPadPluginHost" },
                name => System.Diagnostics.Process.GetProcessesByName(name).Length > 0);
            if (running.Length > 0) {
                if (silent) return 1;   // 无头模式：直接放弃，不弹窗
                DialogResult r = MessageBox.Show(
                    "检测到正在运行的 xfsWinPad（编辑器或插件宿主进程）。\n\n" +
                    "文件被占用会导致更新不完整。请先保存工作并退出 xfsWinPad，" +
                    "然后点「重试」继续安装；点「取消」中止安装。",
                    "xfsWinPad 安装", MessageBoxButtons.RetryCancel, MessageBoxIcon.Warning);
                if (r != DialogResult.Retry) return 1;
                // 再给一次机会检测（用户可能已关闭）
                running = Array.FindAll(running,
                    name => System.Diagnostics.Process.GetProcessesByName(name).Length > 0);
                if (running.Length > 0)
                    throw new Exception("xfsWinPad 仍在运行（" +
                        string.Join(", ", running) + "），已中止。请退出后重新运行安装器。");
            }

            Directory.CreateDirectory(dest);
            string langDir = Path.Combine(dest, "lang");
            Directory.CreateDirectory(langDir);

            string[] binFiles = {
                "xfsWinPad.exe", "xfsWinPadPluginHost.exe",
                "Scintilla.dll", "Lexilla.dll",
                "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                "app.ico", "README.md", "THIRD_PARTY_NOTICES.md",
                "xtaclean.exe",
            };

            Assembly asm = Assembly.GetExecutingAssembly();

            // ---- UI language packs -------------------------------------------------
            // I18n::Load resolves lang\<code>.json next to the exe, so every
            // embedded *.json must land in lang\ (the exe itself does not embed
            // the dictionaries -- verified: the new "sb.model.note" key appears
            // 0 times in xfsWinPad.exe, 5 times in setup.exe).
            //
            // This list is DERIVED from the manifest on purpose. It used to be a
            // hardcoded `{ "en.json", "zh-CN.json" }` written on 2026-09-02; batch
            // 40 (7a157aa) then added zh-TW / ja / ko, and nothing updated the
            // array -- so from batch 40 onward every setup.exe shipped 2 of the 5
            // languages while dist\xfsWinPad-portable.zip shipped all 5. Symptom
            // was silent: switching to 繁體中文/日本語/한국어 in an installed copy
            // just failed (I18n::Load returns false) with no error dialog.
            // Enumerating the resources keeps the installer in lockstep with
            // make-payload.ps1, which already picks up lang\*.json by wildcard.
            foreach (string res in asm.GetManifestResourceNames()) {
                if (!res.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                    continue;
                Extract(asm, res, Path.Combine(langDir, res));
            }
            foreach (string f in binFiles) Extract(asm, f, Path.Combine(dest, f));

            string exe = Path.Combine(dest, "xfsWinPad.exe");
            string icon = "\"" + exe + "\",0";

            // ---- 右键菜单 (HKCU, 免管理员)：以 xfsWinPad 打开 ----
            using (RegistryKey shellKey = Registry.CurrentUser.CreateSubKey(
                       @"Software\Classes\*\shell\OpenWithxfsWinPad")) {
                shellKey.SetValue("", "以 xfsWinPad 打开");
                shellKey.SetValue("Icon", icon);
                using (RegistryKey cmdKey = shellKey.CreateSubKey("command"))
                    cmdKey.SetValue("", "\"" + exe + "\" \"%1\"");
            }

            // ---- 右键文件夹 → 作为项目工作区打开（批次 30）----
            // 应用已支持目录参数：OpenCliFiles 里 is_directory → SetProjectRoot
            using (RegistryKey shellKey = Registry.CurrentUser.CreateSubKey(
                       @"Software\Classes\Directory\shell\OpenWithxfsWinPad")) {
                shellKey.SetValue("", "以 xfsWinPad 打开");
                shellKey.SetValue("Icon", icon);
                using (RegistryKey cmdKey = shellKey.CreateSubKey("command"))
                    cmdKey.SetValue("", "\"" + exe + "\" \"%1\"");
            }

            // ---- .xfm 文件关联（双击直达宏加载）----
            using (RegistryKey clsKey = Registry.CurrentUser.CreateSubKey(
                       @"Software\Classes\.xfm")) {
                clsKey.SetValue("", "xfsWinPad.Macro");
            }
            using (RegistryKey appKey = Registry.CurrentUser.CreateSubKey(
                       @"Software\Classes\xfsWinPad.Macro")) {
                appKey.SetValue("", "xfsWinPad 宏");
                using (RegistryKey defIcon = appKey.CreateSubKey("DefaultIcon"))
                    defIcon.SetValue("", icon);
                using (RegistryKey shellKey = appKey.CreateSubKey("shell"))
                    using (RegistryKey openKey = shellKey.CreateSubKey("open"))
                        using (RegistryKey cmdKey = openKey.CreateSubKey("command"))
                            cmdKey.SetValue("", "\"" + exe + "\" \"%1\"");
            }
            // .xfm 同样需要 UserChoice（Win11 实测无它即弹「选择打开方式」）
            SFTA.SetFileAssociation("xfsWinPad.Macro", ".xfm");

            // ---- 文本/代码文件类型关联（批次 30）----
            // 交互模式弹勾选框（用户点头才算关联）；/silent 取默认集；
            // 「跳过」= 一个都不关联，安装继续。
            List<string> assoc = new List<string>();
            if (silent) {
                assoc.AddRange(AssocDefault);
            } else {
                using (AssocDialog dlg = new AssocDialog(AssocCandidates, AssocDefault))
                    if (dlg.ShowDialog() == DialogResult.OK)
                        assoc.AddRange(dlg.Chosen);
            }
            if (assoc.Count > 0)
                AssociateExtensions(exe, assoc);

            // 撤销旧版本写入的 .csv 关联（本版本起 csv 不再关联，见候选集注释）。
            // 必须放在关联写入之后：即便用户这次又勾了 csv（旧对话框里勾过），
            // 也以「不关联 csv」为准。
            UndoLegacyCsvAssociation();

            // ---- 卸载项 (HKCU) ----
            // 版本从 exe 的 FileVersion 读（CMake project(VERSION) 单一数据源）
            string version = "0.0.0";
            try {
                version = FileVersionInfo.GetVersionInfo(exe).FileVersion;
                if (string.IsNullOrEmpty(version)) version = "0.0.0";
            } catch { /* 读取失败兜底，不阻断安装 */ }
            using (RegistryKey unKey = Registry.CurrentUser.CreateSubKey(
                        @"Software\Microsoft\Windows\CurrentVersion\Uninstall\xfsWinPad")) {
                unKey.SetValue("DisplayName", "xfsWinPad");
                unKey.SetValue("DisplayIcon", icon);
                unKey.SetValue("UninstallString", "\"" + Path.Combine(dest, "uninstall.cmd") + "\"");
                unKey.SetValue("InstallLocation", dest);
                unKey.SetValue("DisplayVersion", version);
                unKey.SetValue("Publisher", "xfsWinPad Project");
                unKey.SetValue("NoModify", 1, RegistryValueKind.DWord);
                unKey.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }

            // ---- 卸载脚本 ----
            // 文件类型关联的清理（批次 30）只删「默认值」与 OpenWithProgids
            // 里的我们的项——不动 .ext 键的其余数据（其他程序的关联信息保留）。
            var unLines = new System.Collections.Generic.List<string> {
                "@echo off",
                "rem xfsWinPad uninstaller",
                "reg delete \"HKCU\\Software\\Classes\\*\\shell\\OpenWithxfsWinPad\" /f >nul 2>&1",
                "reg delete \"HKCU\\Software\\Classes\\Directory\\shell\\OpenWithxfsWinPad\" /f >nul 2>&1",
                "reg delete \"HKCU\\Software\\Classes\\.xfm\" /f >nul 2>&1",
                "reg delete \"HKCU\\Software\\Classes\\xfsWinPad.Macro\" /f >nul 2>&1",
                "reg delete \"HKCU\\Software\\Classes\\" + DocProgId + "\" /f >nul 2>&1",
            };
            foreach (string ext in assoc) {
                unLines.Add("reg delete \"HKCU\\Software\\Classes\\." + ext + "\" /ve /f >nul 2>&1");
                unLines.Add("reg delete \"HKCU\\Software\\Classes\\." + ext +
                            "\\OpenWithProgids\" /v " + DocProgId + " /f >nul 2>&1");
            }
            // FileExts\UserChoice 带 Deny-SetValue ACE，reg.exe 删不动——用
            // 助手走 RegDeleteKey 路径整树删除（.xfm 恒有，assoc 视勾选而定）
            string ucArgs = "xfm" + (assoc.Count > 0 ? " " + string.Join(" ", assoc) : "");
            unLines.Add("\"%~dp0xtaclean.exe\" " + ucArgs);
            unLines.Add("reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\xfsWinPad\" /f >nul 2>&1");
            if (!testMode) {
                unLines.Add("del /q \"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\xfsWinPad.lnk\" >nul 2>&1");
                unLines.Add("del /q \"%USERPROFILE%\\Desktop\\xfsWinPad.lnk\" >nul 2>&1");
            }
            unLines.Add("rmdir /s /q \"" + dest + "\" >nul 2>&1");
            unLines.Add("echo xfsWinPad uninstalled.");
            File.WriteAllText(Path.Combine(dest, "uninstall.cmd"),
                              string.Join("\r\n", unLines) + "\r\n");

            // ---- 快捷方式 ----
            // 注意：IconLocation 不能带引号（WScript.Shell 会把引号当作
            // 文件名的一部分导致解析失败 → 白色默认图标）；
            // 注册表的 Icon 值才需要引号，两处写法不同。
            string lnkIcon = exe + ",0";
            if (!testMode) {
                CreateShortcut(Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.Programs),
                    "xfsWinPad.lnk"), exe, dest, lnkIcon);
                string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                if (Directory.Exists(desktop))
                    CreateShortcut(Path.Combine(desktop, "xfsWinPad.lnk"), exe, dest, lnkIcon);
            }

            // 通知 Explorer 刷新图标/快捷方式缓存
            SHChangeNotify(0x08000000 /*SHCNE_ASSOCCHANGED*/, 0x0000, IntPtr.Zero, IntPtr.Zero);

            // ---- AI 后端：自动下载 opencode CLI（serve 模式驱动用）----
            // 目标 <dest>\bin\opencode.exe——FindOpencodeExe 的 exe 同目录
            // 补充查找路径之一。已存在（升级安装）则跳过。
            // 测试模式跳过（60MB 下载不该在 CI/联调里发生）。
            if (!testMode) InstallOpencode(dest);

            string aiNote = File.Exists(Path.Combine(dest, "bin", "opencode.exe"))
                ? "\nAI 助手（opencode CLI）已就绪。"
                : "\n注意：AI 助手组件未能自动下载（可能无网络），" +
                  "不影响编辑器使用；联网后可在 AI 面板中按指引安装。";

            string assocNote = assoc.Count > 0
                ? "\n已关联 " + assoc.Count + " 种文件类型（" +
                  string.Join(" ", assoc.ConvertAll(e => "." + e).ToArray()) +
                  "）；重跑安装器可重新选择。"
                : "\n未关联文件类型（可重跑安装器选择）。";

            if (!silent)
                MessageBox.Show(
                    "xfsWinPad v" + version + " 已安装到：\n" + dest +
                    "\n\n已注册右键菜单「以 xfsWinPad 打开」（文件与文件夹）。\n" +
                    "已关联 .xfm 宏文件（双击 = 在 xfsWinPad 中加载宏）。" +
                    assocNote +
                    "\n可在开始菜单/桌面找到 xfsWinPad 快捷方式。" + aiNote,
                    "xfsWinPad 安装", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 0;
        } catch (Exception ex) {
            try {   // 静默模式无处显示，落盘便于排查
                File.WriteAllText(Path.Combine(Path.GetTempPath(),
                    "xfsWinPad-setup-error.log"), ex.ToString());
            } catch { }
            if (!silent)
                MessageBox.Show("安装失败：" + ex.Message, "xfsWinPad",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 2;
        }
    }

    // ---- 文件类型关联写注册表（批次 30）------------------------------------
    // 共享 ProgID xfsWinPad.Document；每个扩展名：
    //   .ext 默认值 = ProgID + OpenWithProgids（"打开方式"可见）
    //   UserChoice（Hash+ProgId）= SFTA 算法写入（批次 30a 修订）：Win10/11
    //   实测无 UserChoice 一律弹「选择打开方式」（Classes 默认值被无视，
    //   .md/.ztest/.xfm 全部实锤），Hash 算法来自 PS-SFTA (DanysysTeam, MIT)。
    //   RegDeleteKey 低权限路径可删除 ACL 保护的旧 UserChoice（WPS/AppX 实锤）。
    static void AssociateExtensions(string exe, IEnumerable<string> exts) {
        string icon = "\"" + exe + "\",0";
        using (RegistryKey appKey = Registry.CurrentUser.CreateSubKey(
                   @"Software\Classes\" + DocProgId)) {
            appKey.SetValue("", "xfsWinPad 文档");
            using (RegistryKey defIcon = appKey.CreateSubKey("DefaultIcon"))
                defIcon.SetValue("", icon);
            using (RegistryKey shellKey = appKey.CreateSubKey("shell"))
                using (RegistryKey openKey = shellKey.CreateSubKey("open"))
                    using (RegistryKey cmdKey = openKey.CreateSubKey("command"))
                        cmdKey.SetValue("", "\"" + exe + "\" \"%1\"");
        }
        foreach (string ext in exts) {
            using (RegistryKey extKey = Registry.CurrentUser.CreateSubKey(
                       @"Software\Classes\." + ext))
                extKey.SetValue("", DocProgId);
            using (RegistryKey owKey = Registry.CurrentUser.CreateSubKey(
                       @"Software\Classes\." + ext + @"\OpenWithProgids"))
                owKey.SetValue(DocProgId, "", RegistryValueKind.String);
            SFTA.SetFileAssociation(DocProgId, "." + ext);
        }
    }

    // ---- 撤销旧版本的 .csv 关联（2026-09-18）-------------------------------
    // 【为什么必须有这一步，光从候选集删掉不够】
    //   关联是写进注册表的**持久状态**，不是安装包里的清单。老版本（批次 30 起）
    //   把 .csv 放进默认集，凡是装过的人，`.csv` 的默认值 / OpenWithProgids /
    //   UserChoice 三处都指向 xfsWinPad。新版即使不再关联，也**不会自动消失**——
    //   用户升级后照样双击 CSV 打开编辑器，然后回来说"没修好"。
    //
    // 【只撤我们写的那一份，别动别人的】
    //   用户可能早已把 CSV 交回 Excel / WPS。三处逐项验证归属：
    //     Classes\.csv 默认值 == xfsWinPad.Document → 清空这个值（**不删整键**，
    //                                                  别人写在键里的数据保留）
    //     Classes\.csv\OpenWithProgids\DocProgId    → 删这个值
    //     FileExts\.csv\UserChoice\ProgId == 我们    → 整树删（把选择权还给系统：
    //                                                  落到 Excel/WPS 或弹一次
    //                                                  「选择打开方式」）
    //   指向别的程序时一个字都不动——删了会强行打掉用户的既有选择。
    //   整树删除走 SFTA 的 RegDeleteKey 低权限路径（Explorer 给 UserChoice 加的
    //   Deny-SetValue ACE 挡不住 DELETE 权限，见 SFTA 注释）。
    static void UndoLegacyCsvAssociation() {
        const string ext = ".csv";
        try {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(
                       @"Software\Classes\" + ext, true)) {
                if (k != null) {
                    object dv = k.GetValue("");
                    if (dv is string && (string)dv == DocProgId)
                        k.DeleteValue("", false);
                    using (RegistryKey ow = k.OpenSubKey("OpenWithProgids", true))
                        if (ow != null && ow.GetValue(DocProgId) != null)
                            ow.DeleteValue(DocProgId, false);
                }
            }
        } catch { /* 权限/竞态：留着也不影响新装行为 */ }
        try {
            string basePath = @"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\" + ext;
            bool ours = false;
            using (RegistryKey uc = Registry.CurrentUser.OpenSubKey(basePath + @"\UserChoice"))
                if (uc != null) {
                    object pid = uc.GetValue("ProgId");
                    ours = pid is string && (string)pid == DocProgId;
                }
            if (ours) SFTA.DeleteFileExtsTree(ext);
        } catch { }
    }

    static void Extract(Assembly asm, string resName, string outPath) {
        using (Stream s = asm.GetManifestResourceStream(resName)) {
            if (s == null) throw new Exception("缺少嵌入资源: " + resName);
            using (FileStream fs = new FileStream(outPath, FileMode.Create, FileAccess.Write))
                s.CopyTo(fs);
        }
    }

    // ---- opencode CLI 下载（AI 后端，失败静默跳过）-------------------------
    const string OcUrl = "https://github.com/sst/opencode/releases/latest/download/opencode-windows-x64.zip";

    static void InstallOpencode(string dest) {
        try {
            string binDir = Path.Combine(dest, "bin");
            string target = Path.Combine(binDir, "opencode.exe");
            if (File.Exists(target)) return;   // 升级安装：已有则不重复下载

            Directory.CreateDirectory(binDir);
            string zip = Path.Combine(Path.GetTempPath(),
                "xfsWinPad-opencode-" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".zip");

            // 下载（60MB 级，超时放宽；进度对话框让用户知道在做什么）
            using (var prog = new ProgressForm("正在下载 AI 助手组件（opencode CLI）…",
                                               "下载中…")) {
                ServicePointManager.SecurityProtocol =
                    SecurityProtocolType.Tls11 | SecurityProtocolType.Tls12;
                var req = (HttpWebRequest)WebRequest.Create(OcUrl);
                req.AllowAutoRedirect = true;
                req.Timeout = 30000;               // 连接超时
                req.ReadWriteTimeout = 120000;     // 单次读超时
                using (var resp = req.GetResponse())
                using (var net = resp.GetResponseStream())
                using (var fs = new FileStream(zip, FileMode.Create, FileAccess.Write)) {
                    long total = resp.ContentLength;
                    var buf = new byte[65536];
                    long done = 0; int n;
                    while ((n = net.Read(buf, 0, buf.Length)) > 0) {
                        fs.Write(buf, 0, n);
                        done += n;
                        prog.Update(total > 0 ? (int)(done * 100 / total) : 0);
                    }
                }
            }

            // 解压 opencode.exe（zip 根下单文件）到 <dest>\bin\
            using (var za = ZipFile.OpenRead(zip)) {
                foreach (ZipArchiveEntry e in za.Entries) {
                    if (!e.FullName.EndsWith("opencode.exe",
                            StringComparison.OrdinalIgnoreCase)) continue;
                    using (Stream es = e.Open())
                    using (var fs = new FileStream(target, FileMode.Create,
                                                   FileAccess.Write))
                        es.CopyTo(fs);
                    break;
                }
            }
            try { File.Delete(zip); } catch { }
            if (!File.Exists(target)) return;   // zip 里没有预期文件
            // 冒烟验证：能启动 --version 才算成功；失败删掉避免留下坏文件
            var psi = new ProcessStartInfo(target, "--version") {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            using (var p = Process.Start(psi)) {
                string ver = p.StandardOutput.ReadToEnd();
                p.WaitForExit(10000);
                if (p.ExitCode != 0 || string.IsNullOrWhiteSpace(ver)) {
                    try { File.Delete(target); } catch { }
                }
            }
        } catch {
            // 无网/代理拦截/磁盘问题——AI 是增强功能，不阻断安装主流程
            try {
                string bad = Path.Combine(dest, "bin", "opencode.exe");
                if (File.Exists(bad)) File.Delete(bad);
            } catch { }
        }
    }

    static void CreateShortcut(string lnk, string target, string workDir, string icon) {
        try {
            Type t = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(t);
            object sc = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod,
                                       null, shell, new object[] { lnk });
            Type st = sc.GetType();
            st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { target });
            st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { workDir });
            st.InvokeMember("IconLocation", BindingFlags.SetProperty, null, sc, new object[] { icon });
            st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
        } catch {
            // 快捷方式非关键，失败不阻断安装
        }
    }
}

// ---- UserChoice（Hash）写入 —— SFTA 算法 ----------------------------------
// Win10/11 Explorer 的实际行为（本机 Win11 23H2 实测）：无 UserChoice 一律
// 弹「选择打开方式」，Classes 默认值被无视（.md/.ztest/.xfm 全部实锤）。
// UserChoice 带 Hash 防篡改校验，算法自 PS-SFTA (DanysysTeam/PS-SFTA, MIT)
// 移植；其清旧键走 RegDeleteKey 低权限路径——Explorer/WPS 创建的 ACL 保护
// 键用 reg.exe 删除报「拒绝访问」，此路径实测可行；删除后重建键即可写入。
// 另写 ApplicationAssociationToasts=0 压制「有新应用可打开」提示（正好治
// IDE 留下的 HKLM 候选残留，如本机的 QoderCN.md）。
static class SFTA {
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint RegDeleteKey(UIntPtr hKey, string subKey);
    static readonly UIntPtr HKCU = new UIntPtr(0x80000001u);

    public static void SetFileAssociation(string progId, string ext) {
        WriteToastSuppressions(progId, ext);
        string hash = ComputeHash(progId, ext);
        string keyPath = @"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\" +
                         ext + @"\UserChoice";
        DeleteFileExtsTree(ext);
        try {
            // 关键：写完值后必须补上「当前用户 SetValue Deny」显式 ACE——
            // Explorer 写 UserChoice 时会加这条保护（本机 .json 实锤），shell
            // 以它的存在鉴别键是否经正当流程写入；缺失则哈希再对也判篡改
            // （拒收 + 删键 + 弹「选择打开方式」）。RegDeleteKey 用的 DELETE
            // 权限不受该 Deny 影响，升级安装仍可整键删除重建。
            using (RegistryKey uc = Registry.CurrentUser.CreateSubKey(keyPath)) {
                uc.SetValue("Hash", hash);
                uc.SetValue("ProgId", progId);
                System.Security.AccessControl.RegistrySecurity sec =
                    uc.GetAccessControl();
                sec.AddAccessRule(new System.Security.AccessControl.RegistryAccessRule(
                    System.Security.Principal.WindowsIdentity.GetCurrent().User,
                    System.Security.AccessControl.RegistryRights.SetValue,
                    System.Security.AccessControl.InheritanceFlags.None,
                    System.Security.AccessControl.PropagationFlags.None,
                    System.Security.AccessControl.AccessControlType.Deny));
                uc.SetAccessControl(sec);
            }
        } catch { /* ACL 保护：放弃 UserChoice，不阻断安装 */ }
    }

    // 整树删除 FileExts\.ext（含 UserChoice 与 OpenWithList/Progids 历史）。
    // 只删 UserChoice 不够：旧选择是打包应用（AppX，如 Win11 记事本默认接管
    // .log）时，残留历史会让 Explorer 持续弹「选择打开方式」（本机 .log 实锤；
    // 整树删除后重建即正常）。
    // 顺序关键：先单删 Deny-SetValue ACE 保护的 UserChoice 子键（DELETE 权限
    // 不受影响），再删整树——否则整树递归打开该子键请求 SetValue 被拒 →
    // ACCESS_DENIED。
    // 也用于「撤销旧关联」：删完不重建，选择权就落回系统默认/其他程序。
    public static void DeleteFileExtsTree(string ext) {
        try {
            RegDeleteKey(HKCU, @"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\" +
                               ext + @"\UserChoice");
            RegDeleteKey(HKCU, @"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\" + ext);
        } catch { /* 无该键 = 本来就没有 */ }
    }

    // 把本 ProgID 与系统侧全部既有候选都记为「已提示过」（DWORD 0）
    static void WriteToastSuppressions(string progId, string ext) {
        const string toastPath = @"HKEY_CURRENT_USER\Software\Microsoft\Windows" +
                                 @"\CurrentVersion\ApplicationAssociationToasts";
        Action<string> mark = name => {
            if (name.Length == 0) return;
            try {
                Microsoft.Win32.Registry.SetValue(toastPath, name, 0,
                                                  RegistryValueKind.DWord);
            } catch { }
        };
        mark(progId + "_" + ext);
        try {   // HKLM OpenWithList 的 exe 名
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(
                       @"Software\Classes\" + ext + @"\OpenWithList"))
                if (k != null)
                    foreach (string sub in k.GetSubKeyNames())
                        mark("Applications\\" + sub + "_" + ext);
        } catch { }
        try {   // HKLM OpenWithProgids 的候选 ProgID（残留源）
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(
                       @"Software\Classes\" + ext + @"\OpenWithProgids"))
                if (k != null)
                    foreach (string name in k.GetValueNames())
                        mark(name + "_" + ext);
        } catch { }
        try {   // 浏览器等注册的能力声明
            string[] roots = {
                @"Software\Clients\StartMenuInternet",
                @"Software\Wow6432Node\Clients\StartMenuInternet",
            };
            RegistryKey[] hives = { Registry.LocalMachine, Registry.CurrentUser };
            foreach (RegistryKey hive in hives)
                foreach (string r in roots)
                    using (RegistryKey clients = hive.OpenSubKey(r)) {
                        if (clients == null) continue;
                        foreach (string client in clients.GetSubKeyNames())
                            using (RegistryKey caps = clients.OpenSubKey(
                                       client + @"\Capabilities\FileAssociations")) {
                                if (caps == null) continue;
                                object v = caps.GetValue(ext);
                                if (v is string) mark((string)v + "_" + ext);
                            }
                    }
        } catch { }
    }

    static string ComputeHash(string progId, string ext) {
        // 直接取当前令牌 SID（PS-SFTA 的 NTAccount→SID 翻译在本机实测会抛
        // IdentityNotMappedException；令牌 SID 才是 Explorer 哈希所用的身份）
        string sid = System.Security.Principal.WindowsIdentity.GetCurrent()
                         .User.Value.ToLowerInvariant();
        // Win10/11 固定串（含 GUID）；PS-SFTA 会从 Shell32.dll 里搜出来，
        // 实测各版本一致，直接用硬编码兜底值
        const string userExperience = "User Choice set via Windows User Experience " +
            "{D18B6DD5-6124-4341-9318-804003BAFA0B}";
        string baseInfo = (ext + sid + progId + GetHexDateTime() + userExperience)
                              .ToLowerInvariant();
        return GetHash(baseInfo);
    }

    // FILETIME 到分钟精度，高低 32 位各 8 位十六进制（同 PS-SFTA）
    static string GetHexDateTime() {
        DateTime now = DateTime.Now;
        DateTime minute = new DateTime(now.Year, now.Month, now.Day,
                                       now.Hour, now.Minute, 0);
        long ft = minute.ToFileTime();
        return (((uint)(ft >> 32)).ToString("X8") +
                ((uint)(ft & 0xFFFFFFFFL)).ToString("X8")).ToLowerInvariant();
    }

    static int GetLong(byte[] b, int i) { return BitConverter.ToInt32(b, i); }

    // 32 位算术右移 + 高位段的 0xFFFF0000 异或（原算法行为，勿改）
    static long Shr(long v, int c) {
        return (v & 0x80000000L) != 0 ? ((v >> c) ^ 0xFFFF0000L) : (v >> c);
    }

    // PS-SFTA Get-Hash：MD5 种子 + 两轮混淆，输出 8 字节 base64
    static string GetHash(string baseInfo) {
        byte[] src = System.Text.Encoding.Unicode.GetBytes(baseInfo);
        byte[] baseBytes = new byte[src.Length + 2];   // 补 0x00,0x00
        Array.Copy(src, baseBytes, src.Length);
        byte[] md5;
        using (var m = System.Security.Cryptography.MD5.Create())
            md5 = m.ComputeHash(baseBytes);

        int lengthBase = baseInfo.Length * 2 + 2;
        int length = ((lengthBase & 4) == 0 ? 1 : 0) + (lengthBase >> 2) - 1;
        if (length <= 1) return "";

        byte[] outHash = new byte[16];
        // ---- 第一轮 ----
        {
            long md51 = (long)(GetLong(md5, 0) | 1) + 0x69FB0000L;
            long md52 = (long)(GetLong(md5, 4) | 1) + 0x13DB0000L;
            long outHash1 = 0, cache = 0;
            long counter = (long)Shr(length - 2, 1) + 1;
            long pdata = 0;
            while (counter != 0) {
                long r0 = (long)(int)(GetLong(baseBytes, (int)pdata) + outHash1);
                long r1 = (long)(int)GetLong(baseBytes, (int)pdata + 4);
                pdata += 8;
                long r2a = (long)(int)((r0 * md51) - (0x10FA9605L * Shr(r0, 16)));
                long r2b = (long)(int)((0x79F8A395L * r2a) + (0x689B6B9FL * Shr(r2a, 16)));
                long r3 = (long)(int)((0xEA970001L * r2b) - (0x3C101569L * Shr(r2b, 16)));
                long r4 = (long)(int)(r3 + r1);
                long r5 = (long)(int)(cache + r3);
                long r6a = (long)(int)((r4 * md52) - (0x3CE8EC25L * Shr(r4, 16)));
                long r6b = (long)(int)((0x59C3AF2DL * r6a) - (0x2232E0F1L * Shr(r6a, 16)));
                outHash1 = (long)(int)((0x1EC90001L * r6b) + (0x35BD1EC9L * Shr(r6b, 16)));
                cache = (long)(int)(r5 + outHash1);
                counter--;
            }
            Array.Copy(BitConverter.GetBytes((int)outHash1), 0, outHash, 0, 4);
            Array.Copy(BitConverter.GetBytes((int)cache), 0, outHash, 4, 4);
        }
        // ---- 第二轮 ----
        {
            long md51 = (long)(GetLong(md5, 0) | 1);
            long md52 = (long)(GetLong(md5, 4) | 1);
            long outHash1 = 0, cache = 0;
            long counter = (long)Shr(length - 2, 1) + 1;
            long pdata = 0;
            while (counter != 0) {
                long r0 = (long)(int)(GetLong(baseBytes, (int)pdata) + outHash1);
                pdata += 8;
                long r1a = (long)(int)(r0 * md51);
                long r1b = (long)(int)((0xB1110000L * r1a) - (0x30674EEFL * Shr(r1a, 16)));
                long r2a = (long)(int)((0x5B9F0000L * r1b) - (0x78F7A461L * Shr(r1b, 16)));
                long r2b = (long)(int)((0x12CEB96DL * Shr(r2a, 16)) - (0x46930000L * r2a));
                long r3 = (long)(int)((0x1D830000L * r2b) + (0x257E1D83L * Shr(r2b, 16)));
                long r4 = (long)(int)(md52 * (long)((long)r3 + GetLong(baseBytes, (int)pdata - 4)));
                long r4b = (long)(int)((0x16F50000L * r4) - (0x5D8BE90BL * Shr(r4, 16)));
                long r5a = (long)(int)((0x96FF0000L * r4b) - (0x2C7C6901L * Shr(r4b, 16)));
                long r5b = (long)(int)((0x2B890000L * r5a) + (0x7C932B89L * Shr(r5a, 16)));
                outHash1 = (long)(int)((0x9F690000L * r5b) - (0x405B6097L * Shr(r5b, 16)));
                cache = (long)(int)((long)outHash1 + cache + r3);
                counter--;
            }
            Array.Copy(BitConverter.GetBytes((int)outHash1), 0, outHash, 8, 4);
            Array.Copy(BitConverter.GetBytes((int)cache), 0, outHash, 12, 4);
        }
        int h1 = GetLong(outHash, 8) ^ GetLong(outHash, 0);
        int h2 = GetLong(outHash, 12) ^ GetLong(outHash, 4);
        byte[] result = new byte[8];
        Array.Copy(BitConverter.GetBytes(h1), 0, result, 0, 4);
        Array.Copy(BitConverter.GetBytes(h2), 0, result, 4, 4);
        return Convert.ToBase64String(result);
    }
}

// 批次 30：文件类型关联勾选框（确定 = 关联勾选项；跳过 = 一个不关联）
class AssocDialog : Form {
    public readonly List<string> Chosen = new List<string>();

    public AssocDialog(string[] exts, string[] preChecked) {
        Text = "xfsWinPad 安装 - 文件关联";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterScreen;
        MaximizeBox = MinimizeBox = false;
        Size = new System.Drawing.Size(540, 480);

        var label = new Label {
            Text = "选择要用 xfsWinPad 打开的文件类型（设为当前用户的默认打开方式）：",
            Left = 12, Top = 10, Width = 500, Height = 22 };
        var list = new CheckedListBox {
            Left = 12, Top = 36, Width = 500, Height = 340,
            CheckOnClick = true, MultiColumn = true, ColumnWidth = 92 };
        foreach (string e in exts) {
            int i = list.Items.Add("." + e);
            if (Array.IndexOf(preChecked, e) >= 0) list.SetItemChecked(i, true);
        }
        var btnAll = new Button { Text = "全选", Left = 12, Top = 388, Width = 72 };
        var btnNone = new Button { Text = "全不选", Left = 92, Top = 388, Width = 72 };
        var btnOk = new Button { Text = "确定", Left = 348, Top = 388, Width = 76 };
        var btnSkip = new Button { Text = "跳过", Left = 432, Top = 388, Width = 76 };
        btnAll.Click += (s, e) => {
            for (int i = 0; i < list.Items.Count; i++) list.SetItemChecked(i, true); };
        btnNone.Click += (s, e) => {
            for (int i = 0; i < list.Items.Count; i++) list.SetItemChecked(i, false); };
        btnOk.Click += (s, e) => {
            foreach (object it in list.CheckedItems) Chosen.Add(it.ToString().Substring(1));
            DialogResult = DialogResult.OK; Close(); };
        btnSkip.Click += (s, e) => { DialogResult = DialogResult.Cancel; Close(); };
        Controls.Add(label);
        Controls.Add(list);
        Controls.Add(btnAll); Controls.Add(btnNone);
        Controls.Add(btnOk); Controls.Add(btnSkip);
        AcceptButton = btnOk;
        CancelButton = btnSkip;
    }
}

// 极简进度窗（模态；后台下载在 UI 线程分块驱动消息泵——安装器场景够用）
class ProgressForm : Form, IDisposable {
    readonly ProgressBar bar;
    readonly Label label;
    readonly string title;

    public ProgressForm(string title, string phase) {
        this.title = title;
        Text = "xfsWinPad 安装";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        ControlBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        Size = new System.Drawing.Size(420, 130);
        label = new Label {
            Text = title, Left = 12, Top = 12, Width = 380, Height = 22 };
        bar = new ProgressBar {
            Left = 12, Top = 44, Width = 380, Height = 22, Minimum = 0, Maximum = 100 };
        Controls.Add(label);
        Controls.Add(bar);
        Show();
        Application.DoEvents();
    }

    public void Update(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        bar.Value = percent;
        label.Text = title + "  " + percent + "%";
        Application.DoEvents();   // 保持窗口响应
    }
}
