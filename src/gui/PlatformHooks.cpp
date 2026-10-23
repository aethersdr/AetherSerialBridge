#include "gui/PlatformHooks.h"

#include <QFile>
#include <cstdio>
#include <chrono>
#include <iterator>
#include <QSet>
#include <QString>

#include <QCoreApplication>
#include <QMetaObject>

#ifdef _WIN32
#include "core/ServicePlan.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/ScmSeams.h"
#include "platform/win/WbemSession.h"
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <vector>
#else
#include "platform/posix/QtSystemdBus.h"
#include "platform/posix/SerialPortList.h"
#include "platform/posix/SystemdUnit.h"
#endif

namespace asb::gui {

#ifdef _WIN32

std::optional<bool> readAutoStart()
{
    win::ScmSeams& s = win::nativeScmSeams();
    win::ScmSeams::Handle h = 0;
    const quint32 err = s.openService(QString::fromLatin1(ServicePlan::kServiceName), win::scm::kQueryConfig, &h);
    if (err == win::scm::kErrorServiceDoesNotExist) return false;
    if (err != win::scm::kOk) return std::nullopt;
    win::ScmServiceConfig c;
    const quint32 q = s.queryConfig(h, &c);
    s.close(h);
    if (q != win::scm::kOk) return std::nullopt;
    return c.startType == win::scm::kAutoStart;
}

QStringList serialPorts()
{
    // The values of SERIALCOMM are the port names. Reading the registry opens no device.
    QStringList names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return names;
    for (DWORD i = 0;; ++i) {
        wchar_t valueName[256];
        DWORD valueNameLen = 256;
        wchar_t data[256];
        DWORD dataBytes = sizeof data;
        DWORD type = 0;
        const LONG rc = RegEnumValueW(key, i, valueName, &valueNameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataBytes);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS || type != REG_SZ) continue;
        const qsizetype chars = qsizetype(dataBytes / sizeof(wchar_t));
        QString name = QString::fromWCharArray(data, chars);
        while (name.endsWith(QChar(0))) name.chop(1);
        if (!name.trimmed().isEmpty()) names << name.trimmed();
    }
    RegCloseKey(key);
    return names;
}

FirewallChecks::Work firewallCheckWork(const QString& installedExe)
{
    // Runs on a FirewallChecks thread: its own WMI session (created, used and released there), every query bounded by
    // the check's deadline (WbemSession::setDeadline); a query past it is a Failed check, which says nothing.
    return [installedExe](const BridgeConfig& config, const FirewallGeneration& g, Deadline deadline) {
        win::WbemSession session;
        session.setDeadline(deadline);
        win::CimFirewallMonitor monitor(session, installedExe);
        monitor.threadInit();
        const IFirewallMonitor::Check check = monitor.checkOnce(config, g);
        return FirewallChecks::Verdict{check, monitor.lastWhy()};
    };
}

QStringList loadedModules()
{
    QStringList out;
    HANDLE self = GetCurrentProcess();
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!K32EnumProcessModulesEx(self, mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) return out;
    if (needed > mods.size() * sizeof(HMODULE)) {
        mods.resize(needed / sizeof(HMODULE));
        if (!K32EnumProcessModulesEx(self, mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) return out;
    }
    const size_t n = needed / sizeof(HMODULE);
    for (size_t i = 0; i < n; ++i) {
        wchar_t path[32768];
        const DWORD len = GetModuleFileNameW(mods[i], path, DWORD(std::size(path)));
        if (len > 0 && len < std::size(path)) out << QString::fromWCharArray(path, int(len));
    }
    return out;
}

#else

std::optional<bool> readAutoStart()
{
    // Read only over the system bus (no root needed). UnitFileState: "enabled" = starts at boot; "masked" (the
    // fail-closed state, D21) or anything else = not.
    try {
        posix::QtSystemdBus bus;
        const auto ms = std::chrono::milliseconds(2000);
        const QString path = bus.loadUnit(QString::fromLatin1(posix::SystemdUnit::kUnitName), ms);
        const QString load = bus.unitProperty(path, QStringLiteral("org.freedesktop.systemd1.Unit"), QStringLiteral("LoadState"), ms).toString();
        if (load == QLatin1String("not-found")) return false;
        const QString state =
            bus.unitProperty(path, QStringLiteral("org.freedesktop.systemd1.Unit"), QStringLiteral("UnitFileState"), ms).toString();
        return load != QLatin1String("masked") && state == QLatin1String("enabled");
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

QStringList serialPorts()
{
    QStringList names;
    for (const posix::SerialPortList::Entry& e : posix::SerialPortList::byId()) names << e.path;
    for (const QString& k : posix::SerialPortList::kernelNames())
        names << (k.startsWith(QLatin1Char('/')) ? k : QStringLiteral("/dev/") + k);
#ifdef ASB_TEST_SEAMS
    // Test builds (tst_GuiProcess::portListOpensNothing, INV-32): extra names to list, and a line saying what was
    // listed, so the test knows the window enumerated them.
    const QString extra = qEnvironmentVariable("ASB_TEST_EXTRA_PORTS");
    if (!extra.isEmpty()) {
        names << extra.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        std::fprintf(stderr, "ASB-TEST-PORTS listed: %s\n", names.join(QLatin1Char(';')).toLocal8Bit().constData());
        std::fflush(stderr);
    }
#endif
    names.removeDuplicates();
    return names;
}

FirewallChecks::Work firewallCheckWork(const QString&)
{
    return {};   // no managed firewall on Linux (D7)
}

QStringList loadedModules()
{
    // The mapped files of /proc/self/maps (pathname column), each once.
    QStringList out;
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly)) return out;
    QSet<QString> seen;
    const QList<QByteArray> lines = maps.readAll().split('\n');
    for (const QByteArray& line : lines) {
        const qsizetype slash = line.indexOf('/');
        if (slash < 0) continue;
        const QString path = QString::fromLocal8Bit(line.mid(slash)).trimmed();
        if (path.endsWith(QLatin1String("(deleted)")) || seen.contains(path)) continue;
        seen.insert(path);
        out << path;
    }
    return out;
}

#endif

}  // namespace asb::gui
