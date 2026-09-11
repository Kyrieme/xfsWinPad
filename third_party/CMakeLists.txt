# Builds vendored Scintilla 5.6.6 and Lexilla 5.5.3 as DLLs.
# Source layout and flags mirror the official win32/scintilla.mak / src/lexilla.mak.

set(SCI ${CMAKE_CURRENT_SOURCE_DIR}/scintilla)
set(LEX ${CMAKE_CURRENT_SOURCE_DIR}/lexilla)

# ---------------- Scintilla.dll ----------------
file(GLOB SCI_SRC_CORE CONFIGURE_DEPENDS ${SCI}/src/*.cxx)
set(SCI_SRC_WIN
    ${SCI}/win32/HanjaDic.cxx
    ${SCI}/win32/PlatWin.cxx
    ${SCI}/win32/ListBox.cxx
    ${SCI}/win32/SurfaceGDI.cxx
    ${SCI}/win32/SurfaceD2D.cxx
    ${SCI}/win32/ScintillaWin.cxx
    ${SCI}/win32/ScintillaDLL.cxx
)

add_library(Scintilla SHARED
    ${SCI_SRC_CORE}
    ${SCI_SRC_WIN}
    ${SCI}/win32/ScintRes.rc
)

target_include_directories(Scintilla PRIVATE ${SCI}/include ${SCI}/src)
target_compile_definitions(Scintilla PRIVATE _CRT_SECURE_NO_DEPRECATE UNICODE _UNICODE)
target_compile_options(Scintilla PRIVATE /utf-8 /EHsc /W3)
target_link_libraries(Scintilla PRIVATE kernel32 user32 gdi32 imm32 ole32 oleaut32 advapi32)

if(MSVC)
    set_target_properties(Scintilla PROPERTIES
        OUTPUT_NAME Scintilla
        LINK_FLAGS "/DEF:\"${SCI}/win32/Scintilla.def\"")
endif()

# ---------------- Lexilla.dll ----------------
file(GLOB LEX_LEXERS CONFIGURE_DEPENDS ${LEX}/lexers/*.cxx)
file(GLOB LEX_LEXLIB CONFIGURE_DEPENDS ${LEX}/lexlib/*.cxx)

add_library(Lexilla SHARED
    ${LEX_LEXERS}
    ${LEX_LEXLIB}
    ${LEX}/src/Lexilla.cxx
    ${LEX}/src/LexillaVersion.rc
)

target_include_directories(Lexilla PRIVATE ${LEX}/include ${LEX}/lexlib ${SCI}/include)
target_compile_definitions(Lexilla PRIVATE _CRT_SECURE_NO_DEPRECATE UNICODE _UNICODE)
target_compile_options(Lexilla PRIVATE /utf-8 /EHsc /W3)

if(MSVC)
    set_target_properties(Lexilla PROPERTIES
        OUTPUT_NAME Lexilla
        LINK_FLAGS "/DEF:\"${LEX}/src/Lexilla.def\"")
endif()

# ---------------- miniz (ZIP read/Write) ----------------
# Vendored miniz 3.1.2; compiled as a static library (MINIZ_EXPORT is empty).
set(MINIZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/miniz)
add_library(miniz STATIC
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c
)
target_include_directories(miniz PUBLIC ${MINIZ_DIR})
target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(miniz PRIVATE /W3 /permissive- /utf-8)
