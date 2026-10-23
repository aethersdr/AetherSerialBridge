// INV-20 control: an object outside the trust layer that reads config.json directly. The
// ConfigReadSymbols check must reject the library that contains it, or the check has stopped
// seeing references and its pass on asb_core means nothing.

#include "core/ConfigFiles.h"

asb::ConfigFiles::Loaded inv20ControlReadsConfigDirectly()
{
    return asb::ConfigFiles::readCommitted([](const QString&, qint64) { return asb::ConfigFiles::RawRead{}; });
}
