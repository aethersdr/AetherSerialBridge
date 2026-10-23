# INV-67 (WS-7): the dependent-load policy, proven with a positive control. The probe and the plant live in their
# own output directories: a planted VERSION.dll must never sit beside the real executables (Qt imports it).
set(_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/bin")

add_library(asb_dllprobe_plant SHARED plant.c plant.def)
set_target_properties(asb_dllprobe_plant PROPERTIES OUTPUT_NAME version
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plant"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plant"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plant-lib")

foreach(_kind unmitigated mitigated)
    add_executable(dllprobe_${_kind} probe.c)
    target_link_libraries(dllprobe_${_kind} PRIVATE version)
    set_target_properties(dllprobe_${_kind} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_probe_dir}")
endforeach()
asb_apply_dll_policy(dllprobe_mitigated ALWAYS)   # the same function the product executables use

add_test(NAME dllprobe_unmitigated
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:dllprobe_unmitigated> -DPLANT=$<TARGET_FILE:asb_dllprobe_plant>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/run-unmitigated -DEXPECT=plant -P ${CMAKE_CURRENT_SOURCE_DIR}/RunProbe.cmake)
add_test(NAME dllprobe_mitigated
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:dllprobe_mitigated> -DPLANT=$<TARGET_FILE:asb_dllprobe_plant>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/run-mitigated -DEXPECT=system -P ${CMAKE_CURRENT_SOURCE_DIR}/RunProbe.cmake)
set_tests_properties(dllprobe_unmitigated dllprobe_mitigated PROPERTIES LABELS "win-integration;dll-policy")
add_dependencies(dllprobe_mitigated asb_dllprobe_plant)
add_dependencies(dllprobe_unmitigated asb_dllprobe_plant)

# The audit's own controls: the mitigated probe passes its load-config check; an executable importing a DLL no
# allowlist names (DBGHELP) is rejected against the headless allowlist.
add_executable(import_audit_control_bad control_bad.c)
target_link_libraries(import_audit_control_bad PRIVATE dbghelp)
set_target_properties(import_audit_control_bad PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_probe_dir}")
asb_add_import_audit(import_audit_control_bad ALLOWLIST_OF aether-serial-bridge)
set_tests_properties(ImportAudit.import_audit_control_bad PROPERTIES
    PASS_REGULAR_EXPRESSION "imports '[dD][bB][gG][hH][eE][lL][pP]\\.[dD][lL][lL]', which")
asb_add_import_audit(dllprobe_mitigated ALLOWLIST_OF aether-serial-bridge)
set_tests_properties(ImportAudit.dllprobe_mitigated PROPERTIES
    PASS_REGULAR_EXPRESSION "carries the dependent load flag 0x800")
