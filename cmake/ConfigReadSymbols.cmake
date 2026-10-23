# ConfigReadSymbols.cmake: INV-20, only the trust layer reads configuration files.
#
# Every configuration read goes through TrustedConfig, a rule kept by linkage. Run as a script on a static library:
#
#   cmake -DNM=<nm> -DARCHIVE=<lib> -DALLOWED=<object file name> [-DREQUIRE_ALLOWED=ON] -P ConfigReadSymbols.cmake
#
# Every object in ARCHIVE that references (nm type U) asb::ConfigFiles::readCommitted, ::readPending or ::readNamed
# must be ALLOWED (TrustedConfig.cpp.o); any other one prints "ConfigReadSymbols: VIOLATION: <object>"
# and fails. With REQUIRE_ALLOWED, the allowed object must reference both functions, so a check that
# stopped seeing the symbols (mangling, a renamed function) fails instead of passing silently. A
# missing input or tool is an error, never a skip.
#
# GNU/LLVM nm only (the Linux, headless and distribution legs); the rule is about source code and is the
# same on every OS.

cmake_minimum_required(VERSION 3.22)
if(NOT NM OR NOT EXISTS "${NM}")
    message(FATAL_ERROR "ConfigReadSymbols: nm missing: '${NM}'")
endif()
if(NOT ARCHIVE OR NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "ConfigReadSymbols: archive missing: '${ARCHIVE}'")
endif()
execute_process(COMMAND "${NM}" -A "${ARCHIVE}" OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ConfigReadSymbols: ${NM} failed (${rc}): ${err}")
endif()

# Itanium mangling: asb::ConfigFiles::readCommitted -> _ZN3asb11ConfigFiles13readCommitted...
string(REPLACE "\n" ";" lines "${out}")
set(violations "")
set(allowedCommitted OFF)
set(allowedPending OFF)
foreach(line IN LISTS lines)
    if(NOT line MATCHES "ConfigFiles(13readCommitted|11readPending|9readNamed)")
        continue()
    endif()
    set(which "${CMAKE_MATCH_1}")
    if(NOT line MATCHES "^[^:]*:([^:]+):[ \t0-9a-fA-F]* U ")
        continue()   # a definition (ConfigFiles.cpp.o) or anything that is not a reference
    endif()
    set(object "${CMAKE_MATCH_1}")
    if(object STREQUAL ALLOWED)
        if(which STREQUAL "13readCommitted")
            set(allowedCommitted ON)
        elseif(which STREQUAL "11readPending")
            set(allowedPending ON)
        endif()
    else()
        list(APPEND violations "${object}")
    endif()
endforeach()

if(violations)
    list(REMOVE_DUPLICATES violations)
    foreach(v IN LISTS violations)
        message("ConfigReadSymbols: VIOLATION: ${v} references a ConfigFiles read function")
    endforeach()
    message(FATAL_ERROR "ConfigReadSymbols: only ${ALLOWED} may read configuration files (INV-20)")
endif()
if(REQUIRE_ALLOWED AND NOT (allowedCommitted AND allowedPending))
    message(FATAL_ERROR "ConfigReadSymbols: ${ALLOWED} does not reference both read functions; the check cannot see them")
endif()
message("ConfigReadSymbols: OK")
