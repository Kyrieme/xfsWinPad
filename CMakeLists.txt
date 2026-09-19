cmake_minimum_required(VERSION 3.24)

project(xfsWinPad VERSION 0.13.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 版本单一数据源：configure 出 version.h（代码侧）与 version.rc（资源侧），
# 安装器运行时从 exe 的 FileVersion 读取（见 installer/Setup.cs）。
math(EXPR XFS_VERSION_MAJOR_ENC "${PROJECT_VERSION_MAJOR} * 65536 + ${PROJECT_VERSION_MINOR}")
configure_file(resources/version.h.in version/version.h @ONLY)
configure_file(resources/version.rc.in version/version.rc @ONLY)

if(MSVC)
    add_compile_options(/utf-8 /MP)
endif()

add_subdirectory(third_party)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
