# asb_cli (WS-9): the command line, the commands, the config editor, the scratch-config rules, elevation and this
# OS's Host. No engine: both executables link it, and the GUI never contains the engine (INV-1); --run and
# --service are the headless executable's (src/app).
add_library(asb_cli STATIC
    CommandLine.cpp CommandLine.h
    Commands.cpp Commands.h
    ConfigEditor.cpp ConfigEditor.h
    Elevation.cpp Elevation.h
    ScratchRun.cpp ScratchRun.h
    SystemHost.h)
target_include_directories(asb_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(WIN32)
    target_sources(asb_cli PRIVATE WindowsHost.cpp WindowsHost.h)
    # user32: the --uninstall-from-apps message boxes (WS-16).
    target_link_libraries(asb_cli PUBLIC asb_core asb_platform_win advapi32 user32)
else()
    # The Linux host, and the pkexec helper (WS-9b, master §5.7): the helper itself, its bounded input, its wire
    # format and the client the Linux GUI drives it with (the GUI's PkexecClient is the process boundary, WS-10).
    target_sources(asb_cli PRIVATE LinuxHost.cpp LinuxHost.h
        Helper.cpp Helper.h
        HelperInput.cpp HelperInput.h
        HelperProtocol.cpp HelperProtocol.h
        HelperClient.cpp HelperClient.h
        StagedHelper.cpp StagedHelper.h)
    target_link_libraries(asb_cli PUBLIC asb_core asb_platform_posix)
    # The polkit policy: one source file, embedded byte for byte (SystemdService writes it at install).
    set(_asb_policy "${CMAKE_SOURCE_DIR}/packaging/linux/org.aethersdr.aetherserialbridge.policy")
    # As a byte array (moc's preprocessor follows this header into every test that includes it; a raw string of XML
    # would confuse its tokenizer).
    file(READ "${_asb_policy}" _asb_policy_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," ASB_POLKIT_POLICY "${_asb_policy_hex}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_asb_policy}")
    configure_file(PolkitPolicy.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/cli/PolkitPolicy.h" @ONLY)
    # The staging script the window runs through pkexec (v1.1 D60): one source file, embedded byte for byte the same way.
    set(_asb_stage "${CMAKE_SOURCE_DIR}/packaging/linux/stage.sh")
    file(READ "${_asb_stage}" _asb_stage_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," ASB_STAGE_SCRIPT "${_asb_stage_hex}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_asb_stage}")
    configure_file(StageScript.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/cli/StageScript.h" @ONLY)
    target_include_directories(asb_cli PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()
target_link_libraries(asb_cli PRIVATE asb_warnings)
if(ASB_BUILD_TESTS)
    # WS-16: ASB_TEST_MESSAGEBOX (WindowsHost) exists only when the tests are built (release_audit.py seams).
    target_compile_definitions(asb_cli PRIVATE ASB_TEST_SEAMS)
endif()
