// The native ICimSession (WS-7, Windows only). A thin pass-through: every decision is in CimFirewall.cpp.

#include "platform/win/WbemSession.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_DCOM
#define _WIN32_DCOM
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace asb::win {

namespace {

std::wstring W(const QString& s) { return s.toStdWString(); }

// A BSTR owned for the duration of a call.
struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const QString& s) : b(SysAllocString(reinterpret_cast<const OLECHAR*>(s.utf16()))) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

QString errorText(HRESULT hr)
{
    ComPtr<IWbemStatusCodeText> t;
    QString s;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemStatusCodeText, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemStatusCodeText,
                                   reinterpret_cast<void**>(t.GetAddressOf())))) {
        BSTR b = nullptr;
        if (SUCCEEDED(t->GetErrorCodeText(hr, 0, 0, &b)) && b) {
            s = QString::fromWCharArray(b).trimmed();
            SysFreeString(b);
        }
    }
    return s;
}

[[noreturn]] void raise(const QString& call, HRESULT hr)
{
    const QString text = errorText(hr);
    throw cim::CimError(static_cast<quint32>(hr),
                        QStringLiteral("%1: %2").arg(call, text.isEmpty() ? QStringLiteral("WMI error") : text));
}
[[noreturn]] void raise(const char* call, HRESULT hr) { raise(QString::fromLatin1(call), hr); }

cim::Value fromVariant(const VARIANT& v)
{
    switch (v.vt) {
    case VT_BSTR: return cim::Value::string(QString::fromWCharArray(v.bstrVal, SysStringLen(v.bstrVal)));
    case VT_BOOL: return cim::Value::integer(v.boolVal ? 1 : 0);
    case VT_I1: return cim::Value::integer(v.cVal);
    case VT_UI1: return cim::Value::integer(v.bVal);
    case VT_I2: return cim::Value::integer(v.iVal);
    case VT_UI2: return cim::Value::integer(v.uiVal);
    case VT_I4: return cim::Value::integer(v.lVal);
    case VT_UI4: return cim::Value::integer(v.ulVal);
    case VT_INT: return cim::Value::integer(v.intVal);
    case VT_UINT: return cim::Value::integer(v.uintVal);
    default: break;
    }
    if ((v.vt & VT_ARRAY) && v.parray) {
        // Class audit (review round 1): a list is read whole or not at all. A failed element read used to drop the
        // element, and an unknown element type gave an empty list: both shrank a list the comparator judges (an
        // extra RemoteAddress could vanish). Now a failure throws, and an element type this reader does not know
        // gives null (unknown), never a shorter list.
        const VARTYPE elem = VARTYPE(v.vt & ~VT_ARRAY);
        if (elem != VT_BSTR && elem != VT_I4 && elem != VT_UI4 && elem != VT_I2 && elem != VT_UI2 && elem != VT_UI1)
            return cim::Value::null();
        QStringList out;
        LONG lo = 0, hi = -1;
        HRESULT hr = SafeArrayGetLBound(v.parray, 1, &lo);
        if (SUCCEEDED(hr)) hr = SafeArrayGetUBound(v.parray, 1, &hi);
        if (FAILED(hr)) raise("SafeArrayGetLBound/UBound", hr);
        for (LONG i = lo; i <= hi; ++i) {
            if (elem == VT_BSTR) {
                BSTR b = nullptr;
                hr = SafeArrayGetElement(v.parray, &i, &b);
                if (FAILED(hr)) raise("SafeArrayGetElement", hr);
                out << QString::fromWCharArray(b, SysStringLen(b));
                SysFreeString(b);
            } else if (elem == VT_I4 || elem == VT_UI4) {
                LONG x = 0;
                hr = SafeArrayGetElement(v.parray, &i, &x);
                if (FAILED(hr)) raise("SafeArrayGetElement", hr);
                out << QString::number(x);
            } else if (elem == VT_I2 || elem == VT_UI2) {
                SHORT x = 0;
                hr = SafeArrayGetElement(v.parray, &i, &x);
                if (FAILED(hr)) raise("SafeArrayGetElement", hr);
                out << QString::number(x);
            } else {
                BYTE x = 0;
                hr = SafeArrayGetElement(v.parray, &i, &x);
                if (FAILED(hr)) raise("SafeArrayGetElement", hr);
                out << QString::number(x);
            }
        }
        return cim::Value::strings(out);
    }
    return cim::Value::null();
}

// Fills `out` (VariantInit'ed by the caller); the caller VariantClear's it.
void toVariant(const cim::Value& v, VARIANT* out)
{
    switch (v.kind) {
    case cim::Value::Kind::Null: out->vt = VT_NULL; return;
    case cim::Value::Kind::Int: out->vt = VT_I4; out->lVal = static_cast<LONG>(v.i); return;
    case cim::Value::Kind::String:
        out->vt = VT_BSTR;
        out->bstrVal = SysAllocString(reinterpret_cast<const OLECHAR*>(v.s.utf16()));
        return;
    case cim::Value::Kind::Strings: {
        SAFEARRAY* sa = SafeArrayCreateVector(VT_BSTR, 0, static_cast<ULONG>(v.list.size()));
        for (LONG i = 0; i < static_cast<LONG>(v.list.size()); ++i) {
            BSTR b = SysAllocString(reinterpret_cast<const OLECHAR*>(v.list[i].utf16()));
            SafeArrayPutElement(sa, &i, b);   // copies
            SysFreeString(b);
        }
        out->vt = VT_ARRAY | VT_BSTR;
        out->parray = sa;
        return;
    }
    }
}

std::atomic<bool> g_securitySet{false};
std::mutex g_securityMutex;

}  // namespace

bool initialiseComSecurity(QString* error)
{
    std::lock_guard g(g_securityMutex);
    if (g_securitySet.load()) return true;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        if (error) *error = QStringLiteral("CoInitializeEx failed (0x%1)").arg(quint32(init), 8, 16, QLatin1Char('0'));
        return false;
    }
    const HRESULT hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr)) {
        if (error) *error = QStringLiteral("CoInitializeSecurity failed (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    g_securitySet.store(true);
    return true;
}

struct WbemSession::Impl {
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> services;
    ComPtr<IWbemContext> contexts[3];
    bool comInitialised = false;
    std::optional<std::chrono::steady_clock::time_point> deadline;

    // The wait for the next result: the time left before the deadline, or WBEM_INFINITE without one.
    long nextTimeout() const
    {
        if (!deadline) return static_cast<long>(WBEM_INFINITE);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
            throw cim::CimError(static_cast<quint32>(WBEM_S_TIMEDOUT), QStringLiteral("the check's deadline passed"));
        return left.count() > 0x7fffffffLL ? 0x7fffffffL : static_cast<long>(left.count());
    }
    std::function<void(const QString&)> beforePutInstance;

    IWbemContext* context(cim::Store store)
    {
        ComPtr<IWbemContext>& c = contexts[static_cast<int>(store)];
        if (c) return c.Get();
        HRESULT hr = CoCreateInstance(CLSID_WbemContext, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemContext,
                                      reinterpret_cast<void**>(c.GetAddressOf()));
        if (FAILED(hr)) raise("CoCreateInstance(WbemContext)", hr);
        VARIANT v;
        VariantInit(&v);
        toVariant(cim::Value::string(cim::storeName(store)), &v);
        hr = c->SetValue(L"PolicyStore", 0, &v);
        VariantClear(&v);
        if (FAILED(hr)) raise("IWbemContext::SetValue(PolicyStore)", hr);
        if (store == cim::Store::Active) {
            VARIANT b;
            VariantInit(&b);
            b.vt = VT_BOOL;
            b.boolVal = VARIANT_TRUE;
            hr = c->SetValue(L"TracePolicyStoreSource", 0, &b);
            if (FAILED(hr)) raise("IWbemContext::SetValue(TracePolicyStoreSource)", hr);
        }
        return c.Get();
    }

    IWbemServices* svc()
    {
        if (!services) throw cim::CimError(0x80041003, QStringLiteral("the firewall provider is not connected"));
        return services.Get();
    }

    static cim::Instance read(IWbemClassObject* o)
    {
        cim::Instance inst;
        // Class audit (review round 1): an instance is read whole or the read fails; a property that could not be
        // read is never just left out (a missing Enabled or RemoteAddress must not read as absent).
        VARIANT p;
        VariantInit(&p);
        HRESULT hr = o->Get(L"__RELPATH", 0, &p, nullptr, nullptr);
        if (FAILED(hr)) raise("IWbemClassObject::Get(__RELPATH)", hr);
        if (p.vt == VT_BSTR) inst.relPath = QString::fromWCharArray(p.bstrVal, SysStringLen(p.bstrVal));
        VariantClear(&p);
        SAFEARRAY* names = nullptr;
        hr = o->GetNames(nullptr, WBEM_FLAG_ALWAYS | WBEM_FLAG_NONSYSTEM_ONLY, nullptr, &names);
        if (FAILED(hr)) raise(QStringLiteral("IWbemClassObject::GetNames of %1").arg(inst.relPath), hr);
        struct Destroy {
            SAFEARRAY* a;
            ~Destroy() { SafeArrayDestroy(a); }
        } destroyNames{names};
        LONG lo = 0, hi = -1;
        hr = SafeArrayGetLBound(names, 1, &lo);
        if (SUCCEEDED(hr)) hr = SafeArrayGetUBound(names, 1, &hi);
        if (FAILED(hr)) raise(QStringLiteral("SafeArrayGetLBound/UBound (the property names of %1)").arg(inst.relPath), hr);
        for (LONG i = lo; i <= hi; ++i) {
            BSTR n = nullptr;
            hr = SafeArrayGetElement(names, &i, &n);
            if (FAILED(hr)) raise(QStringLiteral("SafeArrayGetElement (a property name of %1)").arg(inst.relPath), hr);
            const QString name = QString::fromWCharArray(n, SysStringLen(n));
            SysFreeString(n);
            VARIANT v;
            VariantInit(&v);
            const std::wstring wn = name.toStdWString();
            hr = o->Get(wn.c_str(), 0, &v, nullptr, nullptr);
            // The property and the object are named, so a provider that refuses one property is diagnosed from
            // the first failed check.
            if (FAILED(hr)) {
                VariantClear(&v);
                raise(QStringLiteral("IWbemClassObject::Get(%1) of %2").arg(name, inst.relPath), hr);
            }
            cim::Value value;
            try {
                value = fromVariant(v);
            } catch (const cim::CimError& e) {
                VariantClear(&v);
                throw cim::CimError(e.hr, QStringLiteral("%1 (property %2 of %3)").arg(QString::fromLocal8Bit(e.what()), name, inst.relPath));
            }
            VariantClear(&v);
            inst.props.insert(name, value);
        }
        return inst;
    }
};

WbemSession::WbemSession() : d(std::make_unique<Impl>()) {}

WbemSession::~WbemSession()
{
    d->services.Reset();
    d->locator.Reset();
    for (auto& c : d->contexts) c.Reset();
    // No CoUninitialize: the session may be destroyed on another thread than the one that connected it (tests);
    // the apartment stays initialised for the life of the thread, which is harmless.
}

void WbemSession::connect()
{
    if (!d->comInitialised) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) raise("CoInitializeEx", init);
        d->comInitialised = SUCCEEDED(init);
    }
    d->services.Reset();
    d->locator.Reset();
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                  reinterpret_cast<void**>(d->locator.GetAddressOf()));
    if (FAILED(hr)) raise("CoCreateInstance(WbemLocator)", hr);
    // Bounded (v1.1 INV-129): WBEM_FLAG_CONNECT_USE_MAX_WAIT returns within 2 minutes; the deadline (setDeadline) still
    // bounds every query after it, and the service monitor's 35 s watchdog still fires first.
    static_assert(cim::kWbemConnectUseMaxWait == WBEM_FLAG_CONNECT_USE_MAX_WAIT);
    const cim::ConnectServerCall call = cim::connectServerCall();
    Bstr ns(call.ns);
    hr = d->locator->ConnectServer(ns.b, nullptr, nullptr, nullptr, call.flags, nullptr, nullptr, d->services.GetAddressOf());
    if (FAILED(hr)) raise("IWbemLocator::ConnectServer", hr);
    hr = CoSetProxyBlanket(d->services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) raise("CoSetProxyBlanket", hr);
}

QList<cim::Instance> WbemSession::query(const QString& wql, cim::Store store)
{
    if (d->deadline) (void)d->nextTimeout();   // no query starts past the deadline
    IWbemContext* ctx = d->context(store);
    Bstr lang(QStringLiteral("WQL"));
    Bstr text(wql);
    ComPtr<IEnumWbemClassObject> e;
    HRESULT hr = d->svc()->ExecQuery(lang.b, text.b, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, ctx,
                                     e.GetAddressOf());
    if (FAILED(hr)) raise("IWbemServices::ExecQuery", hr);
    QList<cim::Instance> out;
    for (;;) {
        ComPtr<IWbemClassObject> o;
        ULONG n = 0;
        hr = e->Next(d->nextTimeout(), 1, o.GetAddressOf(), &n);
        if (hr == WBEM_S_FALSE && n == 0) break;   // the end of the enumeration: the only success
        if (FAILED(hr)) raise("IEnumWbemClassObject::Next", hr);
        if (hr == WBEM_S_TIMEDOUT) raise("IEnumWbemClassObject::Next", hr);
        if (n == 1 && o) out << Impl::read(o.Get());
        if (hr == WBEM_S_FALSE) break;
    }
    return out;
}

void WbemSession::put(const QString& className, const QString& relPath, const cim::Props& props, cim::PutFlag flag,
                      cim::Store store, const std::function<void()>& beforeWrite)
{
    IWbemContext* ctx = d->context(store);
    ComPtr<IWbemClassObject> obj;
    if (relPath.isEmpty()) {
        ComPtr<IWbemClassObject> cls;
        Bstr name(className);
        HRESULT hr = d->svc()->GetObject(name.b, 0, ctx, cls.GetAddressOf(), nullptr);
        if (FAILED(hr)) raise("IWbemServices::GetObject(class)", hr);
        hr = cls->SpawnInstance(0, obj.GetAddressOf());
        if (FAILED(hr)) raise("IWbemClassObject::SpawnInstance", hr);
    } else {
        Bstr path(relPath);
        const HRESULT hr = d->svc()->GetObject(path.b, 0, ctx, obj.GetAddressOf(), nullptr);
        if (FAILED(hr)) raise("IWbemServices::GetObject", hr);
    }
    for (auto it = props.cbegin(); it != props.cend(); ++it) {
        VARIANT v;
        VariantInit(&v);
        toVariant(*it, &v);
        const std::wstring n = W(it.key());
        const HRESULT hr = obj->Put(n.c_str(), 0, &v, 0);
        VariantClear(&v);
        if (FAILED(hr)) raise("IWbemClassObject::Put", hr);
    }
    if (!relPath.isEmpty() && d->beforePutInstance) d->beforePutInstance(relPath);
    // The lookup above may have blocked past the caller's deadline: the caller decides before anything is written.
    if (beforeWrite) beforeWrite();
    const HRESULT hr = d->svc()->PutInstance(obj.Get(), static_cast<long>(flag), ctx, nullptr);
    if (FAILED(hr)) raise("IWbemServices::PutInstance", hr);
}

void WbemSession::setDeadline(std::chrono::steady_clock::time_point deadline) { d->deadline = deadline; }

void WbemSession::disconnect()
{
    d->services.Reset();
    d->locator.Reset();
    for (auto& c : d->contexts) c.Reset();
}

void WbemSession::setBeforePutInstance(std::function<void(const QString&)> f) { d->beforePutInstance = std::move(f); }

void WbemSession::remove(const QString& relPath, cim::Store store)
{
    IWbemContext* ctx = d->context(store);
    Bstr path(relPath);
    const HRESULT hr = d->svc()->DeleteInstance(path.b, 0, ctx, nullptr);
    if (FAILED(hr)) raise("IWbemServices::DeleteInstance", hr);
}

}  // namespace asb::win
