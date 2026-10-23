# INV-1 (WS-4 deferred row, Linux --service half; WS-8): the serial device factory is registered only by the
# service hosts and `--run --config` (WS-9: app/ScratchEngine.cpp). Every source file that names engine::makeQtSerialDevice must be
# in ALLOWED; the engine's own definition is allowed by name. Prints "FactorySites: OK" or the offenders.
#   cmake -DSRC=<repo>/src -DALLOWED="app/ServiceHost_linux.cpp;..." -P FactorySites.cmake
file(GLOB_RECURSE sources RELATIVE "${SRC}" "${SRC}/*.cpp" "${SRC}/*.h")
set(bad "")
foreach(f IN LISTS sources)
    file(READ "${SRC}/${f}" text)
    string(FIND "${text}" "makeQtSerialDevice" at)
    if(at GREATER -1)
        if(f STREQUAL "engine/QtSerialDevice.cpp" OR f STREQUAL "engine/QtSerialDevice.h")
            continue()
        endif()
        list(FIND ALLOWED "${f}" ok)
        if(ok EQUAL -1)
            list(APPEND bad "${f}")
        endif()
    endif()
endforeach()
if(bad)
    message(STATUS "FactorySites: VIOLATION: makeQtSerialDevice referenced by ${bad}")
    message(FATAL_ERROR "FactorySites: the device factory is named outside the service hosts")
endif()
message(STATUS "FactorySites: OK")
