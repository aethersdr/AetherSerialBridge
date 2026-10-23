# NoQtGuiCheck.cmake: INV-37, the headless executable contains no QtGui code.
#
# This file has two faces.
#
#  Included as a module it defines
#      asb_write_link_graphs(<target>...)     the link closure of each target, at configure time
#      asb_enable_link_map(<target>)          ask the linker for a map file
#      asb_add_nogui_check(<target>)          CTest: the target must be clean
#      asb_add_nogui_control(<target>)        CTests: the deliberately bad target must be rejected
#
#  Run as a script (cmake -P) it performs the check on one executable:
#      -DEXE=<file> -DGRAPH=<closure file> -DMAP=<link map> -DREADELF/-DNM (ELF) or -DDUMPBIN (PE)
#      -DLAYERS=graph;map;artifact   (default: all three); each layer needs only its own input
#                                    (graph: GRAPH, map: MAP, artifact: EXE)
#      -DNAME=<label>                what the messages call the subject (default: the EXE file name)
#      -DGRAPH_PATTERN=<regex>       test hook: replaces the graph layer's forbidden-name regex, which
#                                    is matched against lower-cased names; used only to prove what the
#                                    closure of a Qt module contains
#      -DMODE=check   exit 1 on the first layer that finds QtGui   (prints "NoQtGuiCheck: VIOLATION")
#      -DMODE=control exit 0 only if EVERY selected layer finds QtGui (the control proves each layer)
#
# Three independent layers look at three different things, so a mistake in one does not hide a
# regression:
#   graph     the CMake link closure (Qt6::Gui, Qt6::Widgets, ..., aethersdr_*, asb_gui), and any
#             library file or -l flag in it that names a QtGui module
#   map       the linker map of the executable
#   artifact  the finished binary: NEEDED / dependents, QtGui class symbols, and Qt6Gui strings
#
# A missing input or a missing tool is an error, never a skip: a check that cannot run must not
# look like a check that passed.

# Qt modules that pull in QtGui (or are GUI-only): none of them may be reachable from headless.
set(_ASB_GUI_MODULES "Gui|Widgets|OpenGL|OpenGLWidgets|PrintSupport|Svg|SvgWidgets|Multimedia|MultimediaWidgets|Quick|QuickWidgets|Qml")

# ---------------------------------------------------------------------------------------------
# Script mode
# ---------------------------------------------------------------------------------------------
if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    cmake_minimum_required(VERSION 3.22)
    if(NOT MODE)
        set(MODE check)
    endif()
    if(NOT MODE STREQUAL "check" AND NOT MODE STREQUAL "control")
        message(FATAL_ERROR "NoQtGuiCheck: MODE must be check or control, got '${MODE}'")
    endif()
    if(NOT LAYERS)
        set(LAYERS "graph;map;artifact")
    endif()
    # A layer that is not a layer would be skipped in check mode and pass by omission.
    foreach(_l IN LISTS LAYERS)
        if(NOT _l MATCHES "^(graph|map|artifact)$")
            message(FATAL_ERROR "NoQtGuiCheck: unknown layer '${_l}' (graph, map, artifact)")
        endif()
    endforeach()
    # Each selected layer must have its input: a check that cannot run is an error, not a skip.
    set(_input_graph GRAPH)
    set(_input_map MAP)
    set(_input_artifact EXE)
    foreach(_l IN LISTS LAYERS)
        set(_v ${_input_${_l}})
        if(NOT ${_v} OR NOT EXISTS "${${_v}}")
            message(FATAL_ERROR "NoQtGuiCheck: input ${_v} (layer ${_l}) missing or not a file: '${${_v}}'")
        endif()
    endforeach()
    if(NAME)
        set(_exe_name "${NAME}")
    elseif(EXE)
        get_filename_component(_exe_name "${EXE}" NAME)
    else()
        get_filename_component(_exe_name "${GRAPH}" NAME_WE)
    endif()

    # Names are matched lower-cased: a target spelled qt6::gui is the same dependency to a reader.
    # A module name may carry a suffix (Qt6::GuiPrivate, Qt6::Quick3D, ...). The "foreign:" lines are
    # library files and flags (libQt6Gui.so.6, -lQt6Gui, Qt6Gui.lib) written by the closure walk.
    string(TOLOWER "${_ASB_GUI_MODULES}" _gui_modules_lc)
    set(_graph_re "^(qt6|qt)::(${_gui_modules_lc})[a-z0-9_]*$|^aethersdr_(canon|sqlite)$|^asb_gui$|^foreign:")
    if(GRAPH_PATTERN)
        set(_graph_re "${GRAPH_PATTERN}")
    endif()
    set(_name_re "Qt6(${_ASB_GUI_MODULES})")
    # Class symbols that only exist if QtGui / QtWidgets code is in (or imported by) the binary.
    set(_sym_re "(^|[^A-Za-z0-9_])Q(Color|Widget|GuiApplication|Application|Image|Pixmap|Painter|Brush|Font|Palette|Icon|Window|Screen)::")

    set(_found_layers "")      # layers that found QtGui
    set(_evidence "")

    # --- graph -----------------------------------------------------------------------------
    if("graph" IN_LIST LAYERS)
        file(STRINGS "${GRAPH}" _closure)
        set(_hits "")
        foreach(_t IN LISTS _closure)
            string(TOLOWER "${_t}" _t_lc)
            if(_t_lc MATCHES "${_graph_re}")
                list(APPEND _hits "${_t}")
            endif()
        endforeach()
        if(_hits)
            list(APPEND _found_layers graph)
            list(APPEND _evidence "graph: link closure reaches ${_hits}")
        endif()
    endif()

    # --- map -------------------------------------------------------------------------------
    if("map" IN_LIST LAYERS)
        file(STRINGS "${MAP}" _map_hits REGEX "${_name_re}" LIMIT_COUNT 5)
        if(_map_hits)
            list(APPEND _found_layers map)
            list(APPEND _evidence "map: ${MAP} mentions QtGui: ${_map_hits}")
        endif()
    endif()

    # --- artifact --------------------------------------------------------------------------
    if("artifact" IN_LIST LAYERS)
        set(_art_hits "")
        if(WIN32)
            if(NOT DUMPBIN)
                message(FATAL_ERROR "NoQtGuiCheck: -DDUMPBIN is required on Windows")
            endif()
            execute_process(COMMAND "${DUMPBIN}" /nologo /dependents "${EXE}"
                            OUTPUT_VARIABLE _dep RESULT_VARIABLE _rc ERROR_VARIABLE _err)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "NoQtGuiCheck: dumpbin /dependents failed (${_rc}): ${_err}")
            endif()
            if(_dep MATCHES "${_name_re}")
                list(APPEND _art_hits "dependents: ${CMAKE_MATCH_0}")
            endif()
        else()
            if(NOT READELF OR NOT NM)
                message(FATAL_ERROR "NoQtGuiCheck: -DREADELF and -DNM are required on ELF platforms")
            endif()
            execute_process(COMMAND "${READELF}" -d "${EXE}"
                            OUTPUT_VARIABLE _dyn RESULT_VARIABLE _rc ERROR_VARIABLE _err)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "NoQtGuiCheck: readelf -d failed (${_rc}): ${_err}")
            endif()
            if(_dyn MATCHES "${_name_re}")
                list(APPEND _art_hits "NEEDED: ${CMAKE_MATCH_0}")
            endif()
            # Static Qt (WS-13) leaves no NEEDED entry, so look at the symbols as well. nm fails on
            # a binary with no static symbol table; the dynamic table is read too.
            foreach(_nmargs "-C" "-D;-C")
                execute_process(COMMAND "${NM}" ${_nmargs} "${EXE}"
                                OUTPUT_VARIABLE _syms RESULT_VARIABLE _rc ERROR_QUIET)
                if(_rc EQUAL 0 AND _syms MATCHES "${_sym_re}")
                    list(APPEND _art_hits "symbol: ${CMAKE_MATCH_0}")
                    break()
                endif()
            endforeach()
        endif()
        # The shared-object / import-library name as a string anywhere in the file.
        file(STRINGS "${EXE}" _strs REGEX "${_name_re}" LIMIT_COUNT 3)
        if(_strs)
            list(APPEND _art_hits "string: ${_strs}")
        endif()
        if(_art_hits)
            list(APPEND _found_layers artifact)
            list(APPEND _evidence "artifact: ${_art_hits}")
        endif()
    endif()

    foreach(_e IN LISTS _evidence)
        message(STATUS "NoQtGuiCheck: ${_exe_name}: ${_e}")
    endforeach()

    if(MODE STREQUAL "check")
        if(_found_layers)
            message(FATAL_ERROR "NoQtGuiCheck: VIOLATION: ${_exe_name} contains QtGui code "
                                "(layers: ${_found_layers})")
        endif()
        message(STATUS "NoQtGuiCheck: OK: ${_exe_name} (layers checked: ${LAYERS})")
    else()
        set(_missed "")
        foreach(_l IN LISTS LAYERS)
            if(NOT _l IN_LIST _found_layers)
                list(APPEND _missed ${_l})
            endif()
        endforeach()
        if(_missed)
            message(FATAL_ERROR "NoQtGuiCheck: CONTROL NOT DETECTED by layer(s) ${_missed} for "
                                "${_exe_name}: the check can no longer see QtGui")
        endif()
        message(STATUS "NoQtGuiCheck: control ${_exe_name} detected by every layer (${LAYERS})")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------------------------
# Module mode
# ---------------------------------------------------------------------------------------------
find_program(ASB_READELF NAMES readelf)
find_program(ASB_NM      NAMES nm)
if(WIN32)
    get_filename_component(_asb_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(ASB_DUMPBIN NAMES dumpbin HINTS "${_asb_linker_dir}")
endif()

# The names reachable from <target>, including the target itself: every target the walk can reach
# plus "foreign:<token>" for every library file or flag that names a QtGui module.
#
# Generator expressions are not evaluated. Every name inside one is followed, so a conditional or
# link-only dependency counts (the safe direction for a "must not contain" check): the value is
# split at the expression syntax ($< > , ; and a single :) while "::" stays part of a target name,
# so $<$<CONFIG:Debug>:Qt6::Gui>, $<LINK_ONLY:Qt6::Gui>, $<TARGET_NAME:Qt6::Gui> and nested forms
# all yield Qt6::Gui. The cost is that a name used only as a condition ($<TARGET_EXISTS:Qt6::Gui>)
# also counts; for this check a false alarm is the right failure.
#
# What the walk reads: the link properties of every target it reaches (including an ALIAS's real
# target, IMPORTED_LINK_INTERFACE_LIBRARIES and its per-configuration forms, link options and
# flags) and the objects $<TARGET_OBJECTS:...> pulls in through SOURCES. What it cannot see (a name
# built by a generator expression, CMAKE_EXE_LINKER_FLAGS, an alias or imported target with a plain
# name that lives in a subdirectory the graphs are not written from, a QtGui header used without
# linking) is the job of the map and artifact layers.
function(asb_nogui_tokens value out)
    string(REPLACE "::" "@NS@" v "${value}")
    string(REGEX REPLACE "[^A-Za-z0-9_+.@-]+" ";" v "${v}")
    string(REPLACE "@NS@" "::" v "${v}")
    set(${out} "${v}" PARENT_SCOPE)
endfunction()

function(asb_link_closure target out)
    string(TOLOWER "${_ASB_GUI_MODULES}" modules_lc)
    set(queue "${target}")
    set(seen "")
    set(foreign "")
    while(queue)
        list(POP_FRONT queue t)
        if(t IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${t}")
        if(NOT TARGET "${t}")
            continue()
        endif()
        # An ALIAS reads like its real target but carries another name: follow both.
        get_target_property(aliased "${t}" ALIASED_TARGET)
        if(aliased)
            list(APPEND queue "${aliased}")
        endif()
        get_target_property(type "${t}" TYPE)
        get_target_property(imported "${t}" IMPORTED)
        set(props INTERFACE_LINK_LIBRARIES INTERFACE_LINK_LIBRARIES_DIRECT INTERFACE_LINK_OPTIONS
                  INTERFACE_SOURCES)
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            list(APPEND props LINK_LIBRARIES LINK_OPTIONS LINK_FLAGS SOURCES)
        endif()
        if(imported)
            list(APPEND props IMPORTED_LINK_INTERFACE_LIBRARIES IMPORTED_LINK_DEPENDENT_LIBRARIES)
            get_target_property(configs "${t}" IMPORTED_CONFIGURATIONS)
            foreach(cfg IN LISTS configs)
                list(APPEND props IMPORTED_LINK_INTERFACE_LIBRARIES_${cfg}
                                  IMPORTED_LINK_DEPENDENT_LIBRARIES_${cfg})
            endforeach()
        endif()
        foreach(prop IN LISTS props)
            get_target_property(value "${t}" "${prop}")
            if(NOT value)
                continue()
            endif()
            if(prop MATCHES "SOURCES$")
                # Source lists are file names; only the objects of another target are a dependency.
                string(REGEX MATCHALL "\\$<TARGET_OBJECTS:[^>]+>" objects "${value}")
                set(value "${objects}")
            endif()
            asb_nogui_tokens("${value}" tokens)
            foreach(tok IN LISTS tokens)
                if(tok STREQUAL "")
                    continue()
                endif()
                if(TARGET "${tok}")
                    if(NOT tok IN_LIST seen)
                        list(APPEND queue "${tok}")
                    endif()
                    continue()
                endif()
                # Not a target the walk can see. A namespaced name (Qt6::Widgets) is kept as it is: an
                # imported target found in a subdirectory is not visible from the directory where the
                # graphs are written, and dropping it would let it through.
                if(tok MATCHES "::")
                    list(APPEND seen "${tok}")
                    continue()
                endif()
                # A library file or a -l flag: no target to follow, but its name still says QtGui.
                string(TOLOWER "${tok}" tok_lc)
                if(tok_lc MATCHES "qt[56]?(${modules_lc})")
                    list(APPEND foreign "foreign:${tok}")
                endif()
            endforeach()
        endforeach()
    endwhile()
    list(REMOVE_DUPLICATES foreign)
    set(${out} "${seen};${foreign}" PARENT_SCOPE)
endfunction()

# Every target whose closure is checked registers here (asb_add_nogui_check/control, the graph-shape
# fixtures); the root CMakeLists writes the graphs once all targets exist.
function(asb_nogui_register_graph target)
    set_property(GLOBAL APPEND PROPERTY ASB_NOGUI_GRAPH_TARGETS "${target}")
endfunction()

function(asb_write_link_graphs)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/nogui")
    get_property(registered GLOBAL PROPERTY ASB_NOGUI_GRAPH_TARGETS)
    list(APPEND registered ${ARGN})
    list(REMOVE_DUPLICATES registered)
    foreach(t IN LISTS registered)
        asb_link_closure("${t}" closure)
        list(JOIN closure "\n" body)
        file(WRITE "${CMAKE_BINARY_DIR}/nogui/${t}.graph" "${body}\n")
    endforeach()
endfunction()

function(asb_enable_link_map target)
    if(MSVC)
        target_link_options(${target} PRIVATE "/MAP:$<TARGET_FILE_DIR:${target}>/${target}.map")
    else()
        target_link_options(${target} PRIVATE "LINKER:-Map=$<TARGET_FILE:${target}>.map")
    endif()
endfunction()

function(_asb_nogui_args target out)
    if(MSVC)
        set(map "$<TARGET_FILE_DIR:${target}>/${target}.map")
    else()
        set(map "$<TARGET_FILE:${target}>.map")
    endif()
    set(args
        "-DEXE=$<TARGET_FILE:${target}>"
        "-DGRAPH=${CMAKE_BINARY_DIR}/nogui/${target}.graph"
        "-DMAP=${map}")
    if(WIN32)
        list(APPEND args "-DDUMPBIN=${ASB_DUMPBIN}")
    else()
        list(APPEND args "-DREADELF=${ASB_READELF}" "-DNM=${ASB_NM}")
    endif()
    set(${out} "${args}" PARENT_SCOPE)
endfunction()

function(asb_add_nogui_check target)
    asb_nogui_register_graph(${target})
    asb_enable_link_map(${target})
    _asb_nogui_args(${target} args)
    add_test(NAME NoQtGuiCheck.${target}
             COMMAND ${CMAKE_COMMAND} ${args} -DMODE=check
                     -P "${CMAKE_SOURCE_DIR}/cmake/NoQtGuiCheck.cmake")
    set_tests_properties(NoQtGuiCheck.${target} PROPERTIES LABELS "nogui;build")
endfunction()

function(asb_add_nogui_control target)
    asb_nogui_register_graph(${target})
    asb_enable_link_map(${target})
    _asb_nogui_args(${target} args)
    # 1. The same check that passes the headless executable rejects the control. The regular
    #    expression is only printed on a real violation, so a crashed or misconfigured run does
    #    not pass by accident.
    add_test(NAME NoQtGuiCheck.${target}_rejected
             COMMAND ${CMAKE_COMMAND} ${args} -DMODE=check
                     -P "${CMAKE_SOURCE_DIR}/cmake/NoQtGuiCheck.cmake")
    set_tests_properties(NoQtGuiCheck.${target}_rejected PROPERTIES
        PASS_REGULAR_EXPRESSION "NoQtGuiCheck: VIOLATION: ${target}"
        LABELS "nogui;build")
    # 2. Each layer sees the control on its own.
    foreach(layer graph map artifact)
        add_test(NAME NoQtGuiCheck.${target}_layer_${layer}
                 COMMAND ${CMAKE_COMMAND} ${args} -DMODE=control -DLAYERS=${layer}
                         -P "${CMAKE_SOURCE_DIR}/cmake/NoQtGuiCheck.cmake")
        set_tests_properties(NoQtGuiCheck.${target}_layer_${layer} PROPERTIES LABELS "nogui;build")
    endforeach()
endfunction()
