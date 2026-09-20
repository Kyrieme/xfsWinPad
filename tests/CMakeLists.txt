# xfsWinPad unit tests
add_executable(test_encoding test_encoding.cpp
    ../src/encoding/Encoding.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_encoding PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_encoding PRIVATE /utf-8 /W3)
endif()

add_test(NAME encoding COMMAND test_encoding)

# 单文件配置 profile：导出/导入字节级回环 + 安全防护
add_executable(test_profile test_profile.cpp
    ../src/settings/Profile.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)
target_compile_definitions(test_profile PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_profile PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_profile PRIVATE user32 shell32)
add_test(NAME profile COMMAND test_profile)

# 插件管理：清单解析 + 已安装注册表（不依赖真实控件）
add_executable(test_plugin_catalog test_plugin_catalog.cpp
    ../src/plugin/PluginCatalog.cpp
    ../src/plugin/PluginRegistry.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_plugin_catalog PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_plugin_catalog PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_plugin_catalog PRIVATE shlwapi version)

add_test(NAME plugin_catalog COMMAND test_plugin_catalog)

# 插件安装器：下载/解压/落盘编排（注入假实现）+ 真实 miniz ZIP 回环
add_executable(test_plugin_installer test_plugin_installer.cpp
    ../src/plugin/PluginInstaller.cpp
    ../src/plugin/PluginCatalog.cpp
    ../src/plugin/PluginRegistry.cpp
    ../src/core/Util.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_plugin_installer PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_plugin_installer PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_plugin_installer PRIVATE miniz winhttp shlwapi version)

add_test(NAME plugin_installer COMMAND test_plugin_installer)

# 真实网络安装闭环：GitHub release 直连下载 + 真实解压（离线时 SKIP）。
add_executable(test_plugin_net test_plugin_net.cpp
    ../src/plugin/PluginInstaller.cpp
    ../src/plugin/PluginRegistry.cpp
    ../src/core/Util.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_plugin_net PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_plugin_net PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_plugin_net PRIVATE miniz winhttp shlwapi version)

add_test(NAME plugin_net COMMAND test_plugin_net)

# Workshop（AI 插件工场）：目录名合法化 + 脚手架产物 + 安装错误路径。
add_executable(test_workshop test_workshop.cpp stub_workspace.cpp
    ../src/plugin/Workshop.cpp
    ../src/plugin/PluginManager.cpp
    ../src/plugin/npp/NppCompat.cpp
    ../src/plugin/oop/OopHost.cpp
    ../src/core/Util.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_workshop PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_workshop PRIVATE /utf-8 /W3)
endif()
target_include_directories(test_workshop PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/src)
target_link_libraries(test_workshop PRIVATE shlwapi user32 shell32)

add_test(NAME workshop COMMAND test_workshop)

# I18n：字典加载/回退/格式化/切换回调（fixture 目录由测试自建）
# 另加"真实语言文件键集合一致"的完整性校验：路径经 configure_file 生成头文件
# 传入，不用 -D 字符串宏（含空格/反斜杠的路径在命令行上转义极易出错）。
set(XFS_LANG_DIR_FWD "${CMAKE_SOURCE_DIR}/resources/lang")
configure_file(lang_test_dir.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/lang_test_dir.h" @ONLY)

add_executable(test_i18n test_i18n.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_i18n PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(test_i18n PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(test_i18n PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_i18n PRIVATE shlwapi)

add_test(NAME i18n COMMAND test_i18n)

add_executable(test_settings test_settings.cpp
    ../src/settings/Settings.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_settings PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_settings PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_settings PRIVATE shlwapi)

add_executable(test_stdf test_stdf.cpp
    ../src/stdf/StdfFile.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_stdf PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_stdf PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_stdf PRIVATE shlwapi)

add_test(NAME stdf COMMAND test_stdf)

add_test(NAME settings COMMAND test_settings)

add_executable(test_styler test_styler.cpp
    ../src/theme/Styler.cpp
    ../src/theme/Theme.cpp
    ../src/core/JsonLite.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_styler PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_styler PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_styler PRIVATE shlwapi)

add_test(NAME styler COMMAND test_styler)

# 自研 ATE 词法器（批次 72）：假 IDocument 驱动真实 ILexer5，
# 覆盖 .pat/.stil/.log 的行分类、跨窗口状态、折叠与保守门。
# 批次 73 追加 .dec/.pln 两支（自研族现在是 5 个词法器）——XfsCreateLexer 里
# 引用了它们，链接期必须有定义。
add_executable(test_atelexer test_atelexer.cpp
    ../src/language/XfsLexer.cpp
    ../src/language/AtePatternLexer.cpp
    ../src/language/StilLexer.cpp
    ../src/language/AteLogLexer.cpp
    ../src/language/ChromaDecLexer.cpp
    ../src/language/ChromaPlanLexer.cpp
    ../src/language/Chroma3380Db.cpp)

target_include_directories(test_atelexer PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include)
target_compile_definitions(test_atelexer PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_atelexer PRIVATE /utf-8 /W3)
endif()

add_test(NAME atelexer COMMAND test_atelexer)
set_tests_properties(atelexer PROPERTIES TIMEOUT 120)

# 批次 73：Chroma 3380 签名提示的位置解析（纯函数，不依赖 Scintilla）。
# 覆盖嵌套括号/多行调用/字符串里的逗号/越界/`-`与`@`前缀，并直接断言手册里
# FORCE_V_MLDPS 的 v_range={@6V,@12V}、i_range 的 7 档——这两条同时钉死
# 生成器曾经的「逐个去重导致候选值区间截断」静默错。
# 批次 77 追加 Chroma3380Complete.cpp（语句补全候选生成）：它 include 了
# XfsLexer.h 取三支词法器的名字，因此这里要给出 scintilla 的头文件目录。
add_executable(test_chromasig test_chromasig.cpp
    ../src/language/ChromaSignature.cpp
    ../src/language/Chroma3380Complete.cpp
    ../src/language/Chroma3380Db.cpp)

target_include_directories(test_chromasig PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include)

target_compile_definitions(test_chromasig PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_chromasig PRIVATE /utf-8 /W3)
endif()

add_test(NAME chromasig COMMAND test_chromasig)

# 批次 80：Chroma 3380 静态校验内核（方向 C）。纯函数，不用 Scintilla，
# 因此这里只编它自己 —— 但**必须**用**手册原文**当正例（§2.3.3 / §2.4.3 的官方
# .dec 示例），再用真实 AAA .pln 全文当"零诊断"回归。
# 批次 86：规则 8 要读手册签名（Chroma3380Db）与"语句起始"判定（Chroma3380Complete），
# 两者同样是纯字符串逻辑、不依赖 Scintilla，所以并到同一个测试目标里，
# 好处是 `test_chromadiag <文件>` 这个回归入口拿到的就是完整的校验能力。
add_executable(test_chromadiag test_chromadiag.cpp
    ../src/language/Chroma3380Diagnostics.cpp
    ../src/language/Chroma3380Db.cpp
    ../src/language/Chroma3380Complete.cpp)

target_compile_definitions(test_chromadiag PRIVATE UNICODE _UNICODE NOMINMAX)
# Chroma3380Complete.cpp 只为取三个词法器的**名字常量**而 include 了 XfsLexer.h，
# 后者又 include <ILexer.h>（上游放在 scintilla/include，不在 lexilla/）。
# 所以这里只需要两个头文件搜索路径，不链接 Scintilla / Lexilla 的任何符号。
target_include_directories(test_chromadiag PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/lexilla/include)
if(MSVC)
    target_compile_options(test_chromadiag PRIVATE /utf-8 /W3)
endif()

add_test(NAME chromadiag COMMAND test_chromadiag)

# 批次 94：CRAFT 工程模型内核（方向 D 第一刀）。同样是纯字符串逻辑、零 Windows API，
# 所以单独一个目标、不链任何 Scintilla / Util。
# 用例把**厂商 makefile 原文**与**厂商中间产物的字节**都钉住了（见测试文件头）。
# 额外提供 `test_craftproj <工程文件>` 探针：拿真实工程跑一遍打印模型，人工核对。
add_executable(test_craftproj test_craftproj.cpp
    ../src/language/CraftProject.cpp)

target_compile_definitions(test_craftproj PRIVATE UNICODE _UNICODE NOMINMAX)
# CraftProject.cpp 用 "language/CraftProject.h" 自包含路径，所以要给 src/ 作为搜索根。
target_include_directories(test_craftproj PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(test_craftproj PRIVATE /utf-8 /W3)
endif()

add_test(NAME craftproj COMMAND test_craftproj)

# 批次 95：CRAFT 编译命令执行器（方向 D 第二刀）。这个测试会**真的起进程**，
# 所以它链 CraftRunner.cpp（需要 windows.h）而不只是纯字符串逻辑。
# 被测子进程就是它自己（`--child <mode>`），不依赖 cmd.exe / ping 之类的系统工具。
# 另提供 `test_craftrunner <工程文件>` 手工探针：真的把该工程编译一遍并打印输出。
add_executable(test_craftrunner test_craftrunner.cpp
    ../src/language/CraftRunner.cpp
    ../src/language/CraftHost.cpp
    ../src/language/CraftProject.cpp)

target_compile_definitions(test_craftrunner PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(test_craftrunner PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(test_craftrunner PRIVATE /utf-8 /W3)
endif()

add_test(NAME craftrunner COMMAND test_craftrunner)

# 批次 101：CRAFT 工具链桩 —— 给 scripts/craft-e2e.ps1 当"装出来的 plncmp/patcmp"。
# **不入 add_test**：它自己没有任何断言，被测的是 craft-e2e.ps1 那条 GUI 链路
# （与下面 dump_nppexec 同理）。样本与退出码由脚本写到桩旁边，见 craft_stub.cpp 文件头。
add_executable(craft_stub craft_stub.cpp)

target_compile_definitions(craft_stub PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(craft_stub PRIVATE /utf-8 /W3)
endif()

add_executable(test_shortcut test_shortcut.cpp
    ../src/shortcut/ShortcutTable.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_shortcut PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_shortcut PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_shortcut PRIVATE shlwapi)

add_test(NAME shortcut COMMAND test_shortcut)

add_executable(test_lineops test_lineops.cpp
    ../src/editor/LineOps.cpp)

target_compile_definitions(test_lineops PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_lineops PRIVATE /utf-8 /W3)
endif()

add_test(NAME lineops COMMAND test_lineops)

# 自动补全词汇扫描（批次 31）：lexer 注释/字符串风格过滤表 + 纯/风格两路扫描
add_executable(test_wordscan test_wordscan.cpp
    ../src/editor/Wordscan.cpp)

target_include_directories(test_wordscan PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/lexilla/include)
target_compile_definitions(test_wordscan PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_wordscan PRIVATE /utf-8 /W3)
endif()

add_test(NAME wordscan COMMAND test_wordscan)

# CSV 表格解析（批次 32/33）：引号/分隔符探测/数值排序/过滤/SetCell/序列化回环
add_executable(test_csv test_csv.cpp
    ../src/csv/CsvParser.cpp
    ../src/csv/CsvPrint.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_csv PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_csv PRIVATE /utf-8 /W3)
endif()

add_test(NAME csv COMMAND test_csv)

# 大文件 CSV 只读虚拟模型（批次 36）：字节级行索引/物化/编码/引号态边界
add_executable(test_csv_big test_csv_big.cpp
    ../src/csv/CsvBigModel.cpp
    ../src/csv/CsvBigFilter.cpp
    ../src/csv/CsvParser.cpp
    ../src/encoding/Encoding.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_csv_big PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_csv_big PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_csv_big PRIVATE user32)

add_test(NAME csv_big COMMAND test_csv_big)

add_executable(test_session test_session.cpp
    ../src/session/Session.cpp
    ../src/core/Util.cpp)

target_compile_definitions(test_session PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_session PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_session PRIVATE shlwapi)

add_test(NAME session COMMAND test_session)

# 大文件虚拟滚动模型：换页 mmap + 稀疏行索引（小样本注入，不到 1MB 也全路径覆盖）
add_executable(test_bigfile test_bigfile.cpp
    ../src/bigfile/BigFileModel.cpp
    ../src/encoding/Encoding.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_bigfile PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_bigfile PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_bigfile PRIVATE shlwapi user32)

add_test(NAME bigfile COMMAND test_bigfile)

add_executable(test_findinfiles test_findinfiles.cpp stub_editor.cpp
    ../src/search/FindInFiles.cpp
    ../src/search/SearchAll.cpp
    ../src/search/SearchService.cpp
    ../src/encoding/Encoding.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_include_directories(test_findinfiles PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/src
    ${CMAKE_SOURCE_DIR}/third_party/lexilla/include)

target_compile_definitions(test_findinfiles PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_findinfiles PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_findinfiles PRIVATE shlwapi user32)

add_test(NAME findinfiles COMMAND test_findinfiles)

add_executable(test_terminal test_terminal.cpp
    ../src/terminal/VtParser.cpp
    ../src/terminal/ConPTY.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_terminal PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_terminal PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_terminal PRIVATE shlwapi user32)

add_test(NAME terminal COMMAND test_terminal)

# opencode AI 客户端：纯 JSON 助手（session/version/messages 解析 + prompt body 转义回环）
add_executable(test_ai_json test_ai_json.cpp
    ../src/ai/OcClient.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_ai_json PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_ai_json PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_ai_json PRIVATE winhttp wbemuuid ole32 oleaut32 shlwapi user32)

add_test(NAME ai_json COMMAND test_ai_json)

# OpenAI-compatible 客户端：纯 JSON 助手（chat body/choices/models/错误解析）
add_executable(test_oai_json test_oai_json.cpp
    ../src/ai/OaiClient.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_oai_json PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_oai_json PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_oai_json PRIVATE winhttp user32)

add_test(NAME oai_json COMMAND test_oai_json)

add_executable(test_conpty_interactive test_conpty_interactive.cpp
    ../src/terminal/ConPTY.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(test_conpty_interactive PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_conpty_interactive PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_conpty_interactive PRIVATE shlwapi user32)

add_test(NAME conpty_interactive COMMAND test_conpty_interactive)

# ---- plugin manager test: build a companion plugin DLL, then load it ---------
add_library(test_plugin_core SHARED plugin_dll/test_plugin_core.cpp)
target_compile_definitions(test_plugin_core PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_plugin_core PRIVATE /utf-8 /W3)
endif()
target_include_directories(test_plugin_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/plugin)
set_target_properties(test_plugin_core PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/plugins
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/plugins)

# NPP 形态的兼容装载测试插件（自带契约结构声明，不 include 宿主头文件）
add_library(test_npp_core SHARED npp_dll/test_npp_core.cpp)
target_compile_definitions(test_npp_core PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_npp_core PRIVATE /utf-8 /W3)
endif()
set_target_properties(test_npp_core PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/plugins
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/plugins)

# 4d：真实对话框 + 可停靠面板注册的 NPP 形态插件（test_dock 的加载对象）。
# 独立输出目录，避免污染 test_plugin 的"混合装载回归"目录（loaded==2）。
add_library(test_npp_dock SHARED npp_dock_dll/test_npp_dock.cpp
    npp_dock_dll/test_npp_dock.rc)
target_compile_definitions(test_npp_dock PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_npp_dock PRIVATE /utf-8 /W3)
endif()
set_target_properties(test_npp_dock PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/plugins_dock
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/plugins_dock)

add_executable(test_plugin test_plugin.cpp stub_workspace.cpp
    ../src/plugin/PluginManager.cpp
    ../src/plugin/npp/NppCompat.cpp
    ../src/plugin/oop/OopHost.cpp
    ../src/core/Log.cpp
    ../src/core/Util.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp)
target_compile_definitions(test_plugin PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_plugin PRIVATE /utf-8 /W3)
endif()
# PluginManager.cpp pulls in Workspace/Document/Editor headers, which include
# <Scintilla.h> (bridge sends messages raw to the child control; no Editor cpp).
target_include_directories(test_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/src)
add_dependencies(test_plugin test_plugin_core test_npp_core)
target_link_libraries(test_plugin PRIVATE shlwapi user32)

add_test(NAME plugin COMMAND test_plugin
    $<TARGET_FILE_DIR:test_plugin_core>)

# ---- 4b: NPP 消息垫片契约单测（假文档集，不依赖真实控件） ------------------
add_executable(test_npp_msgs test_npp_msgs.cpp stub_workspace.cpp
    ../src/plugin/PluginManager.cpp
    ../src/plugin/npp/NppCompat.cpp
    ../src/plugin/oop/OopHost.cpp
    ../src/core/Log.cpp
    ../src/core/Util.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp)
target_compile_definitions(test_npp_msgs PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_npp_msgs PRIVATE /utf-8 /W3)
endif()
target_include_directories(test_npp_msgs PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/src)
target_link_libraries(test_npp_msgs PRIVATE shlwapi user32)

add_test(NAME npp_msgs COMMAND test_npp_msgs)

# ---- 进程外插件桥 e2e（真实代理进程 + 真实 NPP 形态测试 DLL）-----------------
# 六个自持契约的测试插件（好插件 / setInfo-exit / 命令回调 AV / 第二好插件 /
# 装载卡死 / messageProc 桥），独立输出目录。
foreach(oopdll oop_good oop_killer oop_crasher oop_multi oop_hang oop_msg)
    add_library(${oopdll} SHARED oop_plugins/${oopdll}.cpp)
    target_compile_definitions(${oopdll} PRIVATE UNICODE _UNICODE NOMINMAX)
    if(MSVC)
        target_compile_options(${oopdll} PRIVATE /utf-8 /W3)
    endif()
    set_target_properties(${oopdll} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/oop_plugins
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/release/oop_plugins)
endforeach()

add_executable(test_oop test_oop.cpp stub_workspace.cpp
    ../src/plugin/PluginManager.cpp
    ../src/plugin/npp/NppCompat.cpp
    ../src/plugin/oop/OopHost.cpp
    ../src/core/Log.cpp
    ../src/core/Util.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp)
target_compile_definitions(test_oop PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_oop PRIVATE /utf-8 /W3)
endif()
# PluginManager.h → Workspace/Document/Editor.h → <Scintilla.h>
target_include_directories(test_oop PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/src)
target_link_libraries(test_oop PRIVATE shlwapi user32 shell32)
add_dependencies(test_oop oop_good oop_killer oop_crasher oop_multi oop_hang oop_msg
    xfsWinPadPluginHost)
add_test(NAME oop COMMAND test_oop
    $<TARGET_FILE_DIR:oop_good>)
set_tests_properties(oop PROPERTIES TIMEOUT 180)

# ---- 4d：DockManager 端到端（真实插件对话框 + 真实 wrapper/布局）------------
add_executable(test_dock test_dock.cpp stub_workspace.cpp
    ../src/plugin/PluginManager.cpp
    ../src/plugin/npp/NppCompat.cpp
    ../src/plugin/oop/OopHost.cpp
    ../src/plugin/DockManager.cpp
    ../src/core/Log.cpp
    ../src/core/Util.cpp
    ../src/core/I18n.cpp
    ../src/core/JsonLite.cpp)
target_compile_definitions(test_dock PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_dock PRIVATE /utf-8 /W3)
endif()
target_include_directories(test_dock PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/include
    ${CMAKE_SOURCE_DIR}/third_party/scintilla/src)
add_dependencies(test_dock test_npp_dock)
target_link_libraries(test_dock PRIVATE shlwapi user32)

add_test(NAME dock COMMAND test_dock
    $<TARGET_FILE_DIR:test_npp_dock>)

# 临时诊断：dump NppExec getFuncsArray 返回内存（不入 add_test）
add_executable(dump_nppexec dump_nppexec.cpp)
target_compile_definitions(dump_nppexec PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(dump_nppexec PRIVATE /utf-8 /W3)
endif()
target_link_libraries(dump_nppexec PRIVATE user32)

# ---- xfsbench：核心数据路径离线基准（mmap/编码/大文件/CSV 虚拟模型）----------
# 不入 ctest；手动运行或由 ci.yml 调用（--csv 输出机器可读行，上传 artifact）。
add_executable(xfsbench xfsbench.cpp
    ../src/bigfile/BigFileModel.cpp
    ../src/csv/CsvBigModel.cpp
    ../src/csv/CsvParser.cpp
    ../src/encoding/Encoding.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)

target_compile_definitions(xfsbench PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(xfsbench PRIVATE /utf-8 /W3)
endif()
target_link_libraries(xfsbench PRIVATE shlwapi user32)

# Git 集成 v1：porcelain 解析 + 仓库根发现（批次 46，纯逻辑无进程）
# 批次 48 追加：QuoteArg 回环 + 真实仓库 add/restore/commit 链路（GitClient::Run）
add_executable(test_git test_git.cpp
    ../src/git/GitStatus.cpp
    ../src/git/GitClient.cpp
    ../src/core/Util.cpp
    ../src/core/Log.cpp)
target_include_directories(test_git PRIVATE ../src)
target_compile_definitions(test_git PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(test_git PRIVATE /utf-8 /W3)
endif()
target_link_libraries(test_git PRIVATE shell32)
add_test(NAME git COMMAND test_git)
