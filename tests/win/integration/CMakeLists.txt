# WS-6 native rows (Windows only), label win-integration. They run on the windows-latest CI leg, which is an
# administrator: real NTFS ACLs, owners, links, locks, local accounts (NetUserAdd + an in-process logon) and the
# job object. The harness enables SeRestorePrivilege where it plants owners; the product never does.

# The names the native tests give to what they create on the machine come from ci/win-test-names.psd1, the one
# source ci.yml and the station script agree with too: WinTestNames.h is generated from its test-side
# entries (tools/check_win_test_names.py fails on drift, and on such a name written out anywhere else).
set(_asb_names_src "${CMAKE_SOURCE_DIR}/ci/win-test-names.psd1")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_asb_names_src}")
file(STRINGS "${_asb_names_src}" _asb_names_lines)
set(_asb_names_body "")
foreach(_key AccountPrefix ScratchPrefix ReportLog KeywordMatrixCiGroup KeywordMatrixTestGroup)
    set(_value "")
    foreach(_line IN LISTS _asb_names_lines)
        if(_line MATCHES "^[ \t]*${_key}[ \t]*=[ \t]*'([^']*)'")
            set(_value "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_value STREQUAL "")
        message(FATAL_ERROR "ci/win-test-names.psd1 has no single-quoted value for ${_key}")
    endif()
    string(APPEND _asb_names_body "inline constexpr char k${_key}[] = \"${_value}\";\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WinTestNames.h" CONTENT
"#pragma once
// Generated from ci/win-test-names.psd1 by tests/win/integration/CMakeLists.txt. Do not edit.
namespace asb::test::names {
${_asb_names_body}}  // namespace asb::test::names
" @ONLY)

function(asb_add_win_integration_test name)
    add_executable(${name} ${name}.cpp WinNativeSupport.h)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
                               "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(${name} PRIVATE asb_platform_win asb_win_adapters asb_ws5_fakes asb_test_support asb_core
                          Qt6::Test Qt6::Core advapi32 netapi32 asb_warnings)
    # CTest on the Windows runner captures no QtTest text: also write it to a file the CI prints on failure.
    add_test(NAME ${name} COMMAND ${name} -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.log,txt")
    set_tests_properties(${name} PROPERTIES LABELS "win-integration" TIMEOUT 600 RUN_SERIAL TRUE)
endfunction()

add_executable(asb_win_lock_helper asb_win_lock_helper.cpp)
target_link_libraries(asb_win_lock_helper PRIVATE asb_platform_win asb_win_adapters asb_core Qt6::Core asb_warnings)

asb_add_win_integration_test(tst_WinDataFolderNative)   # INV-16, 17, 19, 40, 62, 75; WS-5 rows INV-63, 74, 116
asb_add_win_integration_test(tst_WinFileIoNative)       # INV-15 (FileIoContract, reader_does_not_block_rename), 75, 77, 96
asb_add_win_integration_test(tst_WinLocksNative)        # INV-66, 97, 104; the engine-lock replace ordering
add_dependencies(tst_WinLocksNative asb_win_lock_helper)
set_tests_properties(tst_WinLocksNative PROPERTIES ENVIRONMENT "ASB_WIN_LOCK_HELPER=$<TARGET_FILE:asb_win_lock_helper>")
asb_add_win_integration_test(tst_WinApplierNative)      # WS-5 rows INV-116 (Windows lock pair, old engine)
add_dependencies(tst_WinApplierNative asb_win_lock_helper)
set_tests_properties(tst_WinApplierNative PROPERTIES ENVIRONMENT "ASB_WIN_LOCK_HELPER=$<TARGET_FILE:asb_win_lock_helper>")
asb_add_win_integration_test(tst_ProcessGuardNative)    # INV-104 applier_process_cannot_create_children

# ---------------------------------------------------------------------------------------------
# WS-7: the service host on the real SCM, the CIM firewall on the real provider. The service is installed into
# %ProgramFiles%\AetherSerialBridge (the runner is a throwaway VM); every row leaves no service behind.
# ---------------------------------------------------------------------------------------------
add_library(asb_win_svc_support STATIC ServiceNativeSupport.cpp ServiceNativeSupport.h)
target_include_directories(asb_win_svc_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
                           "${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(asb_win_svc_support PUBLIC asb_platform_win asb_win_service asb_core Qt6::Core Qt6::Network
                      advapi32 PRIVATE asb_warnings)

function(asb_add_win_service_test name)
    asb_add_win_integration_test(${name})
    target_link_libraries(${name} PRIVATE asb_win_svc_support Qt6::Network)
    add_dependencies(${name} aether-serial-bridge)
    set_tests_properties(${name} PROPERTIES TIMEOUT 1800
        ENVIRONMENT "ASB_HEADLESS_EXE=$<TARGET_FILE:aether-serial-bridge>")
endfunction()

asb_add_win_integration_test(tst_CimNative)    # INV-11, 114, 117 against the real WMI firewall provider; keyword pin
asb_add_win_service_test(tst_ServiceNative)
asb_add_win_service_test(tst_CliNative)   # WS-9: the console exe's operations (OperationHost worker, guard, CIM) end to end
asb_add_win_service_test(tst_ServiceScheduleNative)   # WS-5/WS-6 deferred native gates (INV-63, 92, 104, 113, 114, 119)   # INV-7, 22, 60, 81, 82, 93, 103, 113, 117 on the real SCM
add_dependencies(tst_ServiceScheduleNative asb_win_lock_helper)   # the old engine of deadline_state_unknown_no_r3
set_property(TEST tst_ServiceScheduleNative APPEND PROPERTY ENVIRONMENT "ASB_WIN_LOCK_HELPER=$<TARGET_FILE:asb_win_lock_helper>")

# The qualification harness the station script drives (WS-7; test builds only).
# User report 2026-10-16: the REAL window executable (offscreen) runs Stop, Save & apply, Start, then the service must
# start through SCM while the window stays open.
if(ASB_BUILD_GUI)
    asb_add_win_service_test(tst_GuiNative)
    # v1.1 WS-17: the handle probe (INV-128); userenv for the standard user's profile (standardUserWindow).
    target_sources(tst_GuiNative PRIVATE HandleProbe.cpp HandleProbe.h)
    target_link_libraries(tst_GuiNative PRIVATE userenv)
    add_dependencies(tst_GuiNative ${ASB_GUI_TARGET})
    set_tests_properties(tst_GuiNative PROPERTIES TIMEOUT 3600)   # the v1.1 lifecycle rows start the window ~12 times
    set_property(TEST tst_GuiNative APPEND PROPERTY ENVIRONMENT "ASB_GUI_EXE=$<TARGET_FILE:${ASB_GUI_TARGET}>")
endif()

add_executable(asb_win_qual ../qual/asb_win_qual.cpp)
target_link_libraries(asb_win_qual PRIVATE asb_win_svc_support Qt6::SerialPort asb_warnings)
