# INV-88 (WS-13): the plant of the release GUI self-test (packaging/windows/release-gui-self-test.ps1). Built in the
# test tree only; the release workflow copies it beside the UNZIPPED release executables under every planted name.
add_library(asb_marker_plant SHARED marker_plant.c)
set_target_properties(asb_marker_plant PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plant"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plant"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plant-lib")
