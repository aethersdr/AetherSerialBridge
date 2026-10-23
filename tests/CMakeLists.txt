# Tests. Every later workstream adds its own here.

set(ASB_TEST_ENV "ASB_HEADLESS_EXE=$<TARGET_FILE:aether-serial-bridge>")
if(ASB_BUILD_GUI)
    list(APPEND ASB_TEST_ENV "ASB_GUI_EXE=$<TARGET_FILE:${ASB_GUI_TARGET}>")
endif()

function(asb_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Core asb_app_common asb_warnings)
    # "-o -,txt": QtTest writes to stdout explicitly (on Windows the default was not captured by CTest). The second -o
    # writes the results to a file too: on the Windows leg QtTest's stdout does not reach ctest, and the failure step
    # prints every <test>.ctest.txt with a failure in it (as the GUI tests below already do).
    add_test(NAME ${name} COMMAND ${name} -o -,txt -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.ctest.txt,txt)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ASB_TEST_ENV}" LABELS "unit;process")
    add_dependencies(${name} aether-serial-bridge)
    if(ASB_BUILD_GUI)
        add_dependencies(${name} ${ASB_GUI_TARGET})
    endif()
endfunction()

asb_add_test(tst_Identity)   # INV-70
asb_add_test(tst_Dispatch)

# --- GUI-side tests (WS-2 onward): need QtWidgets and the vendored canon ---------------------
if(ASB_BUILD_GUI)
    function(asb_add_gui_test name)
        add_executable(${name} ${name}.cpp ${ARGN})
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Widgets aethersdr_canon asb_warnings)
        # The second -o writes the per-function results to a file as well: ctest hides a passing
        # test's output, and on the Windows leg QtTest's stdout has not reached ctest at all, so
        # the CI step that runs after a failed ctest prints this file and names the failing function.
        add_test(NAME ${name} COMMAND ${name} -o -,txt -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.ctest.txt,txt)
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;gui")
    endfunction()

    # The theme probe (tst_ThemePatches, INV-52), with the bridge's P1 host ...
    add_executable(asb_theme_probe support/ThemeProbe.cpp support/ThemeFixtures.h)
    target_include_directories(asb_theme_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(asb_theme_probe PRIVATE Qt6::Widgets aethersdr_canon asb_warnings)

    # ... and with AetherSDR's own host: the positive control. The same vendored ThemeManager
    # sources, compiled again without the bridge's ThemeManagerHost.cpp.
    add_executable(asb_theme_probe_aetherhost
        support/ThemeProbe.cpp support/ThemeFixtures.h support/AetherSdrThemeHost.cpp
        ${ASB_AETHERSDR_THEME_SOURCES} $<TARGET_OBJECTS:asb_resources>)
    target_include_directories(asb_theme_probe_aetherhost SYSTEM PRIVATE ${ASB_AETHERSDR_INCLUDES})
    target_include_directories(asb_theme_probe_aetherhost PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(asb_theme_probe_aetherhost PRIVATE
        Qt6::Widgets aethersdr_shims aethersdr_upstream_flags)

    asb_add_gui_test(tst_ThemePatches support/ThemeFixtures.h)       # INV-52, INV-84
    target_compile_definitions(tst_ThemePatches PRIVATE
        ASB_AETHERSDR_DIR="${ASB_AETHERSDR_DIR}")
    add_dependencies(tst_ThemePatches asb_theme_probe asb_theme_probe_aetherhost)
    set_tests_properties(tst_ThemePatches PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;ASB_THEME_PROBE=$<TARGET_FILE:asb_theme_probe>;ASB_THEME_PROBE_AETHERHOST=$<TARGET_FILE:asb_theme_probe_aetherhost>")
    asb_add_gui_test(tst_AppSettingsShim)
    asb_add_gui_test(tst_CanonBody)
    target_link_libraries(tst_CanonBody PRIVATE asb_gui)

    # --- Theme following (WS-11): INV-31, 36, 53, 85, 86, 95 and INV-52's follower gate -----
    find_package(Python3 REQUIRED COMPONENTS Interpreter)  # the reference validator (oracle)

    # The stalling wrapper VFS (C). Its own warning flags: the bridge's set carries C++-only
    # options.
    add_library(asb_test_slowvfs STATIC support/SlowVfs.c support/SlowVfs.h)
    target_include_directories(asb_test_slowvfs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
    target_link_libraries(asb_test_slowvfs PUBLIC aethersdr_sqlite)
    if(MSVC)
        target_compile_options(asb_test_slowvfs PRIVATE /W4 $<$<BOOL:${ASB_WERROR}>:/WX>)
    else()
        target_compile_options(asb_test_slowvfs PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${ASB_WERROR}>:-Werror>)
    endif()

    # AetherSDR's own Save As, producing the schema-2 + baseTheme fixture.
    add_executable(asb_theme_saveas_gen support/SaveAsFixtureGen.cpp)
    target_link_libraries(asb_theme_saveas_gen PRIVATE Qt6::Widgets aethersdr_canon asb_warnings)

    asb_add_gui_test(tst_ThemeFollower)
    target_link_libraries(tst_ThemeFollower PRIVATE asb_gui asb_test_slowvfs aethersdr_sqlite)
    target_compile_definitions(tst_ThemeFollower PRIVATE ASB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_dependencies(tst_ThemeFollower asb_theme_saveas_gen)
    set_tests_properties(tst_ThemeFollower PROPERTIES TIMEOUT 300 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;ASB_PYTHON=${Python3_EXECUTABLE};ASB_SAVEAS_GEN=$<TARGET_FILE:asb_theme_saveas_gen>")
    # ctest -j: on Windows, Qt's test mode puts GenericConfigLocation in one per-user folder (...\qttest), and both
    # tests write and remove files in its AetherSDR\themes (tst_ThemePatches and its asb_theme_probe plant
    # asb-planted-user-theme.json there; asb_theme_saveas_gen saves and removes "ASB Saved Light.json"). Never together.
    set_property(TEST tst_ThemePatches tst_ThemeFollower APPEND PROPERTY RESOURCE_LOCK asb-qttest-user-themes)

    # The oracle still gives the recorded result for every fixture (the Qt side is checked by
    # tst_ThemeFollower::oracleFixturesAgree against the same manifest).
    add_test(NAME theme_validator_oracle
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/theme_validator_ref.py"
                     --check-manifest "${CMAKE_SOURCE_DIR}/tests/data/themes")
    set_tests_properties(theme_validator_oracle PROPERTIES LABELS "unit;gui")

    # INV-31 writer-symbol tripwire: the follower's objects reference none of the listed writer
    # APIs, and a control object that references several of them is rejected with every one seen.
    # (A tripwire, not a proof of read-only behaviour: see the header of ThemeWriterCheck.cmake.)
    asb_add_theme_writer_check(asb_theme ThemeWriterCheck.asb_theme)
    add_library(theme_writer_control_bad OBJECT build/theme_writer_control_bad.cpp)
    target_link_libraries(theme_writer_control_bad PRIVATE aethersdr_canon aethersdr_sqlite)
    set(_twc_expect "ThemeManager:setColor" "fopen|fopen64" "freopen|freopen64")
    if(MSVC)
        list(APPEND _twc_expect __std_fs_remove __std_fs_rename _wfopen CreateFileA)
    else()
        list(APPEND _twc_expect "filesystem:remove" "filesystem:rename")
    endif()
    asb_add_theme_writer_check(theme_writer_control_bad ThemeWriterCheck.theme_writer_control_bad_rejected
                               CONTROL EXPECT ${_twc_expect})

    # AetherSDR's own canon_window_test, vendored unmodified, built against the shims.
    add_executable(canon_window_test "${ASB_AETHERSDR_DIR}/tests/canon_window_test.cpp")
    target_link_libraries(canon_window_test PRIVATE
        aethersdr_canon Qt6::Widgets Qt6::Gui Qt6::Test aethersdr_upstream_flags)
    add_test(NAME canon_window_test COMMAND canon_window_test)
    set_tests_properties(canon_window_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "gui;vendored")
endif()

# INV-37 control: a target that DOES contain QtGui code. NoQtGuiCheck must reject it, on every
# layer, or the check has stopped seeing QtGui and its pass on the real executable means nothing.
# Skipped only where there is no QtGui to link (a -no-gui Qt, WS-13).
if(ASB_HAVE_QT_GUI)
    add_executable(nogui_control_bad build/nogui_control_bad.cpp)
    target_link_libraries(nogui_control_bad PRIVATE Qt6::Gui Qt6::Core asb_warnings)
    asb_add_nogui_control(nogui_control_bad)
else()
    message(WARNING "QtGui not available: the NoQtGuiCheck control target is NOT built")
endif()

# ---------------------------------------------------------------------------------------------
# WS-3: config model, validation, AtomicJson, status types (unit tests on asb_core, no executables).
# ---------------------------------------------------------------------------------------------
add_library(asb_test_support STATIC
    support/FakeFileIo.cpp support/FakeFileIo.h
    support/FileIoContract.cpp support/FileIoContract.h
    support/TestPrinters.h)
target_include_directories(asb_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asb_test_support PUBLIC asb_core Qt6::Test PRIVATE asb_warnings)

function(asb_add_core_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE asb_test_support asb_core Qt6::Test Qt6::Core asb_warnings)
    target_compile_definitions(${name} PRIVATE ASB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME ${name} COMMAND ${name} -o -,txt)
    set_tests_properties(${name} PROPERTIES LABELS "unit")
endfunction()

asb_add_core_test(tst_Config)          # INV-30 (+ the INV-12 name fence rows)
asb_add_core_test(tst_AllowedClients)  # INV-14
asb_add_core_test(tst_AtomicJson)      # INV-15, INV-77, INV-96 (seam)
asb_add_core_test(tst_EditTracker)
asb_add_core_test(tst_AppLocations)
asb_add_core_test(tst_StatusPrinter)
asb_add_core_test(tst_StatusStore)
asb_add_core_test(tst_RunningTime)     # README nit WS-3/WS-4: running-time mapping; watchdog state rules
asb_add_core_test(tst_BuildStamp)      # v1.1 D61, INV-120: the build stamp (both OSes: ELF and PE)
asb_add_core_test(tst_InstallationEvidence)   # v1.1 INV-120: build identity in the classifier (both OSes)

# ---------------------------------------------------------------------------------------------
# WS-5: trust logic, Applier transaction, runner (fakes for every platform seam; no native calls).
# ---------------------------------------------------------------------------------------------
add_library(asb_ws5_fakes STATIC support/Fakes.cpp support/Fakes.h)
target_link_libraries(asb_ws5_fakes PUBLIC asb_test_support asb_core Qt6::Test PRIVATE asb_warnings)

function(asb_add_ws5_test name)
    asb_add_core_test(${name})
    target_link_libraries(${name} PRIVATE asb_ws5_fakes)
    # Threads and gates: a broken test must fail, never hang the run.
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

asb_add_ws5_test(tst_AceJudge)          # INV-19 judgement (Unknown ACEs)
asb_add_ws5_test(tst_DataDirectoryTrust)# INV-63 regular-file rule
asb_add_ws5_test(tst_TrustedConfig)     # INV-21, INV-22, INV-44, INV-74 (mode table)
asb_add_ws5_test(tst_FirewallPlan)      # INV-12 (typed values), INV-11 plan order
asb_add_ws5_test(tst_ServicePlan)       # INV-77 flushAfterStartType (WS-3 deferred row)
asb_add_ws5_test(tst_StartToken)        # INV-113 token format, revocation, read
asb_add_ws5_test(tst_FilesystemPolicy)  # INV-116 supported file systems
asb_add_ws5_test(tst_InstallationStates)# INV-98 installation states
asb_add_ws5_test(tst_Applier)           # INV-24/25/29/39/43/46/47/49/63/73/74/76/91/92/98/99/102/106/108/109/113
asb_add_ws5_test(tst_OperationHost)     # INV-118 operation lifetime
asb_add_ws5_test(tst_BridgeRunner)      # INV-8/34/43/103/113/119 runner
asb_add_ws5_test(tst_LockIdentity)      # INV-116 lock identity and pair states
asb_add_ws5_test(tst_IrreversibleGate)  # WS-5 round 3: one gate for every irreversible step (audit (b) as rows)
# The bypass scan reads the transaction's sources.
target_compile_definitions(tst_IrreversibleGate PRIVATE ASB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# INV-20: only TrustedConfig.cpp.o references the ConfigFiles read functions (nm on the archive), and a
# control library with a direct read must be rejected. GNU/LLVM nm toolchains only.
if(NOT MSVC)
    if(NOT CMAKE_NM)
        message(FATAL_ERROR "INV-20 check: CMAKE_NM is not set")
    endif()
    add_library(inv20_control STATIC build/inv20_control.cpp)
    target_link_libraries(inv20_control PRIVATE asb_core asb_warnings)
    add_test(NAME inv20_config_read_symbols
             COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DARCHIVE=$<TARGET_FILE:asb_core>
                     -DALLOWED=TrustedConfig.cpp.o -DREQUIRE_ALLOWED=ON
                     -P "${CMAKE_SOURCE_DIR}/cmake/ConfigReadSymbols.cmake")
    set_tests_properties(inv20_config_read_symbols PROPERTIES PASS_REGULAR_EXPRESSION "ConfigReadSymbols: OK"
                         LABELS "unit;build")
    add_test(NAME inv20_control_rejected
             COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DARCHIVE=$<TARGET_FILE:inv20_control>
                     -DALLOWED=TrustedConfig.cpp.o -P "${CMAKE_SOURCE_DIR}/cmake/ConfigReadSymbols.cmake")
    set_tests_properties(inv20_control_rejected PROPERTIES
                         PASS_REGULAR_EXPRESSION "ConfigReadSymbols: VIOLATION: inv20_control.cpp.o" LABELS "unit;build")
    # ctest does not build targets: make sure the control library exists when the tests run.
    add_dependencies(tst_TrustedConfig inv20_control)
    # WS-9: the CLI and the scratch engine read configuration only through TrustedConfig: no object in
    # either archive references a ConfigFiles read function.
    foreach(_lib asb_cli asb_scratch_engine)
        add_test(NAME inv20_config_read_symbols.${_lib}
                 COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DARCHIVE=$<TARGET_FILE:${_lib}> -DALLOWED=no-object-allowed
                         -P "${CMAKE_SOURCE_DIR}/cmake/ConfigReadSymbols.cmake")
        set_tests_properties(inv20_config_read_symbols.${_lib} PROPERTIES PASS_REGULAR_EXPRESSION "ConfigReadSymbols: OK"
                             LABELS "unit;build")
    endforeach()
endif()

# ---------------------------------------------------------------------------------------------
# WS-6: the Windows data-folder adapter, the two locks, WinFileIo and the log file. The adapter logic runs on
# the recording NtSeams fake on every OS (so its mutants run on Linux); the native rows run on windows-latest
# (tests/win/integration, label win-integration).
# ---------------------------------------------------------------------------------------------
add_library(asb_ws6_fakes STATIC support/FakeNtSeams.cpp support/FakeNtSeams.h support/WinFakeFixture.h)
target_link_libraries(asb_ws6_fakes PUBLIC asb_win_adapters asb_test_support PRIVATE asb_warnings)

function(asb_add_ws6_test name)
    asb_add_ws5_test(${name})
    target_link_libraries(${name} PRIVATE asb_ws6_fakes)
endfunction()

asb_add_ws6_test(tst_WindowsDataFolder)   # INV-16, 17, 19, 40, 62, 74, 116 (seam); raw descriptor decoding
asb_add_ws6_test(tst_WinFileIo)           # INV-15 FileIoContract (seam), INV-75/96 create-time owner, INV-77
asb_add_ws6_test(tst_FileApplyLock)       # INV-66, 97, 104 (seam): apply.lock and engine.lock

# ---------------------------------------------------------------------------------------------
# WS-7: the Windows service host's decisions, the CIM firewall and the comparator, at their seams (every OS).
# ---------------------------------------------------------------------------------------------
add_library(asb_ws7_fakes STATIC support/FakeCim.cpp support/FakeCim.h support/FakeScm.cpp support/FakeScm.h)
target_include_directories(asb_ws7_fakes PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asb_ws7_fakes PUBLIC asb_win_service Qt6::Core PRIVATE asb_warnings)

function(asb_add_ws7_test name)
    asb_add_ws5_test(${name})
    target_link_libraries(${name} PRIVATE asb_ws7_fakes asb_win_service)
endfunction()

asb_add_ws7_test(tst_ServiceStatus)     # INV-60 table and transitions; INV-81/93 host order (seam)
asb_add_ws7_test(tst_FirewallCompare)   # INV-117 comparator (pure) and RemoteAddress normalisation
asb_add_ws7_test(tst_CimFirewall)       # INV-11 recipe order, INV-114 flags/deadline/exclusion (seam)
asb_add_ws7_test(tst_WindowsService)    # SCM plan, INV-77 flush, installation detection, INV-41 install copy (seam); WS-16 program removal

# WS-16: the Apps & features entry (INV-126): its values over a recording registry and the Applier's decisions about it
# over the WS-5 fakes (every OS); SelfElevate's argument quoting (D64).
add_executable(tst_UninstallEntry win/tst_UninstallEntry.cpp)
target_link_libraries(tst_UninstallEntry PRIVATE asb_test_support asb_ws5_fakes asb_win_service asb_core Qt6::Test Qt6::Core
                      asb_warnings)
add_test(NAME tst_UninstallEntry COMMAND tst_UninstallEntry -o -,txt)
set_tests_properties(tst_UninstallEntry PROPERTIES LABELS "unit" TIMEOUT 300)
asb_add_core_test(tst_UninstallPreview)   # WS-16 INV-125: what an uninstall says goes down

if(WIN32)
    add_subdirectory(win/integration)
    add_subdirectory(win/dllprobe)   # INV-67 (WS-7)
    add_subdirectory(win/release)    # INV-88 (WS-13): the release self-test's plant
endif()

# ---------------------------------------------------------------------------------------------
# WS-4: session engine, serial adapter, side-channel worker, exit primitives.
# ---------------------------------------------------------------------------------------------
add_library(asb_test_support_engine STATIC
    support/EngineTestSupport.h
    support/FakeSerialBackend.h
    support/FakeSerialDevice.cpp support/FakeSerialDevice.h)
target_include_directories(asb_test_support_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asb_test_support_engine PUBLIC asb_engine asb_test_support Qt6::Test PRIVATE asb_warnings)

function(asb_add_engine_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE asb_test_support_engine asb_engine asb_core Qt6::Test Qt6::Core asb_warnings)
    add_test(NAME ${name} COMMAND ${name} -o -,txt)
    set_tests_properties(${name} PROPERTIES LABELS "unit")
endfunction()

asb_add_engine_test(tst_PortBridge)        # INV-1, 2, 3, 4, 5, 6, 9, 10, 27, 28, 55, 56, 57, 59, 80
asb_add_engine_test(tst_QtSerialDevice)    # INV-58
asb_add_engine_test(tst_SideChannel)       # INV-68, INV-83
asb_add_engine_test(tst_LogSink)           # INV-68
asb_add_engine_test(tst_DeadlineWatchdog)  # exit primitives: the deadline watchdog (D56, D59, N9-1)
asb_add_engine_test(tst_Exit)              # exit primitives: faultDie, Exit::now (D59)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Real QSerialPort on a pseudo-terminal (INV-59 hangup). Never skipped on Linux.
    asb_add_engine_test(tst_SerialPty support/PtyPair.cpp support/PtyPair.h)
    set_tests_properties(tst_SerialPty PROPERTIES LABELS "unit;pty")
endif()

# INV-1: the GUI executable never contains the engine (link_gui_excludes_engine), proven against a
# control executable that does (engine_control_bad must be rejected by every layer).
add_executable(engine_control_bad build/engine_control_bad.cpp)
target_link_libraries(engine_control_bad PRIVATE asb_engine asb_warnings)
set(_asb_engine_graph_targets engine_control_bad)
if(ASB_BUILD_GUI)
    list(APPEND _asb_engine_graph_targets ${ASB_GUI_TARGET})
endif()
asb_write_link_graphs(${_asb_engine_graph_targets})

function(_asb_engine_link_args target out)
    set(args "-DGRAPH=${CMAKE_BINARY_DIR}/nogui/${target}.graph")
    if(NOT WIN32 AND ASB_NM)
        list(APPEND args "-DEXE=$<TARGET_FILE:${target}>" "-DNM=${ASB_NM}")
    endif()
    set(${out} "${args}" PARENT_SCOPE)
endfunction()

_asb_engine_link_args(engine_control_bad _ctl)
add_test(NAME link_engine_control_rejected
         COMMAND ${CMAKE_COMMAND} ${_ctl} -DMODE=check -P "${CMAKE_CURRENT_SOURCE_DIR}/build/EngineLinkCheck.cmake")
set_tests_properties(link_engine_control_rejected PROPERTIES
    PASS_REGULAR_EXPRESSION "EngineLinkCheck: VIOLATION: engine_control_bad" LABELS "build")
add_test(NAME link_engine_control_every_layer
         COMMAND ${CMAKE_COMMAND} ${_ctl} -DMODE=control -P "${CMAKE_CURRENT_SOURCE_DIR}/build/EngineLinkCheck.cmake")
set_tests_properties(link_engine_control_every_layer PROPERTIES LABELS "build")
if(ASB_BUILD_GUI)
    _asb_engine_link_args(${ASB_GUI_TARGET} _gui)
    add_test(NAME link_gui_excludes_engine
             COMMAND ${CMAKE_COMMAND} ${_gui} -DMODE=check -DTEST_NAME=link_gui_excludes_engine
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/build/EngineLinkCheck.cmake")
    set_tests_properties(link_gui_excludes_engine PROPERTIES LABELS "build")
endif()

# ---------------------------------------------------------------------------------------------
# WS-8: the Linux adapters (data folder, FileIo, locks, systemd over D-Bus, identity, guard, ports).
# Real file systems in a temp directory; the systemd manager behind a fake bus. The systemd-as-PID-1
# suite is tests/systemd/driver.py (CI: the containers leg).
# ---------------------------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(asb_ws8_support STATIC
        support/RecordingSys.cpp support/RecordingSys.h
        support/FakeSystemdBus.cpp support/FakeSystemdBus.h)
    target_link_libraries(asb_ws8_support PUBLIC asb_ws5_fakes asb_platform_posix asb_test_support Qt6::Test PRIVATE asb_warnings)

    function(asb_add_ws8_test name)
        asb_add_core_test(${name})
        target_link_libraries(${name} PRIVATE asb_ws8_support asb_platform_posix)
        set_tests_properties(${name} PROPERTIES TIMEOUT 300)
    endfunction()

    asb_add_ws8_test(tst_PosixFileIo)       # INV-15 contract (native), INV-75/77/96 POSIX rows
    asb_add_ws8_test(tst_PosixDataFolder)   # INV-65, INV-63/74 POSIX rows, INV-22/44, INV-116 data folder
    asb_add_ws8_test(tst_EngineLockPosix)   # INV-110 lock rows, INV-66/97 twins, INV-116 lock directory
    asb_add_ws8_test(tst_ServiceIdentity)   # INV-100
    asb_add_ws8_test(tst_SystemdUnit)
    target_compile_definitions(tst_SystemdUnit PRIVATE ASB_REPO="${ASB_REPO}")
    asb_add_ws8_test(tst_SystemdService)    # INV-48/79 seam halves, installation states
    asb_add_ws8_test(tst_SystemdDbus)       # INV-115
    asb_add_ws8_test(tst_SerialPortList)    # INV-32 (listing; strace in the systemd suite)

    # INV-1 (WS-4 deferred, the Linux --service half): only the service hosts name the device factory. WS-9 adds
    # its `--run --config` file to ALLOWED; WS-7 added its Windows service host.
    add_test(NAME factory_registration_sites
             COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/src
                     "-DALLOWED=app/ServiceHost_linux.cpp$<SEMICOLON>platform/win/ServiceMain.cpp$<SEMICOLON>app/ScratchEngine.cpp"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/build/FactorySites.cmake")
    set_tests_properties(factory_registration_sites PROPERTIES PASS_REGULAR_EXPRESSION "FactorySites: OK" LABELS "unit;build")
    add_test(NAME factory_registration_sites_control
             COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/src "-DALLOWED="
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/build/FactorySites.cmake")
    set_tests_properties(factory_registration_sites_control PROPERTIES
                         PASS_REGULAR_EXPRESSION "FactorySites: VIOLATION: makeQtSerialDevice referenced by app/ScratchEngine.cpp;app/ServiceHost_linux.cpp;platform/win/ServiceMain.cpp"
                         LABELS "unit;build")

    # v1.1 WS-15: the window's routes without the window, for the systemd suite's bootstrap_* cases (QtCore only, so the
    # containers leg's headless build has it, and it runs in the Fedora polkit container too).
    add_executable(asb_stage_client systemd/stage_client.cpp)
    target_link_libraries(asb_stage_client PRIVATE asb_cli Qt6::Core asb_warnings)

    # The process guard (INV-110): the probe installs the product's guard, then attempts one thing.
    add_executable(asb_seccomp_probe systemd/seccomp_probe.cpp)
    target_link_libraries(asb_seccomp_probe PRIVATE asb_platform_posix Qt6::Core Qt6::DBus asb_warnings)
    add_executable(tst_ProcessGuard tst_ProcessGuard.cpp)
    target_link_libraries(tst_ProcessGuard PRIVATE Qt6::Test Qt6::Core asb_warnings)
    add_dependencies(tst_ProcessGuard asb_seccomp_probe)
    add_test(NAME tst_ProcessGuard COMMAND tst_ProcessGuard -o -,txt)
    set_tests_properties(tst_ProcessGuard PROPERTIES LABELS "unit;process" TIMEOUT 300
                         ENVIRONMENT "ASB_SECCOMP_PROBE=$<TARGET_FILE:asb_seccomp_probe>")
endif()

# ---------------------------------------------------------------------------------------------
# WS-9: the command line, the commands, the config editor, the scratch rules, the runner behind the engine.
# ---------------------------------------------------------------------------------------------
asb_add_test(tst_ServiceHost)   # INV-90 serviceRequiresServiceManager (both OSes: the real executable)

add_executable(tst_CommandLine tst_CommandLine.cpp)
target_link_libraries(tst_CommandLine PRIVATE asb_app_common asb_cli asb_scratch_engine asb_test_support_engine
                      Qt6::Test Qt6::Network asb_warnings)
add_test(NAME tst_CommandLine COMMAND tst_CommandLine -o -,txt)
set_tests_properties(tst_CommandLine PROPERTIES LABELS "unit" TIMEOUT 300)   # INV-26, INV-90, INV-101

asb_add_core_test(tst_ConfigEditor)
target_link_libraries(tst_ConfigEditor PRIVATE asb_cli)

foreach(_t tst_Commands tst_Cli)   # the commands over the WS-5 fakes; INV-118 tst_Cli::exitsAfterFinal
    asb_add_ws5_test(${_t})
    target_link_libraries(${_t} PRIVATE asb_cli)
endforeach()

asb_add_ws5_test(tst_RunnerIntegration)   # WS-4 deferred INV-1, 2, 3, 9, 59: the engine behind the runner
target_link_libraries(tst_RunnerIntegration PRIVATE asb_test_support_engine Qt6::Network)

# WS-12: the TUI (tests/Tui.cmake).
include("${CMAKE_CURRENT_SOURCE_DIR}/Tui.cmake")

# INV-112: process_creation_audit. No object of a product executable references a process-creation function or
# QProcess (the guard probe of a test build and ProcessGuard's seccomp call excepted, by object and symbol); the
# control executable that calls QProcess::start must be found. And no raw syscall( / __NR_ in the sources.
add_executable(process_audit_control_bad build/process_control_bad.cpp)
target_link_libraries(process_audit_control_bad PRIVATE Qt6::Core asb_warnings)
asb_enable_link_map(process_audit_control_bad)
set(_asb_process_libs asb_core asb_engine asb_cli asb_tui asb_app_common asb_scratch_engine asb_process_policy
    asb_platform_posix asb_platform_win asb_win_adapters asb_win_service asb_win_service_host asb_gui aethersdr_canon
    aethersdr_sqlite aethersdr_shims)
function(_asb_process_audit_args target out)
    if(MSVC)
        set(_objs "$<TARGET_OBJECTS:${target}>")
        foreach(_l IN LISTS _asb_process_libs)
            if(TARGET ${_l})
                list(APPEND _objs "$<TARGET_OBJECTS:${_l}>")
            endif()
        endforeach()
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/process-audit/$<CONFIG>/${target}.objects"
             CONTENT "$<JOIN:${_objs},\n>\n")
        set(args "-DEXE=$<TARGET_FILE:${target}>" "-DDUMPBIN=${ASB_DUMPBIN}"
                 "-DOBJECTS=${CMAKE_BINARY_DIR}/process-audit/$<CONFIG>/${target}.objects"
                 # WS-16 (D64): SelfElevate.cpp is the one object that may call ShellExecuteExW. SelfElevateTestChild.cpp
                 # (test builds only, v1.1 station fix) starts the relaunch as a real child for tst_GuiNative.
                 "-DALLOWED=GuardProbe.cpp.obj|CreateProcessW,GuardProbe.obj|CreateProcessW,SelfElevate.cpp.obj|ShellExecuteExW,SelfElevate.obj|ShellExecuteExW,SelfElevateTestChild.cpp.obj|CreateProcessW,SelfElevateTestChild.obj|CreateProcessW")
    else()
        set(_allow "ProcessGuard.cpp.o|syscall,GuardProbe.cpp.o|fork|CreateProcessW")
        if(ASB_BUILD_GUI AND target STREQUAL "${ASB_GUI_TARGET}")
            # The Linux GUI's pkexec launch (INV-112, WS-10): QProcess in PkexecClient.cpp only.
            string(APPEND _allow ",PkexecClient.cpp.o|QProcess::.*")
        endif()
        set(args "-DEXE=$<TARGET_FILE:${target}>" "-DMAP=$<TARGET_FILE:${target}>.map" "-DNM=${ASB_NM}"
                 "-DBINARY_DIR=${CMAKE_BINARY_DIR}" "-DALLOWED=${_allow}")
    endif()
    set(${out} "${args}" PARENT_SCOPE)
endfunction()
set(_asb_audited aether-serial-bridge)
if(ASB_BUILD_GUI)
    list(APPEND _asb_audited ${ASB_GUI_TARGET})
endif()
foreach(_exe IN LISTS _asb_audited)
    _asb_process_audit_args(${_exe} _args)
    add_test(NAME process_creation_audit.${_exe}
             COMMAND ${CMAKE_COMMAND} ${_args} -DMODE=check -DTEST_NAME=process_creation_audit
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/build/ProcessCreationAudit.cmake")
    set_tests_properties(process_creation_audit.${_exe} PROPERTIES LABELS "build" PASS_REGULAR_EXPRESSION "ProcessCreationAudit: OK")
endforeach()
_asb_process_audit_args(process_audit_control_bad _ctl)
add_test(NAME process_creation_audit_control
         COMMAND ${CMAKE_COMMAND} ${_ctl} -DMODE=control -P "${CMAKE_CURRENT_SOURCE_DIR}/build/ProcessCreationAudit.cmake")
set_tests_properties(process_creation_audit_control PROPERTIES LABELS "build"
                     PASS_REGULAR_EXPRESSION "ProcessCreationAudit: control process_audit_control_bad[.a-z]* detected")
add_test(NAME process_creation_audit_control_fails_check
         COMMAND ${CMAKE_COMMAND} ${_ctl} -DMODE=check -P "${CMAKE_CURRENT_SOURCE_DIR}/build/ProcessCreationAudit.cmake")
set_tests_properties(process_creation_audit_control_fails_check PROPERTIES LABELS "build"
                     PASS_REGULAR_EXPRESSION "ProcessCreationAudit: VIOLATION: process_audit_control_bad[.a-z]*: .*QProcess::start")
if(MSVC)
    # WS-16 (D64): with SelfElevate.cpp's exception in force, an object elsewhere that calls ShellExecuteExW must still
    # fail the audit (shellexecute_control).
    add_executable(process_audit_shellexecute_bad build/shellexecute_control_bad.cpp)
    target_link_libraries(process_audit_shellexecute_bad PRIVATE shell32)
    _asb_process_audit_args(process_audit_shellexecute_bad _shx)
    add_test(NAME shellexecute_control
             COMMAND ${CMAKE_COMMAND} ${_shx} -DMODE=check -P "${CMAKE_CURRENT_SOURCE_DIR}/build/ProcessCreationAudit.cmake")
    set_tests_properties(shellexecute_control PROPERTIES LABELS "build"
                         PASS_REGULAR_EXPRESSION "ProcessCreationAudit: VIOLATION: .*ShellExecuteExW")
endif()
add_test(NAME raw_syscall_audit
         COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/src "-DALLOWED=platform/posix/ProcessGuard.cpp"
                 -DTEST_NAME=process_creation_audit -P "${CMAKE_CURRENT_SOURCE_DIR}/build/SyscallGrep.cmake")
set_tests_properties(raw_syscall_audit PROPERTIES LABELS "build" PASS_REGULAR_EXPRESSION "SyscallGrep: OK")
add_test(NAME raw_syscall_audit_control
         COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/src "-DALLOWED=" -P "${CMAKE_CURRENT_SOURCE_DIR}/build/SyscallGrep.cmake")
set_tests_properties(raw_syscall_audit_control PROPERTIES LABELS "build"
                     PASS_REGULAR_EXPRESSION "SyscallGrep: VIOLATION: platform/posix/ProcessGuard.cpp")

# guard_installed_before_first_object (INV-112): at the CLI's mode entry, the test build's probe tries to create a
# process right after the guard's call site and before the first Applier/Platform object; it must be denied. The
# probe ends the process (ASB_TEST_GUARD_PROBE=exit), so nothing is changed.
add_test(NAME guard_installed_before_first_object COMMAND aether-serial-bridge --stop)
set_tests_properties(guard_installed_before_first_object PROPERTIES LABELS "process" ENVIRONMENT "ASB_TEST_GUARD_PROBE=exit"
                     PASS_REGULAR_EXPRESSION "ASB-GUARD-PROBE denied" FAIL_REGULAR_EXPRESSION "ASB-GUARD-PROBE created")
# The diagnostic run's mode entry: the probe after its guard call site, before the QCoreApplication and
# the engine. The scratch path need not exist: the probe ends the process first.
add_test(NAME guard_installed_before_first_object_run COMMAND aether-serial-bridge --run --config guard-probe-scratch/c.json)
set_tests_properties(guard_installed_before_first_object_run PROPERTIES LABELS "process"
                     ENVIRONMENT "ASB_TEST_GUARD_PROBE=exit;ASB_TEST_GUARD_PROBE_NAME=guard_installed_before_first_object_run"
                     PASS_REGULAR_EXPRESSION "ASB-GUARD-PROBE denied" FAIL_REGULAR_EXPRESSION "ASB-GUARD-PROBE created")

# ---------------------------------------------------------------------------------------------
# WS-10: the window (offscreen) over the WS-5 fakes, and the GUI executable as a process.
# ---------------------------------------------------------------------------------------------
if(ASB_BUILD_GUI)
    asb_add_gui_test(tst_BridgeWindow)   # INV-13, 23, 33, 35, 69, 94, 107; INV-118/102/29 window rows
    target_link_libraries(tst_BridgeWindow PRIVATE asb_gui asb_ws5_fakes)
    # v1.1 WS-19: the docs images' demo fixture (tst_BridgeWindow::docsShots, run by tools/docs_shots.py; INV-135).
    target_compile_definitions(tst_BridgeWindow PRIVATE ASB_DOCS_DEMO_DIR="${CMAKE_SOURCE_DIR}/docs/images/demo")
    set_tests_properties(tst_BridgeWindow PROPERTIES TIMEOUT 300)
    asb_add_test(tst_GuiProcess)         # --self-test-init; INV-32's GUI row (strace, Linux)
    set_tests_properties(tst_GuiProcess PROPERTIES LABELS "process;gui" TIMEOUT 300)
    target_link_libraries(tst_GuiProcess PRIVATE asb_gui)   # PkexecClient (Linux)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # v1.1 WS-15: the Linux window's routes (staged and installed helper) over a fake process (INV-122, INV-123).
        asb_add_core_test(tst_HelperActions)
        target_link_libraries(tst_HelperActions PRIVATE asb_gui)
        # v1.1 WS-17 (INV-130): the window's guard by elevation and its actions, decided before the window; --intent.
        asb_add_core_test(tst_GuiMain)
        target_link_libraries(tst_GuiMain PRIVATE asb_gui asb_app_common asb_ws5_fakes)
    endif()
    if(WIN32)
        # INV-112 (WS-9 table: "GUI executable: --gui, required on Windows"): the window's mode entry installs the job
        # guard before any Platform or Applier object; the probe right after it must be denied. It ends the process.
        add_test(NAME guard_installed_before_first_object_gui COMMAND ${ASB_GUI_TARGET} --gui)
        set_tests_properties(guard_installed_before_first_object_gui PROPERTIES LABELS "process"
                             ENVIRONMENT "ASB_TEST_GUARD_PROBE=exit;ASB_TEST_GUARD_PROBE_NAME=guard_installed_before_first_object_gui;QT_QPA_PLATFORM=offscreen"
                             PASS_REGULAR_EXPRESSION "ASB-GUARD-PROBE denied" FAIL_REGULAR_EXPRESSION "ASB-GUARD-PROBE created")
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# WS-9b: the Linux pkexec helper (master §5.7): its bounded input (INV-89), its wire format and the polkit policy,
# the helper itself over the WS-5 fakes (INV-50 unit half, INV-118), the GUI's client (INV-107) and the client →
# helper → client round trip. tst_HelperRoundTrip runs in process here; the systemd suite's helper_round_trip runs the
# same binary against the installed program (directly as root, and through pkexec under a test polkit rule).
# ---------------------------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(_t tst_HelperInput tst_HelperProtocol tst_HelperClient)
        asb_add_core_test(${_t})
        target_link_libraries(${_t} PRIVATE asb_cli)
    endforeach()
    target_compile_definitions(tst_HelperProtocol PRIVATE ASB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # v1.1 WS-15: the staging script embedded byte for byte, shellcheck-clean, its stderr lines = the catalogue.
    asb_add_core_test(tst_StageScript)
    target_link_libraries(tst_StageScript PRIVATE asb_cli)
    target_compile_definitions(tst_StageScript PRIVATE ASB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    foreach(_t tst_Helper tst_HelperRoundTrip)
        asb_add_ws5_test(${_t})
        target_link_libraries(${_t} PRIVATE asb_cli)
    endforeach()

    # The helper's mode entry (INV-112: `--helper` is a guarded entry): the probe right after its guard call site,
    # before the root check, the request and the first Platform object. Unprivileged and with no input: the probe
    # ends the process first.
    add_test(NAME guard_installed_before_first_object_helper COMMAND aether-serial-bridge --helper)
    set_tests_properties(guard_installed_before_first_object_helper PROPERTIES LABELS "process"
                         ENVIRONMENT "ASB_TEST_GUARD_PROBE=exit;ASB_TEST_GUARD_PROBE_NAME=guard_installed_before_first_object_helper"
                         PASS_REGULAR_EXPRESSION "ASB-GUARD-PROBE denied" FAIL_REGULAR_EXPRESSION "ASB-GUARD-PROBE created")
endif()
