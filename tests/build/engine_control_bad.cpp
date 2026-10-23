// INV-1 control (WS-4): an executable that DOES link the engine. link_gui_excludes_engine's check must
// reject it on every layer, or a pass on the GUI executable means nothing.

#include "engine/QtSerialDevice.h"

int main()
{
    return asb::engine::toSerialError(QSerialPort::NoError) == asb::engine::SerialError::None ? 0 : 1;
}
