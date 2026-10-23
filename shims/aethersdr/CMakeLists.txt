# The vendored AetherSDR closure (vendor/aethersdr, byte-identical upstream files plus patches P1/P2,
# master §2.2) built against the bridge's shims (master §3.2, WS-2).
#
#   aethersdr_sqlite   the SQLite amalgamation (C), upstream's compile options
#   aethersdr_shims    AppSettings + LogManager shims (bridge code, bridge warnings)
#   aethersdr_canon    CanonWindow, FramelessResizer, ModemChrome, ThemeManager (+ the generated seed)
#                      and the bridge's ThemeManagerHost; carries the GUI resources (resources/asb.qrc)
#
# Include order, everywhere the vendored files are compiled (§2.2): shims/aethersdr/src/core,
# shims/aethersdr/src, vendor/aethersdr/src. A quoted include searches the includer's own directory
# first, so the vendored tree must never hold an AppSettings.h, LogManager.h or ThemeManagerHost.h
# (FILES does not list them, and the sync tool refuses unlisted files).

set(ASB_AETHERSDR_DIR "${CMAKE_SOURCE_DIR}/vendor/aethersdr" CACHE INTERNAL "")
set(ASB_AETHERSDR_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
set(ASB_AETHERSDR_INCLUDES
    "${ASB_AETHERSDR_SHIM_DIR}/src/core"
    "${ASB_AETHERSDR_SHIM_DIR}/src"
    "${ASB_AETHERSDR_DIR}/src"
    CACHE INTERNAL "")

# Upstream's own warning flags for upstream sources (AetherSDR CMakeLists.txt @ 34363a38, the
# first-party targets): never -Werror, because a vendored file cannot be edited to satisfy a newer
# compiler. /utf-8 matters: the sources contain non-ASCII text.
add_library(aethersdr_upstream_flags INTERFACE)
if(MSVC)
    target_compile_options(aethersdr_upstream_flags INTERFACE
        /W3 /Zc:__cplusplus /permissive- /utf-8 /bigobj)
else()
    target_compile_options(aethersdr_upstream_flags INTERFACE -Wall -Wextra -Wpedantic)
endif()

# --- aethersdr_sqlite ------------------------------------------------------------------------
find_package(Threads REQUIRED)
add_library(aethersdr_sqlite STATIC "${ASB_AETHERSDR_DIR}/third_party/sqlite/sqlite3.c")
target_include_directories(aethersdr_sqlite SYSTEM PUBLIC "${ASB_AETHERSDR_DIR}/third_party/sqlite")
# The same options as AetherSDR's vendored aether_sqlite3 target (see third_party/sqlite/README.md).
target_compile_definitions(aethersdr_sqlite PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DEFAULT_FILE_PERMISSIONS=0600
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE)
if(MSVC)
    target_compile_options(aethersdr_sqlite PRIVATE /w)
else()
    target_compile_options(aethersdr_sqlite PRIVATE -w)
endif()
target_link_libraries(aethersdr_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# --- aethersdr_shims -------------------------------------------------------------------------
add_library(aethersdr_shims STATIC
    src/core/AppSettings.cpp src/core/AppSettings.h
    src/core/LogManager.cpp src/core/LogManager.h
    src/core/ThemeManagerHost.h)
target_include_directories(aethersdr_shims SYSTEM PUBLIC ${ASB_AETHERSDR_INCLUDES})
target_link_libraries(aethersdr_shims PUBLIC Qt6::Core PRIVATE asb_warnings)

# --- resources (resources/asb.qrc) -----------------------------------------------------------
# An object library whose objects are INTERFACE sources of aethersdr_canon: every executable that
# links the canon compiles in the resource initializer itself. Inside a static archive nothing
# references it, the linker would drop it, and ":/themes" would silently be empty.
qt_add_resources(_asb_rcc_sources "${CMAKE_SOURCE_DIR}/resources/asb.qrc")
add_library(asb_resources OBJECT ${_asb_rcc_sources})
target_link_libraries(asb_resources PRIVATE Qt6::Core)

# --- aethersdr_canon -------------------------------------------------------------------------
set(ASB_AETHERSDR_THEME_SOURCES
    "${ASB_AETHERSDR_DIR}/src/core/ThemeManager.cpp"
    "${ASB_AETHERSDR_DIR}/src/core/ThemeManager.h"
    "${ASB_AETHERSDR_DIR}/src/core/ThemeSeedGenerated.cpp"
    CACHE INTERNAL "")
add_library(aethersdr_canon STATIC
    ${ASB_AETHERSDR_THEME_SOURCES}
    "${ASB_AETHERSDR_DIR}/src/gui/CanonIndicators.h"
    "${ASB_AETHERSDR_DIR}/src/gui/CanonWindow.cpp"
    "${ASB_AETHERSDR_DIR}/src/gui/CanonWindow.h"
    "${ASB_AETHERSDR_DIR}/src/gui/ComboStyle.h"
    "${ASB_AETHERSDR_DIR}/src/gui/FramelessMoveHelper.h"
    "${ASB_AETHERSDR_DIR}/src/gui/FramelessResizer.cpp"
    "${ASB_AETHERSDR_DIR}/src/gui/FramelessResizer.h"
    "${ASB_AETHERSDR_DIR}/src/gui/ModemChrome.cpp"
    "${ASB_AETHERSDR_DIR}/src/gui/ModemChrome.h"
    "${ASB_AETHERSDR_DIR}/src/gui/Theme.h"
    "${ASB_AETHERSDR_DIR}/src/gui/ThemedImageCache.h"
    # The bridge's P1 host: no user theme directory.
    src/core/ThemeManagerHost.cpp)
target_include_directories(aethersdr_canon SYSTEM PUBLIC ${ASB_AETHERSDR_INCLUDES})
target_link_libraries(aethersdr_canon
    PUBLIC aethersdr_shims Qt6::Widgets Qt6::Gui Qt6::Core
    PRIVATE aethersdr_upstream_flags)
target_sources(aethersdr_canon INTERFACE $<TARGET_OBJECTS:asb_resources>)
