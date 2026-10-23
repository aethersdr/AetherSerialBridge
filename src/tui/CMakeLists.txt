# asb_tui: the FTXUI interface of both executables (WS-12; master section 6). FTXUI comes from cmake/Ftxui.cmake.
# The logo is generated: tools/gen_logo_art.py writes LogoArt.inc (CI runs it with --check).
add_library(asb_tui STATIC
    Tui.cpp Tui.h
    TuiApp.cpp TuiApp.h
    TuiView.cpp TuiView.h
    TuiHelp.cpp TuiHelp.h
    TuiState.h TuiText.h
    TuiPalette.cpp TuiPalette.h
    Logo.cpp Logo.h LogoArt.inc)
target_include_directories(asb_tui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(asb_tui PUBLIC asb_cli asb_core Qt6::Core ftxui::component ftxui::dom ftxui::screen)
target_link_libraries(asb_tui PRIVATE asb_warnings)
