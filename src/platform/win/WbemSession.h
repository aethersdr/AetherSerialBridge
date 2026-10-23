#pragma once

// The native ICimSession (WS-7; Windows only): IWbemLocator / IWbemServices / IEnumWbemClassObject, synchronous,
// on the thread that called connect() (CimSeams.h has the contract). One session per thread: the Applier's
// operation worker and the service's monitor thread each own one; a session is never shared between threads.

#include "platform/win/CimSeams.h"

#include <chrono>
#include <functional>
#include <memory>

namespace asb::win {

// Process COM security, once, at mode entry on the calling thread (master §4.6, WS-7 "COM setup"):
// CoInitializeEx(COINIT_MULTITHREADED) and CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
// RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr). A second call is a no-op.
// false (+ *error) when security could not be set (RPC_E_TOO_LATE: something initialised it first); the proxy
// blanket of every session still asks for impersonation, and a CIM failure fails closed either way.
bool initialiseComSecurity(QString* error);

class WbemSession final : public ICimSession {
public:
    WbemSession();
    ~WbemSession() override;   // releases the interfaces (on the owning thread: production never destroys one elsewhere)
    WbemSession(const WbemSession&) = delete;
    WbemSession& operator=(const WbemSession&) = delete;

    void connect() override;
    QList<cim::Instance> query(const QString& wql, cim::Store) override;
    void put(const QString& className, const QString& relPath, const cim::Props& props, cim::PutFlag, cim::Store,
             const std::function<void()>& beforeWrite = {}) override;
    void remove(const QString& relPath, cim::Store) override;
    void disconnect() override;
    // Every later query must finish by `deadline`: a query is not started after it, and each wait for the next result
    // (IEnumWbemClassObject::Next) is bounded by the time left; past it the query fails (WBEM_S_TIMEDOUT) and the
    // enumerator is released. ConnectServer is bounded by WBEM_FLAG_CONNECT_USE_MAX_WAIT (about 2 minutes, v1.1 INV-129),
    // not by this deadline. The window's stale-rule check sets one; the
    // Applier's and the service's sessions do not (their callers bound them).
    void setDeadline(std::chrono::steady_clock::time_point deadline);

    // Test seam (native tests only; never set in production): called between GetObject and PutInstance of an
    // update, with the object's path, so a test can delete the object in that window (INV-114
    // update_only_cannot_resurrect_deleted_rule).
    void setBeforePutInstance(std::function<void(const QString& relPath)> f);

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};

}  // namespace asb::win
