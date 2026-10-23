# SyscallGrep.cmake (WS-9, INV-112, master §4.6): no raw system call in bridge code. The symbol audit sees
# references to library functions; a raw `syscall(` or a `__NR_` number would bypass it (the runtime guards still
# apply), so the source is searched for both, outside comments. ALLOWED lists the files that may (the seccomp
# installer: src/platform/posix/ProcessGuard.cpp, whose filter needs the syscall numbers).
#   cmake -DSRC=<repo>/src -DALLOWED="platform/posix/ProcessGuard.cpp" [-DTEST_NAME=<name>] -P SyscallGrep.cmake
# Prints "SyscallGrep: OK" or one "SyscallGrep: VIOLATION: <file>" per offender.
cmake_minimum_required(VERSION 3.22)
file(GLOB_RECURSE sources RELATIVE "${SRC}" "${SRC}/*.cpp" "${SRC}/*.h" "${SRC}/*.c")
set(bad "")
foreach(f IN LISTS sources)
    list(FIND ALLOWED "${f}" ok)
    if(NOT ok EQUAL -1)
        continue()
    endif()
    file(READ "${SRC}/${f}" text)
    # Comments are not code: block comments, then line comments.
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" text "${text}")
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")
    if(text MATCHES "(^|[^A-Za-z0-9_])syscall[ \t]*\\(" OR text MATCHES "__NR_")
        list(APPEND bad "${f}")
    endif()
endforeach()
if(bad)
    foreach(b IN LISTS bad)
        message("SyscallGrep: VIOLATION: ${b}")
    endforeach()
    if(TEST_NAME)
        list(GET bad 0 first)
        message("FAIL!  : ${TEST_NAME}() raw syscall in ${first}")
    endif()
    message(FATAL_ERROR "SyscallGrep: raw system calls outside the allow-list (INV-112)")
endif()
message(STATUS "SyscallGrep: OK")
