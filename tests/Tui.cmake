# WS-12: the TUI (FTXUI). Included by tests/CMakeLists.txt after the WS-5 fakes and the WS-9 section.

# The controller and the screens in process, over the WS-5 fakes, on every OS (render goldens in data/tui).
asb_add_ws5_test(tst_Tui)
target_link_libraries(tst_Tui PRIVATE asb_tui asb_cli)
# v1.1 WS-19: the docs images' demo fixture (tst_Tui::docsShots, run by tools/docs_shots.py; INV-135).
target_compile_definitions(tst_Tui PRIVATE ASB_DOCS_DEMO_DIR="${CMAKE_SOURCE_DIR}/docs/images/demo")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The fake service host behind the real TUI entry (tui::run), for the pty process tests (master 8.4).
    add_executable(asb_tui_testhost support/TuiTestHost.cpp support/TuiScenario.h)
    target_link_libraries(asb_tui_testhost PRIVATE asb_tui asb_cli asb_ws5_fakes asb_test_support asb_platform_posix
                          asb_warnings)

    # The TUI as a process on a pseudo-terminal: INV-71, INV-72 and the integration rows (WS-12 doc).
    add_executable(tst_TuiProcess tst_TuiProcess.cpp)
    target_link_libraries(tst_TuiProcess PRIVATE asb_tui asb_core Qt6::Test Qt6::Core util asb_warnings)
    add_dependencies(tst_TuiProcess aether-serial-bridge asb_tui_testhost)
    set(_asb_tui_env "ASB_HEADLESS_EXE=$<TARGET_FILE:aether-serial-bridge>" "ASB_TUI_TESTHOST=$<TARGET_FILE:asb_tui_testhost>")
    if(ASB_BUILD_GUI)
        add_dependencies(tst_TuiProcess ${ASB_GUI_TARGET})
        list(APPEND _asb_tui_env "ASB_GUI_EXE=$<TARGET_FILE:${ASB_GUI_TARGET}>")
    endif()
    add_test(NAME tst_TuiProcess COMMAND tst_TuiProcess -o -,txt)
    set_tests_properties(tst_TuiProcess PROPERTIES LABELS "unit;process;pty" TIMEOUT 300 ENVIRONMENT "${_asb_tui_env}")
endif()

# INV-112 at the TUI's mode entry: the guard comes before the terminal check, the QCoreApplication and the Platform,
# so the test build's probe (right after the guard's call site) runs even without a terminal, and must be denied.
add_test(NAME guard_installed_before_first_object_tui COMMAND aether-serial-bridge --tui)
set_tests_properties(guard_installed_before_first_object_tui PROPERTIES LABELS "process"
                     ENVIRONMENT "ASB_TEST_GUARD_PROBE=exit;ASB_TEST_GUARD_PROBE_NAME=guard_installed_before_first_object_tui"
                     PASS_REGULAR_EXPRESSION "ASB-GUARD-PROBE denied" FAIL_REGULAR_EXPRESSION "ASB-GUARD-PROBE created")

# INV-20: the TUI reads configuration only through TrustedConfig (no object in asb_tui references a ConfigFiles read).
if(NOT MSVC)
    add_test(NAME inv20_config_read_symbols.asb_tui
             COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DARCHIVE=$<TARGET_FILE:asb_tui> -DALLOWED=no-object-allowed
                     -P "${CMAKE_SOURCE_DIR}/cmake/ConfigReadSymbols.cmake")
    set_tests_properties(inv20_config_read_symbols.asb_tui PROPERTIES PASS_REGULAR_EXPRESSION "ConfigReadSymbols: OK"
                         LABELS "unit;build")
endif()
