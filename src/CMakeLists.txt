# xfsWinPad application sources
set(XFS_APP_SOURCES
    main.cpp

    app/MainWindow.cpp
    app/StatusBar.cpp
    app/FindDialog.cpp
    app/ResultsPanel.cpp
    app/CommandPalette.cpp
    app/FileExplorer.cpp
    app/InputBox.cpp
    app/ListPicker.cpp
    app/ListPicker.cpp
    app/PreferencesDialog.cpp
    app/StyleConfiguratorDialog.cpp
    app/ShortcutMapperDialog.cpp
    app/WindowsListDialog.cpp
    app/StdfPanel.cpp
    app/CsvPanel.cpp
    app/DiagnosticsPanel.cpp
    app/CompilePanel.cpp

    bigfile/BigFileModel.cpp
    bigfile/BigFileView.cpp

    ai/OcClient.cpp
    ai/OaiClient.cpp
    ai/AiPanel.cpp

    core/Log.cpp
    core/Util.cpp
    core/JsonLite.cpp
    core/I18n.cpp
    core/NavHistory.cpp

    encoding/Encoding.cpp
    csv/CsvParser.cpp
    csv/CsvBigModel.cpp
    csv/CsvBigFilter.cpp
    csv/CsvPrint.cpp
    diff/DiffView.cpp
    git/GitStatus.cpp
    git/GitClient.cpp
    hex/HexPanel.cpp
    log/LogPanel.cpp
    macro/MacroRecorder.cpp
    editor/Editor.cpp
    editor/LineOps.cpp
    editor/Wordscan.cpp
    document/Document.cpp
    language/LanguageMap.cpp
    language/XfsLexer.cpp
    language/AtePatternLexer.cpp
    language/StilLexer.cpp
    language/AteLogLexer.cpp
    language/Chroma3380Db.cpp
    language/ChromaDecLexer.cpp
    language/ChromaPlanLexer.cpp
    language/ChromaSignature.cpp
    language/Chroma3380Complete.cpp
    language/Chroma3380Diagnostics.cpp
    language/CraftProject.cpp
    language/CraftHost.cpp
    language/CraftRunner.cpp
    theme/Styler.cpp
    plugin/PluginManager.cpp
    plugin/PluginCatalog.cpp
    plugin/PluginRegistry.cpp
    plugin/PluginInstaller.cpp
    plugin/PluginAdminDialog.cpp
    plugin/Workshop.cpp
    plugin/npp/NppCompat.cpp
    plugin/oop/OopHost.cpp
    plugin/DockManager.cpp
    search/SearchService.cpp
    search/SearchAll.cpp
    search/FindInFiles.cpp
    session/Session.cpp
    settings/Settings.cpp
    settings/Profile.cpp
    stdf/StdfFile.cpp
    shortcut/ShortcutTable.cpp
    tabs/TabBar.cpp
    terminal/ConPTY.cpp
    terminal/VtParser.cpp
    terminal/TerminalPanel.cpp
    theme/Theme.cpp
    workspace/Workspace.cpp

    ../resources/xfsWinPad.rc
)

add_executable(xfsWinPad WIN32 ${XFS_APP_SOURCES})

target_include_directories(xfsWinPad PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/version
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/src
    ${CMAKE_SOURCE_DIR}/third_party/lexilla/include
)

target_compile_definitions(xfsWinPad PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(xfsWinPad PRIVATE /W3 /permissive- /utf-8)
# Manifest is embedded via resources/xfsWinPad.rc; disable the linker-generated one.
set_property(TARGET xfsWinPad APPEND_STRING PROPERTY LINK_FLAGS " /MANIFEST:NO /MAP:\"${CMAKE_BINARY_DIR}/xfsWinPad.map\"")
target_link_libraries(xfsWinPad PRIVATE Scintilla Lexilla miniz winhttp comctl32 user32 gdi32 shell32 advapi32 ole32 version shlwapi)

# Copy the editor DLLs next to the exe after build
add_custom_command(TARGET xfsWinPad POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Scintilla> $<TARGET_FILE_DIR:xfsWinPad>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Lexilla> $<TARGET_FILE_DIR:xfsWinPad>
    COMMENT "Copying Scintilla.dll and Lexilla.dll"
)

# Copy the i18n language files next to the exe (lang/*.json)
add_custom_command(TARGET xfsWinPad POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:xfsWinPad>/lang
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/resources/lang/zh-CN.json
        $<TARGET_FILE_DIR:xfsWinPad>/lang/zh-CN.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/resources/lang/en.json
        $<TARGET_FILE_DIR:xfsWinPad>/lang/en.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/resources/lang/zh-TW.json
        $<TARGET_FILE_DIR:xfsWinPad>/lang/zh-TW.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/resources/lang/ja.json
        $<TARGET_FILE_DIR:xfsWinPad>/lang/ja.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/resources/lang/ko.json
        $<TARGET_FILE_DIR:xfsWinPad>/lang/ko.json
    COMMENT "Copying lang/*.json"
)

# ---- 进程外插件代理（xfsWinPadPluginHost.exe）--------------------------------
# 极简 win32 exe：不链接宿主任何代码（自持契约镜像），只依赖 user32/shell32。
add_executable(xfsWinPadPluginHost WIN32 plugin/oop/PluginHostMain.cpp)
target_compile_definitions(xfsWinPadPluginHost PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(xfsWinPadPluginHost PRIVATE /W3 /permissive- /utf-8)
target_link_libraries(xfsWinPadPluginHost PRIVATE user32 shell32)
set_property(TARGET xfsWinPadPluginHost APPEND_STRING PROPERTY LINK_FLAGS " /MANIFEST:NO")
