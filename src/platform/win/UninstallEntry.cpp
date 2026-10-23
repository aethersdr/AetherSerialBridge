#include "platform/win/UninstallEntry.h"

namespace asb::win {
namespace {

IRegistry::Value sz(const char* name, const QString& v)
{
    IRegistry::Value x;
    x.type = IRegistry::Value::Type::Sz;
    x.name = QString::fromLatin1(name);
    x.sz = v;
    return x;
}

IRegistry::Value dword(const char* name, quint32 v)
{
    IRegistry::Value x;
    x.type = IRegistry::Value::Type::Dword;
    x.name = QString::fromLatin1(name);
    x.dword = v;
    return x;
}

constexpr quint32 kFileNotFound = 2;

}  // namespace

QString win32ErrorText(quint32 code)
{
    switch (code) {
    case 2: return QStringLiteral("not found (2)");
    case 5: return QStringLiteral("access denied (5)");
    default: return QStringLiteral("Windows error %1").arg(code);
    }
}

QList<IRegistry::Value> UninstallEntry::values(const Facts& f)
{
    const QString exe = QStringLiteral("\"%1\"").arg(f.installedExe);
    const qint64 kib = (f.exeBytes + 1023) / 1024;
    return {
        sz("DisplayName", QStringLiteral("Aether Serial Bridge")),
        sz("DisplayVersion", f.version),
        sz("Publisher", QStringLiteral("AetherSDR")),
        sz("InstallLocation", f.installLocation),
        sz("DisplayIcon", exe + QStringLiteral(",0")),
        sz("UninstallString", exe + QLatin1Char(' ') + QString::fromLatin1(kAppsArgument)),
        dword("NoModify", 1),
        dword("NoRepair", 1),
        dword("EstimatedSize", quint32(qBound<qint64>(0, kib, 0xFFFFFFFFLL))),
        sz("InstallDate", f.date.toString(QStringLiteral("yyyyMMdd"))),
    };
}

QString UninstallEntry::write(const Facts& f)
{
    const quint32 e = m_r.writeKey(QString::fromLatin1(kKey), values(f));
    return e == 0 ? QString() : win32ErrorText(e);
}

QString UninstallEntry::remove()
{
    const quint32 e = m_r.deleteKey(QString::fromLatin1(kKey));
    return e == 0 || e == kFileNotFound ? QString() : win32ErrorText(e);
}

}  // namespace asb::win
