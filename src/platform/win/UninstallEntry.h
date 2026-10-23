#pragma once

// The Apps & features entry (WS-16; D63, INV-126): the key UninstallEntry::kKey under HKLM, in the 64-bit registry
// view, naming the installed console exe with --uninstall-from-apps.
//
//  write   creates or opens the key and sets exactly the values of values(), then RegFlushKey. WindowsService calls it
//          after R3 installed (or verified) this build's program; an update rewrites DisplayVersion and the rest.
//  remove  deletes the key; "not there" is not an error. Uninstall calls it last, only once the program it names is gone.
//
// Never an input to the installation classifier (InstallationEvidence): a v1.0 install has no entry and is Current.
// Portable logic over IRegistry, so the values are tested on every OS; the native pass-through is in WindowsPlatform.cpp.

#include <QDate>
#include <QList>
#include <QString>

namespace asb::win {

// HKEY_LOCAL_MACHINE in the 64-bit view (KEY_WOW64_64KEY). Win32 error codes; 0 = success.
class IRegistry {
public:
    struct Value {
        enum class Type { Sz, Dword } type = Type::Sz;
        QString name;
        QString sz;
        quint32 dword = 0;
        bool operator==(const Value&) const = default;
    };
    virtual ~IRegistry() = default;
    // RegCreateKeyExW + RegSetValueExW for each value + RegFlushKey.
    virtual quint32 writeKey(const QString& subkey, const QList<Value>& values) = 0;
    // RegDeleteKeyExW; ERROR_FILE_NOT_FOUND (2) when the key is not there.
    virtual quint32 deleteKey(const QString& subkey) = 0;
    // RegOpenKeyExW(KEY_QUERY_VALUE) succeeded.
    virtual bool keyExists(const QString& subkey) = 0;
};

class IUninstallEntry {
public:
    struct Facts {
        QString installLocation;   // the resolved %ProgramFiles%\AetherSerialBridge
        QString installedExe;      // <installLocation>\aether-serial-bridge.exe
        QString version;           // this build's core::version()
        qint64 exeBytes = 0;       // the installed exe's size
        QDate date;                // local date of the write
    };
    virtual ~IUninstallEntry() = default;
    // Empty when written; else why not (the install is not failed by it).
    virtual QString write(const Facts&) = 0;
    // Empty when removed or not there; else why not.
    virtual QString remove() = 0;
    virtual bool present() = 0;
};

class UninstallEntry final : public IUninstallEntry {
public:
    static constexpr char kKey[] = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AetherSerialBridge";
    static constexpr char kAppsArgument[] = "--uninstall-from-apps";

    explicit UninstallEntry(IRegistry& r) : m_r(r) {}
    QString write(const Facts&) override;
    QString remove() override;
    bool present() override { return m_r.keyExists(QString::fromLatin1(kKey)); }

    // The values, in the order they are written (pure).
    static QList<IRegistry::Value> values(const Facts&);

private:
    IRegistry& m_r;
};

QString win32ErrorText(quint32 code);

}  // namespace asb::win
