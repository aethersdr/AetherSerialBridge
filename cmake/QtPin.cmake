# QtPin.cmake: find Qt and enforce the pin and the floors in cmake/qt-pin.env.
#
#   asb_find_qt()
#
# Sets ASB_QT_PIN_VERSION, ASB_QT_PIN_REVISION, ASB_QT_GUI_FLOOR, ASB_QT_HEADLESS_FLOOR,
# ASB_HAVE_QT_GUI (Qt6::Gui is available: needed by the NoQtGuiCheck control target) and, in the
# caller's scope, finds Qt6.
#
# Floors (master section 9): the GUI needs Qt >= QT_GUI_FLOOR, the headless build accepts
# QT_HEADLESS_FLOOR so a distro Qt works (distro-arm leg). With ASB_REQUIRE_PINNED_QT=ON the Qt
# found must be exactly QT_VERSION, which is how the aqt legs prove they did not silently build
# against a distro Qt.

set(_asb_pin_file "${CMAKE_CURRENT_LIST_DIR}/qt-pin.env")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_asb_pin_file}")

file(STRINGS "${_asb_pin_file}" _asb_pin_lines REGEX "^[A-Z0-9_]+=")
foreach(_line IN LISTS _asb_pin_lines)
    string(REGEX MATCH "^([A-Z0-9_]+)=\"?([^\"]*)\"?$" _ "${_line}")
    set(_asb_pin_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()

foreach(_key QT_VERSION QT_PACKAGE_REVISION QT_GUI_FLOOR QT_HEADLESS_FLOOR)
    if(NOT _asb_pin_${_key})
        message(FATAL_ERROR "${_asb_pin_file} is missing ${_key}.")
    endif()
endforeach()
set(ASB_QT_PIN_VERSION    "${_asb_pin_QT_VERSION}")
set(ASB_QT_PIN_REVISION   "${_asb_pin_QT_PACKAGE_REVISION}")
set(ASB_QT_GUI_FLOOR      "${_asb_pin_QT_GUI_FLOOR}")
set(ASB_QT_HEADLESS_FLOOR "${_asb_pin_QT_HEADLESS_FLOOR}")

macro(asb_find_qt)
    set(_asb_qt_components Core Network SerialPort)
    if(ASB_BUILD_TESTS)
        list(APPEND _asb_qt_components Test)
    endif()
    if(NOT WIN32)
        list(APPEND _asb_qt_components DBus)
    endif()
    if(ASB_BUILD_GUI)
        list(APPEND _asb_qt_components Widgets)
        set(_asb_qt_floor "${ASB_QT_GUI_FLOOR}")
    else()
        set(_asb_qt_floor "${ASB_QT_HEADLESS_FLOOR}")
    endif()

    # Probe QUIET first so a below-floor Qt gets an actionable error rather than CMake's bare
    # "the version found is not compatible".
    find_package(Qt6 QUIET COMPONENTS Core)
    if(Qt6_FOUND AND Qt6_VERSION VERSION_LESS "${_asb_qt_floor}")
        message(FATAL_ERROR
            "Qt ${Qt6_VERSION} found, but this configuration needs Qt ${_asb_qt_floor} or newer "
            "(ASB_BUILD_GUI=${ASB_BUILD_GUI}). The pinned Qt is ${ASB_QT_PIN_VERSION}; "
            "ci/scripts/install-qt.sh installs it. A headless-only build "
            "(-DASB_BUILD_GUI=OFF) accepts Qt ${ASB_QT_HEADLESS_FLOOR}.")
    endif()
    find_package(Qt6 "${_asb_qt_floor}" REQUIRED COMPONENTS ${_asb_qt_components})

    if(ASB_REQUIRE_PINNED_QT AND NOT Qt6_VERSION VERSION_EQUAL "${ASB_QT_PIN_VERSION}")
        message(FATAL_ERROR
            "ASB_REQUIRE_PINNED_QT=ON: found Qt ${Qt6_VERSION}, the pin is ${ASB_QT_PIN_VERSION}.")
    endif()

    # The NoQtGuiCheck control target links QtGui. A -no-gui Qt (the static headless profile,
    # WS-13) has none, and the control is then built by a Gui-capable toolchain instead.
    find_package(Qt6 "${_asb_qt_floor}" QUIET COMPONENTS Gui)
    if(Qt6Gui_FOUND)
        set(ASB_HAVE_QT_GUI ON)
    else()
        set(ASB_HAVE_QT_GUI OFF)
    endif()
    message(STATUS "Qt ${Qt6_VERSION} (pin ${ASB_QT_PIN_VERSION}, floor ${_asb_qt_floor}, "
                   "GUI build ${ASB_BUILD_GUI}, QtGui available ${ASB_HAVE_QT_GUI})")
endmacro()
