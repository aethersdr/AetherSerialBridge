# The two executables (D1) and the code they share: identity (D20) and mode dispatch.

# Test builds carry seams the executables must never ship with (`--identity-probe`, ...). They are
# on exactly when the tests are built, so a release configuration (ASB_BUILD_TESTS=OFF) has none.
if(ASB_BUILD_TESTS)
    set(ASB_TEST_SEAMS ON)
else()
    set(ASB_TEST_SEAMS OFF)
endif()

add_library(asb_app_common STATIC Identity.cpp Identity.h Dispatch.cpp Dispatch.h
    WindowProgram.cpp WindowProgram.h)   # v1.1 WS-14: the window's program locator
target_include_directories(asb_app_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(asb_app_common PUBLIC asb_core asb_cli asb_process_policy Qt6::Core PRIVATE asb_warnings)
if(ASB_TEST_SEAMS)
    target_compile_definitions(asb_app_common PUBLIC ASB_TEST_SEAMS)
endif()

# The application icon (resources/icon/asb.rc: IDI_ICON1, asb.ico) for both Windows executables: Explorer, the
# taskbar, Alt-Tab and the uninstall entry (DisplayIcon "<exe>,0") show it. A resource adds no imports.
set(ASB_ICON_RC "${CMAKE_SOURCE_DIR}/resources/icon/asb.rc")

# The diagnostic run's engine (WS-9, `--run --config <scratch>`): headless only, like everything that contains the
# engine (INV-1). A library so tst_CommandLine can bind it and read the OS's socket table (runForcesLoopback).
add_library(asb_scratch_engine STATIC ScratchEngine.cpp ScratchEngine.h)
target_include_directories(asb_scratch_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(asb_scratch_engine PUBLIC asb_cli asb_engine asb_core PRIVATE asb_warnings)

# --- aether-serial-bridge: service host, CLI, TUI. INV-37: contains no QtGui code. --------------
qt_add_executable(aether-serial-bridge main_headless.cpp ServiceHost.h HeadlessHost.cpp HeadlessHost.h)
target_link_libraries(aether-serial-bridge PRIVATE asb_app_common asb_engine asb_scratch_engine asb_tui asb_cli asb_warnings)
if(ASB_TEST_SEAMS)
    # guard_installed_before_first_object (INV-112): the only process creation in a product executable, test builds
    # only; the process_creation_audit allow-lists this object by name.
    target_sources(aether-serial-bridge PRIVATE GuardProbe.cpp GuardProbe.h)
endif()
if(WIN32)
    target_link_libraries(aether-serial-bridge PRIVATE asb_win_service_host)   # --service (WS-7)
    target_sources(aether-serial-bridge PRIVATE "${ASB_ICON_RC}")
endif()
asb_apply_dll_policy(aether-serial-bridge)   # INV-67: /DEPENDENTLOADFLAG:0x800 (static Qt)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The Linux --service host (WS-8). Here, not in asb_platform_posix: it creates the engine, and the GUI
    # links the platform library but must never contain the engine (INV-1).
    target_sources(aether-serial-bridge PRIVATE ServiceHost_linux.cpp)
    if(ASB_TEST_SEAMS)
        # --test-op and the CLI's seams: the systemd suite only
        target_sources(aether-serial-bridge PRIVATE TestOps_linux.cpp TestSeams_linux.cpp TestSeams_linux.h)
    endif()
endif()
asb_enable_link_map(aether-serial-bridge)
if(ASB_BUILD_TESTS)
    asb_add_nogui_check(aether-serial-bridge)
    asb_add_import_audit(aether-serial-bridge)
    asb_add_glibc_audit(aether-serial-bridge)
endif()

# --- the GUI executable ---------------------------------------------------------------------
if(ASB_BUILD_GUI)
    # Windows: AetherSerialBridge.exe, a WIN32-subsystem program. Linux: aether-serial-bridge-gui.
    if(WIN32)
        set(ASB_GUI_TARGET AetherSerialBridge)
        qt_add_executable(${ASB_GUI_TARGET} WIN32 main_gui.cpp)
    else()
        set(ASB_GUI_TARGET aether-serial-bridge-gui)
        qt_add_executable(${ASB_GUI_TARGET} main_gui.cpp)
    endif()
    set(ASB_GUI_TARGET "${ASB_GUI_TARGET}" PARENT_SCOPE)
    target_link_libraries(${ASB_GUI_TARGET} PRIVATE asb_app_common asb_gui Qt6::Widgets asb_warnings)
    # v1.1 WS-17 §5: the GUI executable's usage errors, --help and --version without a console of its own.
    target_sources(${ASB_GUI_TARGET} PRIVATE GuiMessage.cpp GuiMessage.h)
    if(WIN32)
        target_link_libraries(${ASB_GUI_TARGET} PRIVATE user32)   # MessageBoxW
        target_sources(${ASB_GUI_TARGET} PRIVATE "${ASB_ICON_RC}")
    endif()
    if(ASB_TEST_SEAMS AND WIN32)
        # guard_installed_before_first_object_gui (INV-112): the Windows window's mode entry, test builds only.
        target_sources(${ASB_GUI_TARGET} PRIVATE GuardProbe.cpp GuardProbe.h)
    endif()
    if(NOT WIN32)
        target_link_libraries(${ASB_GUI_TARGET} PRIVATE asb_tui)   # Linux without a display: the read-only TUI (WS-12)
    endif()
    asb_apply_dll_policy(${ASB_GUI_TARGET})
    asb_enable_link_map(${ASB_GUI_TARGET})   # process_creation_audit (INV-112)
    # The GUI embeds the digest of the finished headless executable, so it is built after it.
    asb_generate_headless_digest(aether-serial-bridge ${ASB_GUI_TARGET})
    if(ASB_BUILD_TESTS)
        asb_add_import_audit(${ASB_GUI_TARGET})
        asb_add_glibc_audit(${ASB_GUI_TARGET})
    endif()
endif()
