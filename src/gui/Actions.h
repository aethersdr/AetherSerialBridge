#pragma once

// What the window can ask for (WS-10 §5.5): every mutating action, behind one busy gate (D57, INV-118).
//
//  - InProcessActions: the Applier on an OperationHost worker in this process (the elevated Windows GUI; tests on
//    either OS over the WS-5 fakes). The worker's Platform is its own, never the window's view Platform.
//  - ReadOnlyActions: nothing can be changed (a standard user on Windows). Every submit returns false; `why` says how
//    to get the actions.
//  - HelperActions (HelperActions.h, Linux): every operation through WS-9b's HelperClient over PkexecClient (the pkexec
//    helper runs the Applier as root).
//
// The untick route is a runtime flavour, not an #ifdef: Windows SaveAndRemove(N), Linux Deactivate(N) (D38), so
// one test host drives both.

#include "core/Applier.h"
#include "core/Interfaces.h"
#include "core/OperationHost.h"

#include <QString>

#include <functional>
#include <memory>

namespace asb::gui {

class IActions {
public:
    virtual ~IActions() = default;
    virtual bool available() const = 0;
    virtual QString unavailableReason() const { return {}; }
    virtual Installation::Flavor flavor() const = 0;
    // True from a submit until its `finished` was delivered on the window's thread.
    virtual bool busy() const = 0;

    // Save & apply with the box ticked (Run; `install` when the service is not Current).
    virtual bool run(const SaveRequest&, bool install, IOperationObserver&) = 0;
    // Save & apply with the box unticked: the flavour's untick operation.
    virtual bool untick(const SaveRequest&, IOperationObserver&) = 0;
    virtual bool start(IOperationObserver&) = 0;
    virtual bool stop(IOperationObserver&) = 0;
    virtual bool repair(IOperationObserver&) = 0;
    // Discard saved changes: `sha256` is the digest of the pending bytes the window showed (INV-127); the core deletes
    // config.pending.json only while it still holds exactly those bytes.
    virtual bool discardPending(const QString& sha256, IOperationObserver&) = 0;
    // While busy: what the operation is doing now (its phase, D57), for the footer; empty when unknown. Never a result.
    virtual QString progress() const { return {}; }

    // The lifecycle (v1.1 WS-17 §1). `install`: Install the bridge service (and Finish installing), with the editor's
    // ports when given; `update`: Update to this build (the same operation; its own name for the wording and the native
    // log); `uninstall`: Uninstall… (WS-16's core). Each goes through the one busy gate; the window applies INV-107's
    // dirty rule to the result as it does for Save & apply.
    virtual bool install(const std::optional<SaveRequest>&, IOperationObserver&) = 0;
    virtual bool update(const std::optional<SaveRequest>&, IOperationObserver&) = 0;
    virtual bool uninstall(UninstallOptions, IOperationObserver&) = 0;
    // Why install and update cannot run from this window now, before any click (WS-15 hand-over: the Linux window's own
    // program, WS-14's windowProgram(), is missing or not the build the window was made with); empty when they can.
    // Uninstall never needs the window's program on Windows; on Linux the staged route does.
    virtual QString lifecycleUnavailableReason() const { return {}; }
    // Called on the window's thread when lifecycleUnavailableReason() may have changed (the Linux probe ended).
    virtual void setLifecycleListener(std::function<void()>) {}

    // Save only, with nothing installed (v1.1 fix after WS-17): the editor's settings become the saved, not applied
    // settings (config.pending.json) and nothing else changes: no service, no firewall rule. The window's main button
    // reads "Save" for it. Only where savesWithoutService(): the in-process Applier writes the data folder itself
    // (Applier::saveOnly, which refuses under apply.lock when a service was installed since the window read it, so the
    // click never stops one). The Linux window has no route for it: with nothing installed there is no
    // installed helper to run, and the staged route runs only install and uninstall (INV-123).
    virtual bool savesWithoutService() const { return false; }
    virtual bool saveOnly(const SaveRequest&, IOperationObserver&) { return false; }
};

class InProcessActions final : public IActions {
public:
    // `host` lives at least as long as this object (the window's process owns both).
    InProcessActions(OperationHost& host, Installation::Flavor flavor) : m_host(host), m_flavor(flavor) {}
    bool available() const override { return true; }
    Installation::Flavor flavor() const override { return m_flavor; }
    bool busy() const override { return m_host.busy(); }
    bool run(const SaveRequest& r, bool install, IOperationObserver& o) override { return m_host.submitRun(r, install, false, o); }
    bool untick(const SaveRequest& r, IOperationObserver& o) override
    {
        return m_flavor == Installation::Flavor::Windows ? m_host.submitSaveAndRemove(r, o) : m_host.submitDeactivate(r, o);
    }
    // The window's Start runs a complete installation of another build as it is (Installation::otherBuild): a Start
    // never replaces the program, and Save & apply asks before it does.
    bool start(IOperationObserver& o) override { return m_host.submitStart(o, /*acceptOtherBuild=*/true); }
    bool stop(IOperationObserver& o) override { return m_host.submitStop(o); }
    bool repair(IOperationObserver& o) override { return m_host.submitRepair(o); }
    bool discardPending(const QString& sha256, IOperationObserver& o) override
    {
        return m_host.submitDiscardPendingIf(sha256, o);
    }
    bool install(const std::optional<SaveRequest>& r, IOperationObserver& o) override
    {
        return m_host.submitRun(r, /*install=*/true, false, o);
    }
    bool update(const std::optional<SaveRequest>& r, IOperationObserver& o) override
    {
        return m_host.submitRun(r, /*install=*/true, false, o);
    }
    bool uninstall(UninstallOptions options, IOperationObserver& o) override { return m_host.submitUninstall(options, o); }
    bool savesWithoutService() const override { return true; }
    bool saveOnly(const SaveRequest& r, IOperationObserver& o) override { return m_host.submitSaveOnly(r, o); }
    QString progress() const override
    {
        if (!m_host.busy() || m_host.workerReturned()) return {};
        const PhaseSnapshot s = m_host.phases().load();
        return s.operationId == m_host.currentOperationId() && s.phase != Phase::None ? phaseName(s.phase) : QString();
    }
    OperationHost& host() { return m_host; }

private:
    OperationHost& m_host;
    Installation::Flavor m_flavor;
};

class ReadOnlyActions final : public IActions {
public:
    ReadOnlyActions(Installation::Flavor flavor, QString why) : m_flavor(flavor), m_why(std::move(why)) {}
    bool available() const override { return false; }
    QString unavailableReason() const override { return m_why; }
    Installation::Flavor flavor() const override { return m_flavor; }
    bool busy() const override { return false; }
    bool run(const SaveRequest&, bool, IOperationObserver&) override { return false; }
    bool untick(const SaveRequest&, IOperationObserver&) override { return false; }
    bool start(IOperationObserver&) override { return false; }
    bool stop(IOperationObserver&) override { return false; }
    bool repair(IOperationObserver&) override { return false; }
    bool discardPending(const QString&, IOperationObserver&) override { return false; }
    bool install(const std::optional<SaveRequest>&, IOperationObserver&) override { return false; }
    bool update(const std::optional<SaveRequest>&, IOperationObserver&) override { return false; }
    bool uninstall(UninstallOptions, IOperationObserver&) override { return false; }
    QString lifecycleUnavailableReason() const override { return m_why; }

private:
    Installation::Flavor m_flavor;
    QString m_why;
};

}  // namespace asb::gui
