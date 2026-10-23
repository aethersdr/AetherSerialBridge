# Graph-layer fixtures for NoQtGuiCheck (INV-37).
#
# The graph layer reads CMake target properties, which hold unevaluated text: a dependency can be
# spelled as a target, wrapped in generator expressions, hidden behind an alias or an imported
# target, or be a plain library file or -l flag. Each shape below is a target (never built:
# EXCLUDE_FROM_ALL) whose closure is run through the graph layer ALONE:
#
#   detected  the layer must flag it         (control mode: exit 0 only if the layer finds QtGui)
#   clean     the layer must pass it         (check mode) AND must have read Qt6::Core, so a walk
#                                            that returns nothing cannot pass for "clean"
#
# Every shape is wired two ways: on the executable itself (its LINK_LIBRARIES) and through a library
# the executable links (that library's INTERFACE_LINK_LIBRARIES). Test names are
# NoQtGuiCheck.graph_shape_<shape>_<wiring>[_sees_core].
#
# Included from the root CMakeLists.txt (not from tests/): the graphs are written from the root
# directory, and an alias or an imported target is only visible in the directory that made it and
# below, so the fixtures must live there to be walked by name.
#
# Included only when QtGui is available to link.

set(ASB_GS_SOURCE "${CMAKE_SOURCE_DIR}/tests/build/nogui_control_bad.cpp")
set(_gs_nowhere "${CMAKE_BINARY_DIR}/no-such-dir")

function(_asb_gs_test test_name mode target)
    add_test(NAME NoQtGuiCheck.${test_name}
             COMMAND ${CMAKE_COMMAND} -DNAME=${target} -DGRAPH=${CMAKE_BINARY_DIR}/nogui/${target}.graph
                     -DLAYERS=graph -DMODE=${mode} ${ARGN}
                     -P "${CMAKE_SOURCE_DIR}/cmake/NoQtGuiCheck.cmake")
    set_tests_properties(NoQtGuiCheck.${test_name} PROPERTIES LABELS "nogui;build;graphshape")
endfunction()

# Register the tests for an existing target.
function(asb_graph_expect shape expect target)
    asb_nogui_register_graph(${target})
    if(expect STREQUAL "detected")
        _asb_gs_test(graph_shape_${shape} control ${target})
    else()
        _asb_gs_test(graph_shape_${shape} check ${target})
        _asb_gs_test(graph_shape_${shape}_sees_core control ${target} "-DGRAPH_PATTERN=^qt6::core$")
    endif()
endfunction()

# The closure of <target> must contain a name matching <pattern> (lower-cased), whatever the module
# list says: this proves what a Qt module forwards to.
function(asb_graph_closure_contains shape target pattern)
    asb_nogui_register_graph(${target})
    _asb_gs_test(graph_shape_${shape} control ${target} "-DGRAPH_PATTERN=${pattern}")
endfunction()

# <items> as target_link_libraries() arguments, on an executable and behind a library.
function(asb_graph_shape shape expect)
    add_executable(asb_gs_${shape}_direct EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
    target_link_libraries(asb_gs_${shape}_direct PRIVATE ${ARGN})
    asb_graph_expect(${shape}_direct ${expect} asb_gs_${shape}_direct)

    add_library(asb_gs_${shape}_lib INTERFACE)
    target_link_libraries(asb_gs_${shape}_lib INTERFACE ${ARGN})
    add_executable(asb_gs_${shape}_via_lib EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
    target_link_libraries(asb_gs_${shape}_via_lib PRIVATE asb_gs_${shape}_lib)
    asb_graph_expect(${shape}_via_lib ${expect} asb_gs_${shape}_via_lib)
endfunction()

# --- the target, and generator expressions around it -----------------------------------------
asb_graph_shape(target                 detected Qt6::Gui)
asb_graph_shape(link_only              detected "$<LINK_ONLY:Qt6::Gui>")
asb_graph_shape(config_conditional     detected "$<$<CONFIG:Debug>:Qt6::Gui>")
asb_graph_shape(nested                 detected "$<$<NOT:$<CONFIG:Release>>:$<LINK_ONLY:Qt6::Gui>>")
asb_graph_shape(config_if              detected "$<IF:$<CONFIG:Debug>,Qt6::Gui,Qt6::Core>")
asb_graph_shape(target_name            detected "$<TARGET_NAME:Qt6::Gui>")
asb_graph_shape(target_name_if_exists  detected "$<TARGET_NAME_IF_EXISTS:Qt6::Gui>")
asb_graph_shape(build_interface        detected "$<BUILD_INTERFACE:Qt6::Gui>")
asb_graph_shape(install_interface      detected "$<INSTALL_INTERFACE:Qt6::Gui>")
asb_graph_shape(target_linker_file     detected "$<TARGET_LINKER_FILE:Qt6::Gui>")
set(_gs_module Gui)
asb_graph_shape(variable_built         detected "Qt6::${_gs_module}")

# --- imported targets, aliases ----------------------------------------------------------------
add_library(asb_gs_alias_of_gui ALIAS Qt6::Gui)
asb_graph_shape(alias                  detected asb_gs_alias_of_gui)

add_library(asb_gs_forwarder INTERFACE IMPORTED)
set_target_properties(asb_gs_forwarder PROPERTIES INTERFACE_LINK_LIBRARIES "Qt6::Gui")
asb_graph_shape(imported_forwarder     detected asb_gs_forwarder)

# Qt's own modules forward to Qt6::Gui through INTERFACE_LINK_LIBRARIES. Each is checked twice:
# flagged as a whole, and with the pattern ^qt6::gui$ so the module list is not what finds it.
# Only against the pinned Qt: a distribution Qt does not ship every package, and ships some
# (Qt6::GuiPrivate without its private headers) in a state CMake refuses to generate.
if(ASB_REQUIRE_PINNED_QT)
    # Qt6::GuiPrivate is the point of its fixture; Qt warns on every use of a private module.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    foreach(_gs_mod Widgets OpenGL PrintSupport GuiPrivate)
        find_package(Qt6 QUIET COMPONENTS ${_gs_mod})
        if(NOT TARGET Qt6::${_gs_mod})
            message(FATAL_ERROR "Qt6::${_gs_mod} is missing from the pinned Qt: its graph shape cannot run")
        endif()
        string(TOLOWER "${_gs_mod}" _gs_mod_lc)
        asb_graph_shape(${_gs_mod_lc} detected Qt6::${_gs_mod})
        if(NOT _gs_mod STREQUAL "GuiPrivate")
            asb_graph_closure_contains(${_gs_mod_lc}_forwards_to_gui_direct  asb_gs_${_gs_mod_lc}_direct  "^qt6::gui$")
            asb_graph_closure_contains(${_gs_mod_lc}_forwards_to_gui_via_lib asb_gs_${_gs_mod_lc}_via_lib "^qt6::gui$")
        endif()
    endforeach()
else()
    message(STATUS "Graph shapes for Qt modules (Widgets, OpenGL, PrintSupport, GuiPrivate, Svg) need the pinned Qt: skipped")
endif()

if(TARGET Qt::Gui)
    asb_graph_shape(versionless_alias  detected Qt::Gui)
endif()

# A target spelled in lower case (names are compared case-insensitively).
add_library(qt6::gui INTERFACE IMPORTED)
asb_graph_shape(lowercase_target       detected qt6::gui)

# The legacy imported-target spelling: IMPORTED_LINK_INTERFACE_LIBRARIES, plain and per configuration.
add_library(asb_gs_legacy_plain STATIC IMPORTED)
set_target_properties(asb_gs_legacy_plain PROPERTIES
    IMPORTED_LOCATION "${_gs_nowhere}/asb_gs_legacy_plain.a"
    IMPORTED_LINK_INTERFACE_LIBRARIES "Qt6::Gui")
asb_graph_shape(imported_legacy_plain  detected asb_gs_legacy_plain)
add_library(asb_gs_legacy_config STATIC IMPORTED)
set_target_properties(asb_gs_legacy_config PROPERTIES
    IMPORTED_CONFIGURATIONS NOCONFIG
    IMPORTED_LOCATION_NOCONFIG "${_gs_nowhere}/asb_gs_legacy_config.a"
    IMPORTED_LINK_INTERFACE_LIBRARIES_NOCONFIG "Qt6::Gui")
asb_graph_shape(imported_legacy_config detected asb_gs_legacy_config)

# --- library files and flags: no target to follow --------------------------------------------
asb_graph_shape(file_so                detected "${_gs_nowhere}/libQt6Gui.so.6")
asb_graph_shape(file_import_lib        detected "${_gs_nowhere}/Qt6Gui.lib")
asb_graph_shape(file_debug_import_lib  detected "${_gs_nowhere}/Qt6Guid.lib")
asb_graph_shape(flag_l                 detected -lQt6Gui)
asb_graph_shape(flag_l_lowercase       detected -lqt6gui)
asb_graph_shape(flag_in_expression     detected "$<$<CXX_COMPILER_ID:GNU,Clang,MSVC>:-lQt6Gui>")
asb_graph_shape(flag_after_comma       detected "-Wl,--no-as-needed,-lQt6Gui")

# --- other ways a dependency reaches an executable --------------------------------------------
# A static library's PRIVATE dependency reaches its consumers as $<LINK_ONLY:...>.
add_library(asb_gs_static STATIC EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
target_link_libraries(asb_gs_static PRIVATE Qt6::Gui)
add_executable(asb_gs_static_private EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
target_link_libraries(asb_gs_static_private PRIVATE asb_gs_static)
asb_graph_expect(static_private detected asb_gs_static_private)
if(NOT CMAKE_VERSION VERSION_LESS 3.24)
    # A link-library feature around the name of a static library that reaches Qt6::Gui.
    asb_graph_shape(link_library_feature detected "$<LINK_LIBRARY:WHOLE_ARCHIVE,asb_gs_static>")
endif()

# An OBJECT library pulled in through $<TARGET_OBJECTS:...> in SOURCES.
add_library(asb_gs_objects OBJECT EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
target_link_libraries(asb_gs_objects PRIVATE Qt6::Gui)
add_executable(asb_gs_object_files EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
target_sources(asb_gs_object_files PRIVATE $<TARGET_OBJECTS:asb_gs_objects>)
asb_graph_expect(object_files detected asb_gs_object_files)

# Link options and flags.
add_executable(asb_gs_link_options EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
target_link_options(asb_gs_link_options PRIVATE -lQt6Gui)
asb_graph_expect(link_options detected asb_gs_link_options)
add_executable(asb_gs_link_flags EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
set_target_properties(asb_gs_link_flags PROPERTIES LINK_FLAGS "-lQt6Gui")
asb_graph_expect(link_flags detected asb_gs_link_flags)

# link_libraries() in the directory scope.
add_subdirectory(tests/graph_shapes_dirscope)
asb_graph_expect(directory_scope detected asb_gs_dirscope)
if(ASB_REQUIRE_PINNED_QT)
    add_subdirectory(tests/graph_shapes_unresolved)
    if(NOT ASB_GS_HAVE_UNRESOLVED)
        message(FATAL_ERROR "Qt6::Svg is missing from the pinned Qt: the unresolved_namespaced graph shape cannot run")
    endif()
    asb_graph_expect(unresolved_namespaced detected asb_gs_unresolved_namespaced)
endif()

# --- clean shapes: the layer must not cry wolf ------------------------------------------------
asb_graph_shape(clean_target           clean Qt6::Core)
asb_graph_shape(clean_link_only        clean "$<LINK_ONLY:Qt6::Core>")
asb_graph_shape(clean_config           clean "$<$<CONFIG:Debug>:Qt6::Core>")
asb_graph_shape(clean_target_name      clean "$<TARGET_NAME:Qt6::Core>")
asb_graph_shape(clean_modules          clean "Qt6::Core;Qt6::Network;Qt6::SerialPort")
asb_graph_shape(clean_file             clean "Qt6::Core;${_gs_nowhere}/libQt6Core.so.6")
asb_graph_shape(clean_flag             clean "Qt6::Core;-lQt6Core")
# A path is a file, not the targets its folders are named after: asb_gs_static (above) reaches Qt6::Gui, and a
# library in a folder of that name is still clean. The same with a drive letter, whatever the host.
asb_graph_shape(clean_file_in_target_dir       clean "Qt6::Core;${_gs_nowhere}/asb_gs_static/libQt6Core.so.6")
asb_graph_shape(clean_file_in_target_dir_drive clean "Qt6::Core;C:/no-such-dir/asb_gs_static/Qt6Core.lib")
