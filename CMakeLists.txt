cmake_minimum_required(VERSION 3.28)
project(arib-caption-monitor VERSION 1.0.0 LANGUAGES CXX)

set(APP_ID io.github.gabrielsmash.AribCaptionMonitor)
option(ARIBMON_BUILD_TESTS "Build the tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Widgets Network Multimedia)
qt_standard_project_setup()

# libaribcaption is pinned and patched instead of using the system copy: upstream wraps a line as soon
# as it is filled up to the edge of the display area, so "full line + APR" (used by Brazilian
# broadcasters) ends up one line too low. See patches/README.md and xqq/libaribcaption#27.
include(FetchContent)
set(ARIBCC_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(ARIBCC_USE_FREETYPE ON CACHE BOOL "" FORCE)
set(ARIBCC_USE_FONTCONFIG ON CACHE BOOL "" FORCE)
set(ARIBCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libaribcaption
    GIT_REPOSITORY https://github.com/xqq/libaribcaption.git
    GIT_TAG 37dde0f32652c32aa98bf92278944d1cfb2e7354
    PATCH_COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/patches/libaribcaption-deferred-wrap.patch
    UPDATE_DISCONNECTED TRUE
    SYSTEM
    EXCLUDE_FROM_ALL  # linked statically; its headers and library are not installed with the app
)
FetchContent_MakeAvailable(libaribcaption)

# System FFmpeg on purpose: a PKG_CONFIG_PATH pointing to a local FFmpeg build would make the binary
# depend on libraries that only exist on one machine.
find_package(PkgConfig REQUIRED)
set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "")
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
set(ENV{PKG_CONFIG_PATH} "${_saved_pkg_config_path}")

qt_add_executable(arib-caption-monitor
    src/main.cpp
    src/MainWindow.cpp
    src/UdpReceiver.cpp
    src/TsDemux.cpp
    src/AribInspector.cpp
    src/CaptionEngine.cpp
    src/CaptionView.cpp
    src/CaptionValidator.cpp
    src/MediaPlayer.cpp
    src/FileSource.cpp
)

target_compile_options(arib-caption-monitor PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(arib-caption-monitor PRIVATE APP_VERSION="${PROJECT_VERSION}" APP_ID="${APP_ID}")

target_link_libraries(arib-caption-monitor PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    aribcaption::aribcaption
    PkgConfig::FFMPEG
)

include(GNUInstallDirs)
install(TARGETS arib-caption-monitor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/${APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

if(NOT ARIBMON_BUILD_TESTS)
    return()
endif()

enable_testing()
add_executable(test-caption-layout tests/test_caption_layout.cpp)
target_link_libraries(test-caption-layout PRIVATE aribcaption::aribcaption)
add_test(NAME caption-layout COMMAND test-caption-layout)

qt_add_executable(test-validation
    tests/test_validation.cpp
    src/AribInspector.cpp
    src/CaptionValidator.cpp
    src/TsDemux.cpp
)
target_include_directories(test-validation PRIVATE src)
target_link_libraries(test-validation PRIVATE Qt6::Core)
add_test(NAME validation COMMAND test-validation)
