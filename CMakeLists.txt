cmake_minimum_required(VERSION 3.22)

project(AetherSerialBridge
    VERSION 1.2.0
    DESCRIPTION "Aether Serial Bridge: serial-device-to-TCP bridge for AetherSDR"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------------------------
option(ASB_BUILD_GUI   "Build the GUI executable (needs Qt >= the GUI floor, Widgets)" ON)
option(ASB_BUILD_TESTS "Build the tests, including the test-build seams in the executables" ON)
option(ASB_WERROR      "Treat compiler warnings as errors" OFF)
option(ASB_REQUIRE_PINNED_QT
    "Fail unless the Qt found is exactly the version pinned in cmake/qt-pin.env" OFF)
set(ASB_GLIBC_MAX "" CACHE STRING
    "Linux only: register glibc_audit tests that fail when a binary needs a newer GLIBC_ than this (e.g. 2.35)")
# Where the project lives on GitHub, as owner/name: the one place the code takes it from (the systemd unit's
# Documentation= link). tools/set_repo.py rewrites this default and every user-facing doc together; its --check, in
# static-checks, fails when a doc names another repository. Checked here because the value is written into a unit file.
set(ASB_REPO "AetherSDR/AetherSerialBridge" CACHE STRING
    "The project's GitHub repository, owner/name (the systemd unit's Documentation= link)")
if(NOT ASB_REPO MATCHES "^[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "ASB_REPO must be owner/name (letters, digits, '-', and '.' or '_' in the name): '${ASB_REPO}'")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Every executable and test lands in one directory so Windows finds the Qt DLLs and the tests find
# the executables by a stable relative path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Warnings are an interface target our own targets link, not a directory-wide add_compile_options:
# FetchContent'd FTXUI is configured in this directory and must not inherit -Werror.
add_library(asb_warnings INTERFACE)
if(MSVC)
    target_compile_options(asb_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8)
    if(ASB_WERROR)
        target_compile_options(asb_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(asb_warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(ASB_WERROR)
        target_compile_options(asb_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------------------------
# v1.1 fix (the window on stock Ubuntu): Qt6::Gui's link interface carries WrapOpenGL -> OpenGL::GL, which CMake's
# FindOpenGL resolves, by default (CMP0072 NEW), to GLVND's libOpenGL.so + libGLX.so, so both are inputs on the
# window's link line. Ubuntu's gcc links with --as-needed, under which ld (binutils 2.38) records DT_NEEDED for an
# input library that satisfies an undefined reference from a regular object, OR one from another needed shared
# library when the input is not already in some needed library's DT_NEEDED list. libQt6Gui leaves glGetString and
# glXChooseFBConfig undefined (its own DT_NEEDED libGL.so.1 provides them at run time); libOpenGL.so.0 and libGLX.so.0
# are in no needed library's DT_NEEDED list, so ld kept both for those references (the window's link map:
# "libOpenGL.so.0  .../libQt6Gui.so.6.12.0 (glGetString)", "libGLX.so.0 ... (glXChooseFBConfig)"). The window then
# NEEDed libOpenGL.so.0, which a stock Ubuntu desktop does not install (libopengl0) and the AppImage must not bundle
# (glvnd dispatch comes from the host), and did not start at all (found 2026-10-19 on a stock Ubuntu 26.04.1 VM).
# LEGACY makes OpenGL::GL libGL.so instead: its soname libGL.so.1 IS in libQt6Gui's DT_NEEDED list, so the second
# clause does not apply and, the window referencing no GL symbol itself, ld records nothing; the window needs no GL
# library of its own (Qt's libraries keep their own NEEDs). Reproduced in asb-build-2204 with a two-file program: a
# library NEEDing libGL.so.1 that calls glGetString, linked with libOpenGL.so (kept) and with libGL.so (dropped).
# Linux only; FindOpenGL ignores it elsewhere. release.yml's host-libs gate (tools/release_audit.py) and the
# stock-desktop start are the guards.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OpenGL_GL_PREFERENCE LEGACY)
endif()
include(QtPin)
asb_find_qt()

include(NoQtGuiCheck)
include(HeadlessDigest)
include(ImportAudit)
include(DllPolicy)
include(GlibcAudit)
include(Ftxui)
include(ThemeWriterCheck)

if(ASB_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------------------------
# Libraries (stubs: each later workstream fills its own)
# ---------------------------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/engine)
# The Windows adapter logic (asb_win_adapters) builds everywhere; the native Windows library only on Windows.
add_subdirectory(src/platform/win)
if(NOT WIN32)
    add_subdirectory(src/platform/posix)
endif()
add_subdirectory(src/cli)
add_subdirectory(src/tui)
if(ASB_BUILD_GUI)
    # aethersdr_canon / aethersdr_sqlite: the vendored closure (vendor/aethersdr, upstream files
    # only, master §2.2) is built from shims/aethersdr, next to the shims it compiles against.
    add_subdirectory(shims/aethersdr)
    add_subdirectory(src/gui)
endif()

# ---------------------------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------------------------
add_subdirectory(src/app)

if(ASB_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# The graph layer on its own, against every way a dependency can be spelled (INV-37). Here, not in
# tests/: see the header of tests/graph_shapes.cmake.
if(ASB_BUILD_TESTS AND ASB_HAVE_QT_GUI)
    include("${CMAKE_SOURCE_DIR}/tests/graph_shapes.cmake")
endif()

# Written last, once every target exists: the link closure of every target that registered with
# NoQtGuiCheck (the executables under check, the controls, the graph-shape fixtures), read by the
# graph layer of INV-37.
asb_write_link_graphs()
