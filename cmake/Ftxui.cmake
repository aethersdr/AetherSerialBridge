# Ftxui.cmake: fetch FTXUI (MIT) for the TUI.
#
# Pinned by release tag AND archive hash, so a moved tag or a changed archive fails the configure.
# Tag v7.0.3 is commit f921fad208912747c17d129a8ef75ec7624b6eec; Loop::RunOnce() (D16) is in
# include/ftxui/component/loop.hpp.
#
# FETCHCONTENT_SOURCE_DIR_FTXUI (or FETCHCONTENT_FULLY_DISCONNECTED) lets an offline build use a
# checkout it already has.

include(FetchContent)

set(ASB_FTXUI_VERSION "v7.0.3")
set(ASB_FTXUI_SHA256 "e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88")

set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_MODULES  OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET          ON  CACHE BOOL "" FORCE)

# DOWNLOAD_EXTRACT_TIMESTAMP exists from CMake 3.24 (it silences policy CMP0135); the project's
# declared minimum is 3.22, where the keyword is an unknown argument and the declaration fails.
set(_asb_ftxui_extract_opts "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(_asb_ftxui_extract_opts DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_Declare(ftxui
    URL "https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/${ASB_FTXUI_VERSION}.tar.gz"
    URL_HASH "SHA256=${ASB_FTXUI_SHA256}"
    ${_asb_ftxui_extract_opts})
FetchContent_MakeAvailable(ftxui)
