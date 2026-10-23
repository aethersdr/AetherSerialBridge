#pragma once

// IFirewall and IFirewallMonitor on Windows, in-process through CIM (WS-7; D51, D53, D56; INV-11, INV-114,
// INV-117). Portable logic over ICimSession (CimSeams.h).
//
// CimFirewall (the Applier's; every call synchronous on the operation worker, INV-114):
//  - recipe per rule (FirewallPlan::syncSteps order, all in the PersistentStore):
//      create   MSFT_NetFirewallRule {InstanceID, ElementName (= DisplayName), RuleGroup, Direction 1, Action 2,
//               Enabled 2 (False), Profiles 0}                                  WBEM_FLAG_CREATE_ONLY
//      filters  the rule's own MSFT_NetProtocolPortFilter {Protocol "TCP", LocalPort [port]},
//               MSFT_NetAddressFilter {RemoteAddress = writer values}, MSFT_NetApplicationFilter
//               {AppPath = the installed exe}, each found through its association  WBEM_FLAG_UPDATE_ONLY
//      enable   the rule {Enabled 1}, only after all three filter updates succeeded  WBEM_FLAG_UPDATE_ONLY
//    (a late update can never recreate a deleted rule: UPDATE_ONLY on a deleted rule is WBEM_E_NOT_FOUND).
//  - sync = create every new rule (above), then delete every rule in the group not of the generation; rollback(g)
//    deletes only g's rules; sweep(keep) every rule not of `keep`; removeAll every rule in the group.
//  - connect() is called exactly once per operation, first (WS-5 round 4); any other call while unconnected
//    throws PlatformError (a step failure), never crashes and never connects on its own.
//  - deadlines: no further normal step (connect, create, filter, enable, sweep/removeAll delete) starts once the
//    deadline has passed; the call throws instead (the Applier then runs failClosed). rollback, the cleanup of the
//    generation that failed, always runs. A call already inside the provider is never abandoned: it returns when
//    the provider returns, and apply.lock / engine.lock stay held until then (they belong to the caller).
//  - RemoteAddress values come ONLY from the plan (AllowedClients::tokens()), normalised by
//    FirewallCompare::writerValue; a token it cannot use fails the sync — never "Any".
//
// CimFirewallMonitor (the service's; runs only on the monitor thread created by BridgeRunner): threadInit
// connects its own session; checkOnce reads the ActiveStore group (rules + four associations each) and runs
// FirewallCompare::compare. Any CIM failure is Failed. It never logs and never touches the side channel.

#include "core/FirewallPlan.h"
#include "core/Interfaces.h"
#include "platform/win/CimSeams.h"
#include "platform/win/FirewallCompare.h"

namespace asb::win {

class CimFirewall final : public IFirewall {
public:
    CimFirewall(ICimSession& session, QString installedExe, IClock& clock = systemClock());

    void connect(Deadline) override;
    void disconnect() override;
    void sync(const BridgeConfig&, const FirewallGeneration&, Deadline) override;
    void rollback(const FirewallGeneration&, Deadline) override;
    void sweep(const FirewallGeneration& keep, Deadline) override;
    void removeAll(Deadline) override;

    // The reserved group as the comparator needs it (rules and their four associations), from `store`. Throws
    // CimError on any failure (never a partial group).
    static QList<FirewallCompare::RuleRead> readGroup(ICimSession&, cim::Store);
    // The InstanceIDs in the reserved group (PersistentStore).
    QStringList groupIds();

private:
    void requireConnected() const;
    void beforeNormalStep(Deadline, const char* what);
    void runSteps(const QList<FirewallPlan::Step>&, const FirewallPlan::Plan*, Deadline, bool cleanup);
    QString rulePath(const QString& instanceId);
    QString filterPath(const QString& ruleRelPath, const char* assocClass, const char* what);

    ICimSession& m_s;
    QString m_exe;
    IClock& m_clock;
    bool m_connected = false;
};

class CimFirewallMonitor final : public IFirewallMonitor {
public:
    CimFirewallMonitor(ICimSession& session, QString installedExe);
    bool enabled() const override { return true; }
    void threadInit() override;
    Check checkOnce(const BridgeConfig&, const FirewallGeneration&) override;
    // The last verdict's reason (the window's check, the service's exit-7 line, tests, the qualification harness).
    QString lastWhy() const override { return m_lastWhy; }

private:
    ICimSession& m_s;
    QString m_exe;
    bool m_connected = false;
    QString m_lastWhy;
};

// The reserved group's WQL (RuleGroup is the CIM property behind PowerShell's -Group).
QString groupQuery();
// The association classes, in the order the comparator reads them.
inline constexpr char kAssocPort[] = "MSFT_NetFirewallRuleFilterByProtocolPort";
inline constexpr char kAssocAddress[] = "MSFT_NetFirewallRuleFilterByAddress";
inline constexpr char kAssocApplication[] = "MSFT_NetFirewallRuleFilterByApplication";
inline constexpr char kAssocSecurity[] = "MSFT_NetFirewallRuleFilterBySecurity";

}  // namespace asb::win
