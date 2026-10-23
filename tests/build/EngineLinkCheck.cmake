# EngineLinkCheck.cmake (WS-4, INV-1): an executable must not contain the session engine.
#
#   cmake -DGRAPH=<link closure file> [-DEXE=<file> -DNM=<nm>] -DMODE=check|control -P EngineLinkCheck.cmake
#
# Two layers:
#   graph     the CMake link closure (written at configure time by asb_write_link_graphs) reaches asb_engine
#   artifact  ELF only: the binary defines asb::engine:: symbols (nm -C). Skipped when NM/EXE are not
#             given (Windows: the graph layer is the check there).
# MODE=check fails ("EngineLinkCheck: VIOLATION") when any layer finds the engine; MODE=control fails
# unless EVERY layer run finds it. A missing input is an error, never a pass. With -DTEST_NAME=<name>, a
# violation also prints QtTest's failure line "FAIL!  : <name>() ...", so a run log names this
# check like any QtTest function.
cmake_minimum_required(VERSION 3.22)
if(NOT GRAPH OR NOT EXISTS "${GRAPH}")
    message(FATAL_ERROR "EngineLinkCheck: GRAPH missing: '${GRAPH}'")
endif()
if(NOT MODE)
    set(MODE check)
endif()
set(_layers graph)
set(_found "")
file(STRINGS "${GRAPH}" _closure)
if("asb_engine" IN_LIST _closure)
    list(APPEND _found graph)
endif()
if(NM AND EXE)
    if(NOT EXISTS "${EXE}")
        message(FATAL_ERROR "EngineLinkCheck: EXE missing: '${EXE}'")
    endif()
    list(APPEND _layers artifact)
    execute_process(COMMAND "${NM}" -C --defined-only "${EXE}" OUTPUT_VARIABLE _syms RESULT_VARIABLE _rc ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "EngineLinkCheck: nm failed on ${EXE}")
    endif()
    if(_syms MATCHES "asb::engine::")
        list(APPEND _found artifact)
    endif()
endif()
get_filename_component(_name "${GRAPH}" NAME_WE)
if(MODE STREQUAL "check")
    if(_found)
        if(TEST_NAME)
            message("FAIL!  : ${TEST_NAME}() ${_name} contains the engine (layers: ${_found})")
        endif()
        message(FATAL_ERROR "EngineLinkCheck: VIOLATION: ${_name} contains the engine (layers: ${_found})")
    endif()
    message(STATUS "EngineLinkCheck: OK: ${_name} (layers checked: ${_layers})")
else()
    foreach(_l IN LISTS _layers)
        if(NOT _l IN_LIST _found)
            message(FATAL_ERROR "EngineLinkCheck: CONTROL NOT DETECTED by layer ${_l} for ${_name}")
        endif()
    endforeach()
    message(STATUS "EngineLinkCheck: control ${_name} detected by every layer (${_layers})")
endif()
