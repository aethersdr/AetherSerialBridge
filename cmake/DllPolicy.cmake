# DllPolicy.cmake: the Windows DLL search policy of the executables (WS-7; master §4.11, D3, B10; INV-67).
#
#   asb_apply_dll_policy(<target> [ALWAYS])
#       Windows: links <target> with /DEPENDENTLOADFLAG:0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32): the loader resolves
#       the executable's static imports of non-KnownDLLs from System32 only (Windows 10 1607+), so a DLL planted
#       next to the executable is never loaded. Applied when Qt is linked statically (the shipped form, D3): a
#       dynamic-Qt development build imports Qt6*.dll from the build tree and could not start with it. ALWAYS
#       applies it regardless (the dllprobe fixture, which imports no Qt). Elsewhere: nothing.
#
#   asb_qt_is_static(<out-var>)   TRUE when Qt6::Core is a static library.

function(asb_qt_is_static out)
    set(_static FALSE)
    if(TARGET Qt6::Core)
        get_target_property(_type Qt6::Core TYPE)
        if(_type STREQUAL "STATIC_LIBRARY")
            set(_static TRUE)
        endif()
    endif()
    set(${out} ${_static} PARENT_SCOPE)
endfunction()

function(asb_apply_dll_policy target)
    cmake_parse_arguments(ARG "ALWAYS" "" "" ${ARGN})
    if(NOT WIN32 OR NOT MSVC)
        return()
    endif()
    asb_qt_is_static(_static)
    if(ARG_ALWAYS OR _static)
        target_link_options(${target} PRIVATE "/DEPENDENTLOADFLAG:0x800")
        set_property(TARGET ${target} PROPERTY ASB_DLL_POLICY ON)
    endif()
endfunction()
