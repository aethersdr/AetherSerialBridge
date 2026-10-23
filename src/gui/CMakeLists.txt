# asb_gui: the Workspace CanonWindow, stylesheet composition, edit and apply flows (WS-10, WS-11).

# Theme following (WS-11): its own object library, so the INV-31 writer-symbol check can look at
# exactly these objects (tests/CMakeLists.txt, cmake/ThemeWriterCheck.cmake).
add_library(asb_theme OBJECT
    ThemeFollower.cpp ThemeFollower.h
    ThemeDiscovery.cpp ThemeDiscovery.h
    ThemeNameReader.cpp ThemeNameReader.h
    ThemeValidator.cpp ThemeValidator.h)
target_include_directories(asb_theme PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(asb_theme
    PUBLIC aethersdr_canon aethersdr_sqlite Qt6::Gui Qt6::Core
    PRIVATE asb_warnings)

# The window and its flows (WS-10): BridgeWindow on CanonWindow, the composed stylesheet, the port table and card,
# the confirmations, the log pane, the editor model (INV-13/94/107), the read-only view and the operations.
add_library(asb_gui STATIC
    CanonBody.cpp CanonBody.h   # AetherSDR excerpts: canonBodyStyleSheet, Inter, Fusion (WS-2)
    BridgeStyle.cpp BridgeStyle.h
    EditorModel.cpp EditorModel.h
    BindAddress.cpp BindAddress.h
    ViewSource.cpp ViewSource.h
    Actions.h
    PortTableModel.cpp PortTableModel.h
    PortCard.cpp PortCard.h
    Dialogs.cpp Dialogs.h
    BridgeWindow.cpp BridgeWindow.h
    FirewallChecks.cpp FirewallChecks.h
    PlatformHooks.cpp PlatformHooks.h
    WindowSetup.cpp WindowSetup.h
    GuiMain.cpp GuiMain.h)
target_include_directories(asb_gui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(asb_gui
    PUBLIC asb_theme aethersdr_canon aethersdr_sqlite asb_cli asb_tui Qt6::Widgets Qt6::Network
    PRIVATE asb_warnings)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The Linux window's actions: WS-9b's HelperClient over PkexecClient, the product's only process creation
    # (INV-112; allow-listed by object in process_creation_audit for the GUI target).
    target_sources(asb_gui PRIVATE PkexecClient.cpp PkexecClient.h HelperActions.cpp HelperActions.h)
endif()
if(ASB_BUILD_TESTS)
    target_compile_definitions(asb_gui PRIVATE ASB_TEST_SEAMS)   # ASB_TEST_GUI_QUIT_MS (tst_Dispatch)
endif()
if(WIN32)
    target_link_libraries(asb_gui PRIVATE user32 advapi32)   # ShutdownBlockReason*, SERIALCOMM, K32EnumProcessModulesEx
endif()
