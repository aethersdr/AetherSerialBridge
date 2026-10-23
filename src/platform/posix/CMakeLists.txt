# asb_platform_posix (WS-8): the Linux data folder, FileIo, locks, systemd over D-Bus, the process guard,
# the --service identity check and the port listing. No engine here: the GUI links this library through
# asb_cli and must never contain the engine (INV-1); the --service host itself is in src/app.
# QtDBus is configured -dbus-runtime in static builds: it dlopens libdbus-1, no child process.
add_library(asb_platform_posix STATIC
    PosixSys.cpp PosixSys.h
    PosixFileIo.cpp PosixFileIo.h
    PosixDataFolder.cpp PosixDataFolder.h
    LinuxLocks.cpp LinuxLocks.h
    NullFirewall.h
    SystemdBus.h
    QtSystemdBus.cpp QtSystemdBus.h
    SystemdUnit.cpp SystemdUnit.h
    SystemdService.cpp SystemdService.h
    ServiceIdentity.cpp ServiceIdentity.h
    SerialPortList.cpp SerialPortList.h
    ProcessGuard.cpp ProcessGuard.h
    LinuxPlatform.cpp LinuxPlatform.h
)
target_include_directories(asb_platform_posix PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(asb_platform_posix PUBLIC asb_core Qt6::DBus PRIVATE asb_warnings)
# The repository the unit's Documentation= line links to (top-level ASB_REPO).
target_compile_definitions(asb_platform_posix PRIVATE ASB_REPO="${ASB_REPO}")
# Security code: the stricter warnings the WS-6 adapters also build with.
target_compile_options(asb_platform_posix PRIVATE -Wshadow)
