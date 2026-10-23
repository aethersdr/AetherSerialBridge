#pragma once

// An in-memory WMI firewall provider behind ICimSession (WS-7 tests), modelled on what the WS-7 probe measured
// on windows-latest (CimSeams.h, FirewallCompare.h):
//  - a rule created by PutInstance(MSFT_NetFirewallRule, CREATE_ONLY) gets one filter of each kind, with the
//    provider's defaults (Protocol "Any", everything else null, OverrideBlockRules false);
//  - CREATE_ONLY on an existing InstanceID fails WBEM_E_ALREADY_EXISTS; UPDATE_ONLY on a deleted object fails
//    WBEM_E_NOT_FOUND; CREATE_OR_UPDATE recreates (the INV-114 flag mutant's observable difference);
//  - the PersistentStore holds the local rules; the ActiveStore is the persistent rules plus `gpoRules` (other
//    stores), each reported with its PolicyStoreSourceType;
//  - every call is logged ("connect", "query <store> <wql>", "put <store> <flag> <class|relPath> k=v...",
//    "remove <store> <relPath>") and passes through `hook` first, which may block, advance a clock or throw.

#include "platform/win/CimSeams.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>
#include <mutex>

namespace asb::test {

class FakeCim final : public asb::win::ICimSession {
public:
    struct Rule {
        win::cim::Props rule, port, address, application, security;
    };

    void connect() override;
    QList<win::cim::Instance> query(const QString& wql, win::cim::Store) override;
    // Logs "get <store> <class|relPath>" (the lookup; `hook` may make it return late), then calls `beforeWrite`,
    // then logs the "put ..." line and applies it.
    void put(const QString& className, const QString& relPath, const win::cim::Props& props, win::cim::PutFlag,
             win::cim::Store, const std::function<void()>& beforeWrite = {}) override;
    void remove(const QString& relPath, win::cim::Store) override;

    // Called before every operation with its log line; may block, throw or change state.
    std::function<void(const QString& call)> hook;

    // State (guarded by `m`; tests may read it directly between calls).
    mutable std::recursive_mutex m;
    QMap<QString, Rule> persistent;   // by InstanceID
    QMap<QString, Rule> gpoRules;     // rules from another store (visible in the ActiveStore only)
    QStringList log;
    int connects = 0;

    // Test helpers.
    static Rule ruleWithDefaults(const QString& instanceId, const QString& group, qint64 enabled);
    // A complete, enabled rule as CimFirewall writes it for the plan rule.
    static Rule asWritten(const QString& instanceId, const QString& group, const QString& port,
                          const QStringList& remote, const QString& appPath);
    QStringList ids() const;
    QStringList calls(const QString& prefix) const;

private:
    static QString relPathOf(const QString& cls, const QString& id);
    Rule* findByRelPath(const QString& relPath, QString* kind, win::cim::Store);
    void call(const QString& line);
};

}  // namespace asb::test
