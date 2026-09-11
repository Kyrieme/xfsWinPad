#pragma once
// xfsWinPad plugin C ABI (v3).
//
// A plugin is a DLL that exports these two C functions:
//   xfsPlugin_getInfo            -> const xfs_plugin_abi* or nullptr
//   xfsPlugin_register(host)     -> appears in the command palette / menus
//   xfsPlugin_unregister()
//
// The host hands the plugin an xfs_plugin_host* at register time with
// function pointers the plugin may call during its lifetime. All strings in
// and out of the ABI are UTF-8 except where noted. The plugin must not call
// back into the host after unregister() returns.
//
// Versioning: bump XFS_PLUGIN_ABI_VERSION on any incompatible change. Host
// refuses to load a plugin whose reported ABI version differs. The host struct
// only ever grows by appending pointers at the end, so a matching version
// always means every pointer in it is valid.
//
// Threading: all host callbacks must be called on the UI thread — i.e. from
// inside command callbacks, event-hook dispatch or other host-initiated calls.

#include <stdint.h>

#define XFS_PLUGIN_ABI_VERSION 3u

// ---- Event types (bit flags; the value passed to an xfs_event_cb is ONE bit)
// ---------------------------------------------------------------------------
#define XFS_EVT_APP_READY     0x01u   // s1=NULL,      fired once after app init
#define XFS_EVT_DOC_OPENED    0x02u   // s1=file path ("": untitled)
#define XFS_EVT_DOC_ACTIVATED 0x04u   // s1=file path
#define XFS_EVT_DOC_SAVED     0x08u   // s1=file path
#define XFS_EVT_DOC_CLOSED    0x10u   // s1=file path of the closed document
#define XFS_EVT_TEXT_MODIFIED 0x20u   // s1=NULL,      coalesced ~250ms
#define XFS_EVT_CURSOR_MOVED  0x40u   // s1="line,column" 1-based, coalesced
#define XFS_EVT_ALL           0x7Fu

#ifdef __cplusplus
extern "C" {
#endif

// A plugin-defined command handle. The host allocates and owns these; the
// plugin holds the pointer returned by addCommand until unregister and must
// never free it (call removeCommand at most).
struct xfs_plugin_command {
    uint32_t id;        // host-allocated id, opaque to the plugin
};
// Host command registration. `label` is shown in the Command Palette and (for
// top-level plugin commands) optional menus. `callback` is invoked on the UI
// thread when the user executes the command; `user` is passed to the callback.
// Returns a non-null command handle on success (failed duplicate = null).
typedef void (*xfs_plugin_cmd_cb)(void* user);

// Host->plugin event dispatch. `event` is ONE XFS_EVT_* bit; `utf8Arg`
// carries the documented payload (file path or "line,column") or NULL.
typedef void (*xfs_event_cb)(uint32_t event, const char* utf8Arg, void* user);

typedef struct xfs_plugin_host {
    // ---- v1 ----------------------------------------------------------------
    uint32_t abiVersion;

    // Output: the plugin calls this to publish a command to the app.
    xfs_plugin_command* (*addCommand)(const char* label, const char* category,
                                      xfs_plugin_cmd_cb cb, void* user);

    // Output: remove a previously-added command (idempotent; null-safe).
    void (*removeCommand)(xfs_plugin_command* cmd);

    // Query: full path of the active document, copied into out[] as UTF-8.
    // Copies up to cap-1 bytes and always NUL-terminates. Returns true when
    // copied in full; false when no active document, truncated or bad args.
    int (*getActiveFile)(char* out, uint32_t cap);

    // Logging: write a line to the app's log panel.
    void (*log)(const char* message);

    // ---- v2: document & editor access --------------------------------------
    // Every function below targets the active document; with no document open
    // they degrade gracefully (return 0/-1/false, never crash).

    int (*getDocumentCount)(void);              // number of open documents
    int (*getActiveDocumentIndex)(void);        // -1 when none

    // Full path of document #index (UTF-8, same copy rules as getActiveFile).
    // Untitled documents report "". Returns false for a bad index.
    int (*getDocumentPath)(int index, char* out, uint32_t cap);

    // Open a file by path (activates an already-open copy instead of
    // duplicating the tab). Returns false when the file cannot be opened.
    int (*openFile)(const char* utf8Path);
    int (*saveActive)(void);                    // false on failure / no doc

    // Text access, whole active buffer.
    uint32_t (*getTextLength)(void);            // byte length of UTF-8 text
    // Bounded copy of the whole text. Returns true if it fit completely;
    // compares getTextLength() against cap-1 beforehand to size the buffer.
    int (*getText)(char* out, uint32_t cap);
    // Replace the entire buffer as ONE undo step.
    int (*setTextUndoable)(const char* utf8Text);
    // Insert text at the caret (undoable), without disturbing the clipboard.
    int (*insertTextAtCaret)(const char* utf8Text);
    // Bounded copy of the current selection (true iff it fit).
    int (*getSelectedText)(char* out, uint32_t cap);

    // Cursor & state.
    int (*getCurrentLine)(void);                // 1-based, -1 when no doc
    int (*getCurrentColumn)(void);              // 1-based, -1 when no doc
    int (*gotoLine)(int line1based);            // scrolls into view
    int (*isModified)(void);                    // dirty since last save
    int (*isReadOnly)(void);                    // read-only flag

    // ---- v3: event hooks & config ------------------------------------------
    // Subscribe to host events. `eventMask` is a bitwise OR of XFS_EVT_* bits;
    // the callback receives ONE event per dispatch with its single bit set.
    // Returns a hook id >= 1, or 0 on failure. Hooks fire on the UI thread;
    // a hook may safely removeEventHook() itself while being dispatched.
    int (*addEventHook)(xfs_event_cb cb, void* user, uint32_t eventMask);

    // Unsubscribe (idempotent; unknown id is ignored).
    void (*removeEventHook)(int hookId);

    // Per-plugin scratch config directory:
    //   %APPDATA%\xfsWinPad\plugins\config  (created on demand)
    // Same copy rules as getActiveFile.
    int (*getConfigDir)(char* out, uint32_t cap);
} xfs_plugin_host;

// Plugin must export this to tell the host how to load it.
struct xfs_plugin_abi {
    uint32_t abiVersion;                 // must equal XFS_PLUGIN_ABI_VERSION
    const char* name;                    // short plugin name (UTF-8)
    const char* version;                 // human version string (UTF-8)
    const char* description;             // one-line description (UTF-8)

    // Lifecycle. register() is called after the host validates abiVersion.
    // Return 0 on success, non-zero to roll back load (unregister won't be called).
    void (*registerPlugin)(const xfs_plugin_host* host);
    void (*unregisterPlugin)(void);
};

#ifdef __cplusplus
}
#endif
