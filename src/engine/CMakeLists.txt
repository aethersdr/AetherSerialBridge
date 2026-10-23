# asb_engine: PortBridge, Session, Pump, BridgeHost, QtSerialDevice (WS-4).
# Linked ONLY into the headless executable (INV-1): the GUI never contains the engine
# (tests: link_gui_excludes_engine).
add_library(asb_engine STATIC
    BridgeHost.cpp BridgeHost.h
    ISerialBackend.h
    ISerialDevice.h
    NativeSerial.cpp NativeSerial.h
    PortBridge.cpp PortBridge.h
    Pump.cpp Pump.h
    QtSerialDevice.cpp QtSerialDevice.h
    Session.cpp Session.h
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(asb_engine PRIVATE NativeSerial_linux.cpp)
endif()
target_include_directories(asb_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(asb_engine PUBLIC asb_core Qt6::Network Qt6::SerialPort PRIVATE asb_warnings)
