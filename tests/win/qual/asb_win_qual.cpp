// asb_win_qual (WS-7, test builds only): the qualification harness the station script (qualification/
// ws7-station.ps1) drives on the station PC, until WS-9's CLI exists. It runs the real Applier over the real
// Windows Platform (WindowsPlatform: the objects WS-9 will build), so what it proves is what the product does.
// It creates no process. Every command prints one "RESULT ..." line; exit 0 = the command did what it says.
//
//   asb_win_qual [--exe <headless exe>] [--runtime <dir>] <command> [--config <file>]
//     detect          the installation state
//     install         Applier Run(install) with the config file's exact bytes
//     apply           Applier Run with the config file
//     start | stop    Applier Start / Stop
//     uninstall       Applier SaveAndRemove (firewall rules, then the service)
//     status          SCM state and status.json
//     list-ports      the serial ports as QSerialPortInfo lists them (HW 10: run under Process Monitor)
//     fw-check        the INV-117 comparator over the ActiveStore for the committed config and the token's generation
//     apply-fail      Applier Run with the config file whose firewall sync is made to fail (a test-only firewall
//                     decorator): the "failed apply" of HW 8 — the service is left disabled (fail closed)
//     remove          Applier SaveAndRemove with the config file saved as pending first (the Windows untick)
//     remove-fail     the same, with the firewall's removeAll made to fail (a test-only decorator): the
//                     activation_matrix point "SaveAndRemove whose removal fails" (INV-99, WS-13)
//     keyword-matrix  fw_keyword_matrix: one DISABLED rule per known keyword in a private test group; stored value read
//                     back; every rule deleted again
// --exe defaults to aether-serial-bridge.exe beside this tool; --runtime (a dynamic-Qt test build only) is a folder
// of DLLs copied beside the installed exe before install (default: <tool dir>\runtime when it exists).

#include "ServiceNativeSupport.h"
#include "WinTestNames.h"

#include "core/ConfigJson.h"
#include "core/FirewallKeywords.h"
#include "core/StartToken.h"
#include "core/StatusStore.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/WbemSession.h"
#include "platform/win/WindowsService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSerialPortInfo>

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace asb;
using namespace asb::test;

namespace {

void out(const QString& s)
{
    std::fputs(qPrintable(s + QLatin1Char('\n')), stdout);
    std::fflush(stdout);
}

int result(const Result& r, bool ok)
{
    out(QStringLiteral("RESULT %1 outcome=%2 persisted=%3 step=%4 message=%5 postcondition=%6 files=%7")
            .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"), enumName(r.outcome), enumName(r.persisted), r.step,
                 r.message, r.postcondition, r.fileChanges.join(QStringLiteral("; "))));
    return ok ? 0 : 1;
}

QString stateName(InstallState s)
{
    switch (s) {
    case InstallState::NotInstalled: return QStringLiteral("NotInstalled");
    case InstallState::IncompleteInstall: return QStringLiteral("IncompleteInstall");
    case InstallState::Current: return QStringLiteral("Current");
    case InstallState::Foreign: return QStringLiteral("Foreign");
    }
    return {};
}

// A firewall whose sync fails (HW 8 "failed apply"); everything else goes to the real one.
class FailingSync final : public IFirewall {
public:
    explicit FailingSync(IFirewall& real) : m_real(real) {}
    void connect(Deadline d) override { m_real.connect(d); }
    void sync(const BridgeConfig&, const FirewallGeneration&, Deadline) override
    {
        throw PlatformError("qualification: the firewall sync was made to fail");
    }
    void rollback(const FirewallGeneration& g, Deadline d) override { m_real.rollback(g, d); }
    void sweep(const FirewallGeneration& g, Deadline d) override { m_real.sweep(g, d); }
    void removeAll(Deadline d) override { m_real.removeAll(d); }

private:
    IFirewall& m_real;
};

// A firewall whose removeAll fails (the activation_matrix's SaveAndRemove point); everything else is the real one.
class FailingRemoveAll final : public IFirewall {
public:
    explicit FailingRemoveAll(IFirewall& real) : m_real(real) {}
    void connect(Deadline d) override { m_real.connect(d); }
    void sync(const BridgeConfig& c, const FirewallGeneration& g, Deadline d) override { m_real.sync(c, g, d); }
    void rollback(const FirewallGeneration& g, Deadline d) override { m_real.rollback(g, d); }
    void sweep(const FirewallGeneration& g, Deadline d) override { m_real.sweep(g, d); }
    void removeAll(Deadline) override { throw PlatformError("qualification: the firewall removal was made to fail"); }

private:
    IFirewall& m_real;
};

int keywordMatrix()
{
    win::WbemSession cim;
    cim.connect();
    const QString group = QString::fromLatin1(asb::test::names::kKeywordMatrixTestGroup);
    QStringList keywords;
    for (std::string_view k : kKnownFirewallKeywords) keywords << QString::fromLatin1(k.data(), qsizetype(k.size()));
    keywords << QStringLiteral("PlayToTargets");
    int mismatches = 0, n = 0;
    for (const QString& kw : keywords) {
        const QString id = QStringLiteral("AsbKeywordMatrix-%1").arg(n++);
        win::cim::Props r;
        r[QStringLiteral("InstanceID")] = win::cim::Value::string(id);
        r[QStringLiteral("ElementName")] = win::cim::Value::string(id);
        r[QStringLiteral("RuleGroup")] = win::cim::Value::string(group);
        r[QStringLiteral("Direction")] = win::cim::Value::integer(1);
        r[QStringLiteral("Action")] = win::cim::Value::integer(2);
        r[QStringLiteral("Enabled")] = win::cim::Value::integer(2);   // never enabled: no exposure
        QString stored = QStringLiteral("<refused>");
        bool accepted = false;
        try {
            cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, r, win::cim::PutFlag::CreateOnly, win::cim::Store::Persistent);
            const auto rule = cim.query(QStringLiteral("SELECT * FROM MSFT_NetFirewallRule WHERE InstanceID = '%1'").arg(id),
                                        win::cim::Store::Persistent);
            const auto filter = cim.query(QStringLiteral("ASSOCIATORS OF {%1} WHERE AssocClass = %2")
                                              .arg(rule.value(0).relPath, QString::fromLatin1(win::kAssocAddress)),
                                          win::cim::Store::Persistent);
            win::cim::Props a;
            a[QStringLiteral("RemoteAddress")] = win::cim::Value::strings({kw});
            try {
                cim.put({}, filter.value(0).relPath, a, win::cim::PutFlag::UpdateOnly, win::cim::Store::Persistent);
                const auto back = cim.query(QStringLiteral("ASSOCIATORS OF {%1} WHERE AssocClass = %2")
                                                .arg(rule.value(0).relPath, QString::fromLatin1(win::kAssocAddress)),
                                            win::cim::Store::Persistent);
                const win::cim::Value v = back.value(0).props.value(QStringLiteral("RemoteAddress"));
                stored = v.kind == win::cim::Value::Kind::Null ? QStringLiteral("<any>") : v.list.join(QLatin1Char(','));
                accepted = true;
            } catch (const std::exception& e) {
                stored = QStringLiteral("<refused: %1>").arg(QString::fromUtf8(e.what()));
            }
            cim.remove(rule.value(0).relPath, win::cim::Store::Persistent);
        } catch (const std::exception& e) {
            stored = QStringLiteral("<error: %1>").arg(QString::fromUtf8(e.what()));
        }
        bool pinned = false;
        for (std::string_view k : kAcceptedFirewallKeywords)
            if (kw == QString::fromLatin1(k.data(), qsizetype(k.size()))) pinned = true;
        const bool agrees = pinned == accepted;
        if (!agrees) ++mismatches;
        out(QStringLiteral("KEYWORD %1 provider=%2 pinned=%3 stored=%4 %5")
                .arg(kw, accepted ? QStringLiteral("accepted") : QStringLiteral("refused"),
                     pinned ? QStringLiteral("accepted") : QStringLiteral("refused"), stored,
                     agrees ? QStringLiteral("AGREES") : QStringLiteral("DIFFERS")));
    }
    out(QStringLiteral("RESULT %1 keyword-matrix mismatches=%2").arg(mismatches ? QStringLiteral("failed") : QStringLiteral("ok")).arg(mismatches));
    return mismatches ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString exe = QCoreApplication::applicationDirPath() + QStringLiteral("/aether-serial-bridge.exe");
    QString runtime = QCoreApplication::applicationDirPath() + QStringLiteral("/runtime");
    QString configPath;
    QString command;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--exe") && i + 1 < args.size()) exe = args[++i];
        else if (args[i] == QLatin1String("--runtime") && i + 1 < args.size()) runtime = args[++i];
        else if (args[i] == QLatin1String("--config") && i + 1 < args.size()) configPath = args[++i];
        else if (command.isEmpty()) command = args[i];
        else {
            out(QStringLiteral("RESULT failed usage: unexpected '%1'").arg(args[i]));
            return 2;
        }
    }
    svc::setHeadlessExe(QDir::toNativeSeparators(QFileInfo(exe).absoluteFilePath()));
    if (command == QLatin1String("list-ports")) {
        QStringList names;
        for (const QSerialPortInfo& i : QSerialPortInfo::availablePorts()) names << i.portName() + QStringLiteral(" (") + i.description() + QLatin1Char(')');
        for (const QString& n : names) out(QStringLiteral("PORT %1").arg(n));
        out(QStringLiteral("RESULT ok list-ports count=%1").arg(names.size()));
        return 0;
    }
    if (command == QLatin1String("keyword-matrix")) return keywordMatrix();

    QString err;
    auto p = svc::platform(&err);
    if (!p) {
        out(QStringLiteral("RESULT failed platform: %1").arg(err));
        return 1;
    }
    const auto later = [] { return std::chrono::steady_clock::now() + std::chrono::seconds(30); };
    auto config = [&]() -> QByteArray {
        QFile f(configPath);
        if (configPath.isEmpty() || !f.open(QIODevice::ReadOnly)) {
            out(QStringLiteral("RESULT failed --config <file> is required and readable"));
            std::exit(2);
        }
        return f.readAll();
    };
    if (command == QLatin1String("detect")) {
        try {
            const Installation i = p->service().detect(later());
            out(QStringLiteral("RESULT ok detect state=%1 detail=%2").arg(stateName(i.state), i.detail));
            return 0;
        } catch (const std::exception& e) {
            out(QStringLiteral("RESULT failed detect: %1").arg(QString::fromUtf8(e.what())));
            return 1;
        }
    }
    if (command == QLatin1String("install")) {
        const QByteArray bytes = config();
        // The dev runtime goes beside the installed exe only when the folder is (or will be) the Qt installation's:
        // never into a foreign installation's folder (the Applier refuses those anyway).
        InstallState state = InstallState::Foreign;
        try {
            state = p->service().detect(later()).state;
        } catch (const std::exception&) {
        }
        const bool ours = state == InstallState::NotInstalled || state == InstallState::IncompleteInstall ||
                          state == InstallState::Current;
        if (ours && QFileInfo(runtime).isDir()) {
            const QStringList copied = svc::deployDevRuntime(p->lockDirectory(), runtime);
            out(QStringLiteral("RUNTIME %1").arg(copied.join(QStringLiteral(", "))));
        }
        const Result r = svc::install(*p, bytes);
        return result(r, r.outcome == Outcome::Ok);
    }
    if (command == QLatin1String("apply")) {
        const Result r = svc::apply(*p, config());
        return result(r, r.outcome == Outcome::Ok);
    }
    if (command == QLatin1String("apply-fail")) {
        Platform& real = p->platform();
        FailingSync failing(real.firewall);
        Platform pf{real.folder, real.service, failing, real.applyLock, real.engineLock, real.token, real.status, real.io};
        const Result r = Applier(pf).run(svc::request(config()), false);
        return result(r, r.outcome == Outcome::Uncommitted);
    }
    if (command == QLatin1String("remove") || command == QLatin1String("remove-fail")) {
        Platform& real = p->platform();
        FailingRemoveAll failing(real.firewall);
        IFirewall& fw = command == QLatin1String("remove") ? real.firewall : static_cast<IFirewall&>(failing);
        Platform pf{real.folder, real.service, fw, real.applyLock, real.engineLock, real.token, real.status, real.io};
        const Result r = Applier(pf).saveAndRemove(svc::request(config()));
        const bool expected = command == QLatin1String("remove")
                                  ? (r.outcome == Outcome::Ok || r.outcome == Outcome::SavedNotApplied)
                                  : r.outcome != Outcome::Ok;
        return result(r, expected);
    }
    if (command == QLatin1String("start")) {
        const Result r = svc::start(*p);
        return result(r, r.outcome == Outcome::Ok);
    }
    if (command == QLatin1String("stop")) {
        const Result r = svc::stop(*p);
        return result(r, r.outcome == Outcome::Ok);
    }
    if (command == QLatin1String("uninstall")) {
        const Result r = svc::uninstall(*p);
        return result(r, r.outcome == Outcome::Ok || r.outcome == Outcome::SavedNotApplied);
    }
    if (command == QLatin1String("status")) {
        const svc::ScmState s = svc::query();
        QFile f(p->dataFolder() + QStringLiteral("\\status.json"));
        const QByteArray status = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        out(QStringLiteral("STATUS %1").arg(QString::fromUtf8(QJsonDocument::fromJson(status).toJson(QJsonDocument::Compact))));
        out(QStringLiteral("RESULT ok status exists=%1 state=%2 pid=%3 exit=%4 specific=%5")
                .arg(s.exists).arg(s.state).arg(s.pid).arg(s.win32ExitCode).arg(s.specificExitCode));
        return 0;
    }
    if (command == QLatin1String("fw-check")) {
        QFile c(p->dataFolder() + QStringLiteral("\\config.json"));
        QFile t(p->dataFolder() + QStringLiteral("\\start.token"));
        if (!c.open(QIODevice::ReadOnly) || !t.open(QIODevice::ReadOnly)) {
            out(QStringLiteral("RESULT failed fw-check: config.json or start.token cannot be read"));
            return 1;
        }
        QString why;
        const std::optional<BridgeConfig> cfg = fromJson(c.readAll(), &why);
        const QJsonObject token = QJsonDocument::fromJson(t.readAll()).object();
        const FirewallGeneration g{token.value(QStringLiteral("generation")).toString().toLatin1()};
        if (!cfg) {
            out(QStringLiteral("RESULT failed fw-check: %1").arg(why));
            return 1;
        }
        win::WbemSession cim;
        win::CimFirewallMonitor mon(cim, p->installedExe());
        mon.threadInit();
        const IFirewallMonitor::Check v = mon.checkOnce(*cfg, g);
        const bool ok = v == IFirewallMonitor::Check::Match;
        out(QStringLiteral("RESULT %1 fw-check verdict=%2 why=%3")
                .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"))
                .arg(v == IFirewallMonitor::Check::Match ? QStringLiteral("Match")
                     : v == IFirewallMonitor::Check::Mismatch ? QStringLiteral("Mismatch") : QStringLiteral("Failed"))
                .arg(mon.lastWhy()));
        return ok ? 0 : 1;
    }
    out(QStringLiteral("RESULT failed usage: asb_win_qual [--exe <exe>] [--runtime <dir>] "
                       "detect|install|apply|apply-fail|remove|remove-fail|start|stop|uninstall|status|list-ports|fw-check|keyword-matrix "
                       "[--config <file>]"));
    return 2;
}
