// test_plugin_core.cpp - a tiny plugin DLL used by the plugin manager unit test.
// Exposes xfsPlugin_getInfo + registers one command whose callback flips a
// global counter the test can observe via the exported query function. The
// callback also exercises the v2 editor/document host callbacks (graceful
// no-workspace degradation asserted exactly via lastFlags bitmap) and v3
// event hooks: two hooks with different masks record everything they receive
// into per-hook arrays for mask-filtering / mid-dispatch removal assertions.
#include "xfs_plugin_api.h"
#include <windows.h>

static const xfs_plugin_host* g_host = nullptr;
static int g_calls = 0;
static unsigned g_flags = 0;

// ---- v3 event-hook capture -------------------------------------------------
static int g_hookAllId = 0;
static int g_hookNarrowId = 0;
#define KMAX_EV 16
struct EvRec { uint32_t type; char arg0; };
static EvRec g_broad[KMAX_EV];  static int g_broadN = 0;
static EvRec g_narrow[KMAX_EV]; static int g_narrowN = 0;

extern "C" __declspec(dllexport) int test_plugin_queryCalls(void) { return g_calls; }
extern "C" __declspec(dllexport) unsigned test_plugin_lastFlags(void) { return g_flags; }
extern "C" __declspec(dllexport) int test_plugin_hookAllId(void) { return g_hookAllId; }
extern "C" __declspec(dllexport) int test_plugin_hookNarrowId(void) { return g_hookNarrowId; }
extern "C" __declspec(dllexport) int test_plugin_broadCount(void) { return g_broadN; }
extern "C" __declspec(dllexport) unsigned test_plugin_broadType(int i) { return g_broad[i].type; }
extern "C" __declspec(dllexport) int test_plugin_narrowCount(void) { return g_narrowN; }
extern "C" __declspec(dllexport) unsigned test_plugin_narrowType(int i) { return g_narrow[i].type; }

static void OnBroad(uint32_t ev, const char* arg, void*) {
    if (g_broadN < KMAX_EV) {
        g_broad[g_broadN] = {ev, arg ? arg[0] : '\0'};
        ++g_broadN;
    }
    // mid-dispatch cross-removal: broad unsubscribes narrow on DOC_SAVED
    if (ev == XFS_EVT_DOC_SAVED && g_host && g_host->removeEventHook &&
        g_hookNarrowId)
        g_host->removeEventHook(g_hookNarrowId);
}

static void OnNarrow(uint32_t ev, const char* arg, void*) {
    if (g_narrowN < KMAX_EV) {
        g_narrow[g_narrowN] = {ev, arg ? arg[0] : '\0'};
        ++g_narrowN;
    }
}

static void test_cmd(void* user) {
    (void)user;
    ++g_calls;
    if (!g_host) return;
    if (g_host->log) g_host->log("test_plugin: command fired");
    if (g_host->abiVersion >= 2) {
        // Exercise v2 callbacks in a host WITHOUT a workspace: every one must be a
        // safe no-op reporting its documented "nothing open" value.
        char buf[8] = "?";
        if (g_host->getDocumentCount() == 0)                 g_flags |= 1;
        if (g_host->getActiveDocumentIndex() == -1)          g_flags |= 2;
        if (buf[0] && !g_host->getActiveFile(buf, sizeof(buf))) g_flags |= 4;
        if (g_host->getTextLength() == 0)                    g_flags |= 8;
        if (!g_host->getText(buf, sizeof(buf)))              g_flags |= 16;
        if (!g_host->getSelectedText(buf, sizeof(buf)))      g_flags |= 32;
        if (g_host->getCurrentLine() == -1)                  g_flags |= 64;
        if (g_host->getCurrentColumn() == -1)                g_flags |= 128;
        if (!g_host->gotoLine(5))                            g_flags |= 256;
        if (!g_host->isModified())                           g_flags |= 512;
        if (!g_host->isReadOnly())                           g_flags |= 1024;
        if (!g_host->saveActive())                           g_flags |= 2048;
        if (!g_host->setTextUndoable("x"))                   g_flags |= 4096;
        if (!g_host->insertTextAtCaret("y"))                 g_flags |= 8192;
        if (!g_host->openFile(""))                           g_flags |= 16384;
    }
    if (g_host->abiVersion >= 3 && g_host->addEventHook && !g_hookAllId) {
        g_hookAllId = g_host->addEventHook(OnBroad, nullptr, XFS_EVT_ALL);
        g_hookNarrowId = g_host->addEventHook(
            OnNarrow, nullptr, XFS_EVT_DOC_SAVED | XFS_EVT_CURSOR_MOVED);
    }
}

static void onRegister(const xfs_plugin_host* host) {
    g_host = host;
    if (host && host->addCommand)
        host->addCommand("Test Command", "测试", test_cmd, nullptr);
}

static void onUnregister(void) {
    g_host = nullptr; g_calls = 0; g_flags = 0;
    g_hookAllId = 0; g_hookNarrowId = 0;
    g_broadN = 0; g_narrowN = 0;
}

static const xfs_plugin_abi kAbi = {
    XFS_PLUGIN_ABI_VERSION,
    "test-plugin",
    "0.2",
    "unit-test plugin",
    onRegister,
    onUnregister,
};

extern "C" __declspec(dllexport) const xfs_plugin_abi* xfsPlugin_getInfo(void) {
    return &kAbi;
}
