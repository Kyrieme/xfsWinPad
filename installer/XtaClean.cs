using System;
using System.Runtime.InteropServices;

// xfsWinPad 卸载助手（随安装器释放，uninstall.cmd 调用后随目录删除）：
// 删除 FileExts\.<ext> 整树。安装器写入的 UserChoice 带「当前用户
// Deny-SetValue」显式 ACE（Explorer 用它鉴别键经正当流程写入），reg.exe
// 删除时因请求了 SetValue 权限报「拒绝访问」；本助手走 advapi32
// RegDeleteKey（仅 DELETE 路径）即可删除。
class XtaClean {
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint RegDeleteKey(UIntPtr hKey, string subKey);

    static int Main(string[] args) {
        int rcSum = 0;
        foreach (string a in args) {
            string ext = a.StartsWith(".") ? a : "." + a;
            string baseKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext;
            // 必须先删受 Deny-SetValue ACE 保护的 UserChoice 子键：
            // RegDeleteKey 递归打开子键时请求 SetValue 权限会被 Deny 挡住
            // （整树一次删 = ACCESS_DENIED，本机实测）；单删该子键走 DELETE
            // 权限则畅通。子键删除后再删整树即可。
            uint rc1 = RegDeleteKey((UIntPtr)0x80000001u, baseKey + "\\UserChoice");
            uint rc2 = 0;
            if (rc1 == 0) rc2 = RegDeleteKey((UIntPtr)0x80000001u, baseKey);
            Console.WriteLine(ext + " uc=" + rc1 + " tree=" + rc2);
            if (rc1 != 0 && rc1 != 2) rcSum = 1;
        }
        return rcSum;
    }
}
