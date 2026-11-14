cmake_minimum_required(VERSION 3.16)

project(MLC VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(QT_QML_GENERATE_QMLLS_INI ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Network Concurrent)
find_package(ZLIB REQUIRED)

qt_standard_project_setup(REQUIRES 6.11)

# nlohmann-json
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Git version
execute_process(COMMAND git describe --tags --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GIT_DESCRIBE STREQUAL "")
    set(GIT_DESCRIBE "v${PROJECT_VERSION}-unknown")
endif()
if(GIT_COMMIT_HASH STREQUAL "")
    set(GIT_COMMIT_HASH "0000000")
endif()
message(STATUS "Build version: ${GIT_DESCRIBE} (${GIT_COMMIT_HASH})")

# ---- QML GUI App (gui/) ----

set(CORE_SOURCES gui/src/util/file_drop_handler.cpp
                 gui/src/bridge/player_bridge.cpp
                 gui/src/bridge/install_bridge.cpp
                 gui/src/bridge/instance_bridge.cpp
                 gui/src/bridge/mod_platform_bridge.cpp)
set(CORE_HEADERS gui/include/util/file_drop_handler.h
                 gui/include/bridge/player_bridge.h
                 gui/include/bridge/install_bridge.h
                 gui/include/bridge/instance_bridge.h
                 gui/include/bridge/mod_platform_bridge.h)

qt_add_executable(mlc-gui gui/src/main.cpp)

# Lucide icons
set(LUCIDE_ICONS "play" "bolt" "arrow-down-to-line" "layout-grid" "x" "minus"
    "shield-check" "unlink" "file-plus"
    "folder" "file-archive" "package" "file-code" "image" "file-text" "file")
set(LUCIDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gui/assets/icons/lucide")
file(MAKE_DIRECTORY "${LUCIDE_DIR}")

function(fetch_lucide_icon icon_name)
    set(out_path "${LUCIDE_DIR}/${icon_name}.svg")
    if(NOT EXISTS "${out_path}")
        set(lucide_url "https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/${icon_name}.svg")
        message(STATUS "Downloading Lucide icon: ${icon_name}")
        file(DOWNLOAD "${lucide_url}" "${out_path}" STATUS dl_status TIMEOUT 10)
        list(GET dl_status 0 status_code)
        if(NOT status_code EQUAL 0)
            message(WARNING "Failed to download lucide:${icon_name}")
            file(REMOVE "${out_path}")
            return()
        endif()
        file(READ "${out_path}" svg_content)
        string(REPLACE "stroke=\"currentColor\"" "stroke=\"#ffffff\"" svg_content "${svg_content}")
        file(WRITE "${out_path}" "${svg_content}")
    endif()
endfunction()

foreach(icon ${LUCIDE_ICONS})
    fetch_lucide_icon(${icon})
endforeach()

# Icon assets
file(GLOB_RECURSE ICON_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} gui/assets/icons/*.svg)
file(GLOB ASSET_SVG_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} gui/assets/*.svg)
list(APPEND ICON_FILES ${ASSET_SVG_FILES})
qt_add_resources(mlc-gui "assets" PREFIX "/" FILES ${ICON_FILES})

target_include_directories(mlc-gui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/src
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/include/util
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include
    ${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src/include)

target_link_libraries(mlc-gui
    PUBLIC Qt6::Quick Qt6::Network Qt6::Concurrent
    PUBLIC nlohmann_json::nlohmann_json ZLIB::ZLIB mlccore)

target_compile_definitions(mlc-gui PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    GIT_DESCRIBE="${GIT_DESCRIBE}"
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}")

# Theme.qml singleton
set_source_files_properties(gui/ui/styles/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(mlc-gui URI MLC VERSION 1.0
    QML_FILES
        gui/ui/Main.qml
        gui/ui/pages/PageLaunch.qml
        gui/ui/pages/PageDownload.qml
        gui/ui/pages/PageSettings.qml
        gui/ui/pages/PageMore.qml
        gui/ui/components/MLCButton.qml
        gui/ui/components/MLCIcon.qml
        gui/ui/components/MLCComboBox.qml
        gui/ui/components/MLCProgressBar.qml
        gui/ui/components/MLCTextBox.qml
        gui/ui/components/MLCScrollBar.qml
        gui/ui/components/MLCCard.qml
        gui/ui/components/MLCListItem.qml
        gui/ui/components/MLCIconButton.qml
        gui/ui/components/MLCCheckBox.qml
        gui/ui/components/MLCSlider.qml
        gui/ui/components/MLCSearchBox.qml
        gui/ui/components/MLCScrollViewer.qml
        gui/ui/components/MLCMsg.qml
        gui/ui/components/MLCHint.qml
        gui/ui/components/MLCDownloadPanel.qml
        gui/ui/styles/Theme.qml
    SOURCES ${CORE_SOURCES} ${CORE_HEADERS})

set_target_properties(mlc-gui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE WIN32_EXECUTABLE TRUE)

include(GNUInstallDirs)
install(TARGETS mlc-gui
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- sdk + CLI ----
set(CMAKE_AUTOMOC ON)
add_subdirectory(sdk)
add_subdirectory(cli)
