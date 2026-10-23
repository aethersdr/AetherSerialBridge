# WS-6: the Windows data-folder trust adapter, apply and engine locks, FileIo and the log file.
#
# asb_win_adapters is the adapter LOGIC over the NtSeams seam: portable C++ with no Windows headers, built on
# every OS, so its seam tests and mutants run on Linux as well (tests/tst_WindowsDataFolder, ...). The real
# seam (NtSeams.cpp), the job-object process guard and the production paths are asb_platform_win, Windows only.
add_library(asb_win_adapters STATIC
    NtSeams.h
    SecurityDescriptor.cpp SecurityDescriptor.h
    WinFileIo.cpp WinFileIo.h
    WindowsDataFolder.cpp WindowsDataFolder.h
    LockFiles.cpp LockFiles.h
    FileApplyLock.cpp FileApplyLock.h
    EngineLock.cpp EngineLock.h
    WinLogFile.cpp WinLogFile.h
)
target_include_directories(asb_win_adapters PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(asb_win_adapters PUBLIC asb_core PRIVATE asb_warnings)
if(NOT MSVC)
    # Closer to MSVC /W4 on the Linux legs, for this security code only.
    target_compile_options(asb_win_adapters PRIVATE -Wshadow -Wconversion -Wsign-conversion)
endif()

# WS-7: the service host's decisions, the SCM adapter and the CIM firewall, as portable logic over native seams
# (ServiceStatus reporter, ICimSession, ...), built on every OS so the WS-7 seam tests and mutants run on Linux.
add_library(asb_win_service STATIC
    ServiceStatus.cpp ServiceStatus.h
    ServiceExit.cpp ServiceExit.h
    ServiceLifecycle.cpp ServiceLifecycle.h
    FirewallCompare.cpp FirewallCompare.h
    CimSeams.h
    CimFirewall.cpp CimFirewall.h
    ScmSeams.h
    WindowsService.cpp WindowsService.h
    InstallBinary.cpp InstallBinary.h
    DataFolderStatusReader.h
    UninstallEntry.cpp UninstallEntry.h
)
target_include_directories(asb_win_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(asb_win_service PUBLIC asb_core asb_win_adapters PRIVATE asb_warnings)

# WS-7: the process-hardening policy of both executables (INV-67). Portable (a no-op off Windows).
add_library(asb_process_policy STATIC ProcessPolicy.cpp ProcessPolicy.h)
target_include_directories(asb_process_policy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(asb_process_policy PUBLIC Qt6::Core PRIVATE asb_warnings)

if(WIN32)
    # asb_platform_win: data folder trust, SCM, CIM firewall, locks (WS-6, WS-7).
    add_library(asb_platform_win STATIC
        PlatformInfo.cpp PlatformInfo.h
        NativeSeams.h NtSeams.cpp ProcessGuard.cpp
        # WS-7: the native SCM and WMI pass-throughs and the Applier's Platform.
        ScmSeams.cpp WbemSession.cpp WbemSession.h WindowsPlatform.cpp WindowsPlatform.h
        # WS-16 (D64): the one object that may call ShellExecuteExW (process_creation_audit).
        SelfElevate.cpp SelfElevate.h)
    target_include_directories(asb_platform_win PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_link_libraries(asb_platform_win
        PUBLIC asb_core asb_win_adapters asb_win_service advapi32 ntdll wbemuuid ole32 oleaut32 shell32
        PRIVATE asb_warnings)
    if(ASB_BUILD_TESTS)
        # WS-16: ASB_TEST_FAIL_LOCK_DISPOSITION (NtSeams.cpp) and ASB_TEST_FAIL_PROGRAM_REMOVAL (WindowsPlatform.cpp) exist
        # only when the tests are built; release_audit.py seams proves a release exe has neither name.
        target_compile_definitions(asb_platform_win PRIVATE ASB_TEST_SEAMS)
        # v1.1 station fix: the relaunch seam that starts a real child (tst_GuiNative::standardUserWindow(real-child));
        # the one test-build object besides GuardProbe.cpp that may call CreateProcessW (process_creation_audit).
        target_sources(asb_platform_win PRIVATE SelfElevateTestChild.cpp)
    endif()

    # WS-7: the --service host. Linked ONLY into the headless executable (it contains the engine, INV-1).
    add_library(asb_win_service_host STATIC ServiceMain.cpp ServiceMain.h)
    target_include_directories(asb_win_service_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_link_libraries(asb_win_service_host PUBLIC asb_platform_win asb_win_service asb_process_policy asb_engine
                          PRIVATE asb_warnings)
    if(ASB_BUILD_TESTS)
        # The service's test seams (ServiceMain.cpp): present only when the tests are built; a release
        # configuration (ASB_BUILD_TESTS=OFF) has none.
        target_compile_definitions(asb_win_service_host PRIVATE ASB_TEST_SEAMS)
    endif()
endif()
