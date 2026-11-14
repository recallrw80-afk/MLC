cmake_minimum_required(VERSION 3.16)

# ---- cli — CLI frontend ----
# 依赖 ../libmlccore（解耦的纯 C++ 核心库）

project(mlc VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Git version injection
execute_process(
    COMMAND git describe --tags --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_DESCRIBE STREQUAL "")
    set(GIT_DESCRIBE "v${PROJECT_VERSION}-unknown")
endif()
if(GIT_COMMIT_HASH STREQUAL "")
    set(GIT_COMMIT_HASH "0000000")
endif()
message(STATUS "Build version: ${GIT_DESCRIBE} (${GIT_COMMIT_HASH})")

# Qt 路径 — 如果从父项目继承则跳过，否则尝试常见路径
if(NOT DEFINED CMAKE_PREFIX_PATH OR CMAKE_PREFIX_PATH STREQUAL "")
    set(CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/6.11.1/gcc_64"
        CACHE PATH "Qt 6 installation prefix")
    message(STATUS "CMAKE_PREFIX_PATH not set, trying: ${CMAKE_PREFIX_PATH}")
endif()

# libmlccore 可能已由父项目构建，也可能需要独立构建
if(NOT TARGET mlccore)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sdk
                     ${CMAKE_CURRENT_BINARY_DIR}/sdk)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(mlc
    src/main.cpp
    src/commands.cpp
    src/help.cpp
    src/cmd_instance.cpp
    src/cmd_player.cpp
    src/cmd_server.cpp
    src/cmd_misc.cpp
    src/test.cpp
    src/tui_select.cpp
    src/tui_prompt.cpp
)

target_include_directories(mlc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include
)

target_link_libraries(mlc PRIVATE mlccore Qt6::Core)

target_compile_definitions(mlc PRIVATE
    GIT_DESCRIBE="${GIT_DESCRIBE}"
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
)

target_compile_features(mlc PRIVATE cxx_std_20)

# rpath: 优先在同目录及 lib/ 子目录找依赖库（打包后依赖放 lib/，开发时 .so 放旁边）
# Linux 必须用老式 DT_RPATH（--disable-new-dtags）：RUNPATH 不可传递，间接依赖
# （如 libgssapi_krb5 依赖 libkrb5）会漏到系统路径；RPATH 对整个进程传递生效
if(APPLE)
    set_target_properties(mlc PROPERTIES
        BUILD_RPATH "@loader_path;@loader_path/lib"
        INSTALL_RPATH "@loader_path;@loader_path/lib"
    )
else()
    set_target_properties(mlc PROPERTIES
        BUILD_RPATH "$ORIGIN;$ORIGIN/lib"
        INSTALL_RPATH "$ORIGIN;$ORIGIN/lib"
    )
    target_link_options(mlc PRIVATE "-Wl,--disable-new-dtags")
endif()
