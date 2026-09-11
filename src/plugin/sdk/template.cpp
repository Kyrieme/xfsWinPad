// xfsWinPad plugin template (C++) - AI workshop scaffold.
// Use this variant when the feature is easier in C++ (std::string, std::vector).
// Same rules as the C template; exports must stay extern "C".
#include "xfs_plugin_api.h"
#include <string>

static const xfs_plugin_host* g_host = nullptr;
static xfs_plugin_command* g_cmd_main = nullptr;

static void OnMainCommand(void* /*user*/) {
    if (!g_host) return;
    // TODO(plugin-author): implement the real feature.
    g_host->insertTextAtCaret("[xfsWinPad plugin] hello from C++ template\n");
}

extern "C" {

static void RegisterImpl(const xfs_plugin_host* host) {
    g_host = host;
    g_cmd_main = host->addCommand("__PLUGIN_CMD__", "Plugins",
                                  OnMainCommand, nullptr);
}

static void UnregisterImpl(void) {
    if (g_host && g_cmd_main) g_host->removeCommand(g_cmd_main);
    g_cmd_main = nullptr;
    g_host = nullptr;
}

static xfs_plugin_abi g_abi;

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

} // extern "C"
