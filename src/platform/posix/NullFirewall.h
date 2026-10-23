#pragma once

// Linux manages no firewall (D7: AllowedClients are recorded, not enforced, and every UI says so), so the
// Applier's firewall steps are no-ops here (WS-8). The engine's monitor on Linux is NoFirewallMonitor
// (Interfaces.h).

#include "core/Interfaces.h"

namespace asb::posix {

class NullFirewall final : public IFirewall {
public:
    void connect(Deadline) override {}
    void sync(const BridgeConfig&, const FirewallGeneration&, Deadline) override {}
    void rollback(const FirewallGeneration&, Deadline) override {}
    void sweep(const FirewallGeneration&, Deadline) override {}
    void removeAll(Deadline) override {}
};

}  // namespace asb::posix
