// xfsWinPad plugin template - AI workshop scaffold.
// Single-file plugin: implement your feature here in this file.
// Build: build.cmd (fixed pipeline - DO NOT modify it).
// Full guide: ../sdk/PLUGIN_DEV_GUIDE.md
// API header: ../sdk/xfs_plugin_api.h (included below)
#include "xfs_plugin_api.h"
#include <string.h>

static const xfs_plugin_host* g_host = NULL;
static xfs_plugin_command* g_cmd_main = NULL;

static void OnMainCommand(void* user) {
    (void)user;
    if (!g_host) return;
    // TODO(plugin-author): implement the real feature here.
    // Example - insert text at the caret:
    g_host->insertTextAtCaret("[xfsWinPad plugin] hello from template\n");
}

static void RegisterImpl(const xfs_plugin_host* host) {
    g_host = host;
    g_cmd_main = host->addCommand("__PLUGIN_CMD__", "Plugins",
                                  OnMainCommand, NULL);
}

static void UnregisterImpl(void) {
    if (g_host && g_cmd_main) g_host->removeCommand(g_cmd_main);
    g_cmd_main = NULL;
    g_host = NULL;
}

static xfs_plugin_abi g_abi;   // 运行期填充（见 getInfo）

__declspec(dllexport) const xfs_plugin_abi* xfsPlugin_getInfo(void) {
    g_abi.abiVersion    = XFS_PLUGIN_ABI_VERSION;
    g_abi.name          = "__PLUGIN_NAME__";
    g_abi.version       = "0.1.0";
    g_abi.description   = "__PLUGIN_DESC__";
    g_abi.registerPlugin   = &RegisterImpl;
    g_abi.unregisterPlugin = &UnregisterImpl;
    return &g_abi;
}
__declspec(dllexport) void xfsPlugin_register(const xfs_plugin_host* host) {
    RegisterImpl(host);
}
__declspec(dllexport) void xfsPlugin_unregister(void) {
    UnregisterImpl();
}
