#pragma once

// QTest failure output for the model types (found by argument-dependent lookup from QCOMPARE).

#include "core/BridgeConfig.h"
#include "core/ConfigJson.h"
#include "core/StatusStore.h"

#include <QTest>

namespace asb {

inline char* toString(const BridgeConfig& c)
{
    return qstrdup(toJson(c).constData());
}

inline char* toString(const PortMapping& p)
{
    BridgeConfig c;
    c.ports = {p};
    return qstrdup(toJson(c).constData());
}

inline char* toString(const StatusReport& r)
{
    return qstrdup(StatusStore::toJson(r).constData());
}

}  // namespace asb
