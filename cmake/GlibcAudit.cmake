# GlibcAudit.cmake: the glibc floor of a Linux binary (INV-87).
#
# Release Linux binaries are built in ubuntu:22.04 so they run on glibc 2.35. This script reads the
# dynamic symbol table of one binary and fails when it needs a newer GLIBC_ version than the limit.
# WS-1 WIRES it (CTest "glibc_audit.<target>", registered when ASB_GLIBC_MAX is set, which the
# 22.04 build container does); WS-13 enforces it on the release artifacts and may not weaken it.
#
#   asb_add_glibc_audit(<target>)
#
# Script mode (cmake -P):  -DEXE=<file> -DOBJDUMP=<objdump> -DMAX=<x.y>
# The reference check is AetherSDR's appimage.yml (objdump -T, highest GLIBC_ requirement).

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    cmake_minimum_required(VERSION 3.22)
    foreach(_v EXE OBJDUMP MAX)
        if(NOT ${_v})
            message(FATAL_ERROR "glibc_audit: -D${_v} is required")
        endif()
    endforeach()
    if(NOT EXISTS "${EXE}")
        message(FATAL_ERROR "glibc_audit: no such file: ${EXE}")
    endif()
    execute_process(COMMAND "${OBJDUMP}" -T "${EXE}"
                    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "glibc_audit: objdump -T failed (${_rc}): ${_err}")
    endif()
    string(REGEX MATCHALL "GLIBC_[0-9]+(\\.[0-9]+)+" _all "${_out}")
    set(_max_seen "0")
    foreach(_g IN LISTS _all)
        string(SUBSTRING "${_g}" 6 -1 _ver)
        if(_ver VERSION_GREATER _max_seen)
            set(_max_seen "${_ver}")
        endif()
    endforeach()
    get_filename_component(_name "${EXE}" NAME)
    if(NOT _all)
        message(FATAL_ERROR "glibc_audit: ${_name} has no GLIBC_ version requirements at all; "
                            "this is not a dynamically linked glibc binary, so the audit proves nothing")
    endif()
    if(_max_seen VERSION_GREATER MAX)
        message(FATAL_ERROR "glibc_audit: ${_name} needs GLIBC_${_max_seen}, above the limit ${MAX}")
    endif()
    message(STATUS "glibc_audit: ${_name} max GLIBC_${_max_seen} <= ${MAX}")
    return()
endif()

find_program(ASB_OBJDUMP NAMES objdump)

function(asb_add_glibc_audit target)
    if(NOT ASB_GLIBC_MAX)
        return()
    endif()
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "ASB_GLIBC_MAX is set, but the glibc audit only applies to Linux")
    endif()
    if(NOT ASB_OBJDUMP)
        message(FATAL_ERROR "ASB_GLIBC_MAX is set, but objdump was not found")
    endif()
    add_test(NAME glibc_audit.${target}
             COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:${target}>" "-DOBJDUMP=${ASB_OBJDUMP}"
                     "-DMAX=${ASB_GLIBC_MAX}" -P "${CMAKE_SOURCE_DIR}/cmake/GlibcAudit.cmake")
    set_tests_properties(glibc_audit.${target} PROPERTIES LABELS "glibc-audit")
endfunction()
