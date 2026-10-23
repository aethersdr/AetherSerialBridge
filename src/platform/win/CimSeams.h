#pragma once

// The CIM seam of the Windows firewall adapter (WS-7; D51, D53). Every WMI call CimFirewall and
// CimFirewallMonitor make goes through ICimSession, so the recipe, the flags, the order and the exclusion rules
// are asserted on a recording fake on every OS (tests/support/FakeCim). The native implementation
// (WbemSession.cpp, Windows only) is a thin pass-through to IWbemLocator / IWbemServices / IEnumWbemClassObject:
//
//  connect   on the calling thread: CoInitializeEx(COINIT_MULTITHREADED) (the process-wide
//            CoInitializeSecurity is made once at mode entry, comSecurity() below), CoCreateInstance(WbemLocator),
//            ConnectServer(L"ROOT\\StandardCimv2", WBEM_FLAG_CONNECT_USE_MAX_WAIT: connectServerCall()), CoSetProxyBlanket(RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
//            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE). The interfaces live on that thread.
//  query     ExecQuery(WQL, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY) with an IWbemContext whose
//            PolicyStore is the store's name (ActiveStore also sets TracePolicyStoreSource), then
//            IEnumWbemClassObject::Next until WBEM_S_FALSE. Any HRESULT failure, WBEM_S_TIMEDOUT or an
//            enumeration that does not reach WBEM_S_FALSE throws: a caller never sees a partial result.
//  put       a new instance (SpawnInstance of the class) or the existing object (GetObject(relPath)), the
//            properties set as typed values, then PutInstance(flags) — synchronous.
//  remove    DeleteInstance(relPath) — synchronous.
// Every call is synchronous on the calling thread: there is no *Async call, sink or CancelAsyncCall (INV-114).

#include "core/Interfaces.h"
#include "platform/win/FirewallCompare.h"

#include <QList>
#include <QString>

#include <functional>

namespace asb::win {

namespace cim {

// WBEM_FLAG_* values passed to PutInstance.
enum class PutFlag : quint32 { CreateOnly = 0x2, UpdateOnly = 0x1, CreateOrUpdate = 0x0 };

// The PolicyStore context of a call.
enum class Store { Persistent, Active, LocalGpo };
QString storeName(Store);   // "PersistentStore", "ActiveStore", "localhost"

using Value = FirewallCompare::Value;
using Props = FirewallCompare::Props;

struct Instance {
    QString relPath;   // __RELPATH
    Props props;
};

// A failed WMI call: `hr` is the HRESULT, what() the provider's error text.
class CimError : public PlatformError {
public:
    CimError(quint32 hr, const QString& text);
    quint32 hr = 0;
};

inline constexpr quint32 kWbemNotFound = 0x80041002;       // WBEM_E_NOT_FOUND
inline constexpr quint32 kWbemAlreadyExists = 0x80041019;  // WBEM_E_ALREADY_EXISTS
inline constexpr quint32 kWbemTimedOut = 0x00040004;       // WBEM_S_TIMEDOUT
inline constexpr qint32 kWbemConnectUseMaxWait = 0x80;     // WBEM_FLAG_CONNECT_USE_MAX_WAIT

// IWbemLocator::ConnectServer's namespace and lFlags (v1.1 INV-129): WBEM_FLAG_CONNECT_USE_MAX_WAIT, which Windows
// documents as returning within 2 minutes, so no connect (the Applier worker's, the service monitor's, the window's own
// check) can wait for ever. WbemSession::connect passes exactly these.
struct ConnectServerCall {
    QString ns;
    qint32 flags = 0;
};
ConnectServerCall connectServerCall();

}  // namespace cim

class ICimSession {
public:
    virtual ~ICimSession() = default;
    virtual void connect() = 0;
    virtual QList<cim::Instance> query(const QString& wql, cim::Store) = 0;
    // `className` set and `relPath` empty: a new instance of the class; otherwise the existing object at relPath.
    // `beforeWrite`, when set, is called after the object was looked up (GetObject; SpawnInstance for a new one)
    // and before the provider is changed (PutInstance): it throws to stop a write whose lookup returned after the
    // deadline (INV-114, review round 1). Nothing is written when it throws.
    virtual void put(const QString& className, const QString& relPath, const cim::Props& props, cim::PutFlag,
                     cim::Store, const std::function<void()>& beforeWrite = {}) = 0;
    virtual void remove(const QString& relPath, cim::Store) = 0;
    // Releases the connection (ConnectServer's proxies, the contexts); connect() makes a new one. On the thread that
    // connected.
    virtual void disconnect() {}
};

}  // namespace asb::win
