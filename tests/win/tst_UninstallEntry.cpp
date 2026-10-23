// The Apps & features entry (WS-16; D63, INV-126), on every OS: its exact values over a recording registry, and the
// Applier's decisions about it over the WS-5 fakes (FakeService models the entry, the program and its folder). The
// real registry is tst_CliNative's (the Windows CI leg).

#include "core/Applier.h"
#include "platform/win/SelfElevate.h"
#include "platform/win/UninstallEntry.h"
#include "support/Fakes.h"

#include <QTest>

#include <functional>

using namespace asb;
using namespace asb::test;
using namespace asb::win;

namespace {

class RecordingRegistry final : public IRegistry {
public:
    QMap<QString, QList<Value>> keys;
    quint32 failWrite = 0;
    quint32 writeKey(const QString& subkey, const QList<Value>& values) override
    {
        if (failWrite) return failWrite;
        keys[subkey] = values;
        return 0;
    }
    quint32 deleteKey(const QString& subkey) override { return keys.remove(subkey) ? 0 : 2; }
    bool keyExists(const QString& subkey) override { return keys.contains(subkey); }
};

IRegistry::Value sz(const char* n, const QString& v)
{
    IRegistry::Value x;
    x.name = QString::fromLatin1(n);
    x.sz = v;
    return x;
}
IRegistry::Value dword(const char* n, quint32 v)
{
    IRegistry::Value x;
    x.type = IRegistry::Value::Type::Dword;
    x.name = QString::fromLatin1(n);
    x.dword = v;
    return x;
}

Applier applier(Rig& r)
{
    return Applier(*r.platform, r.world.clock);
}

void station(Rig& r)
{
    r.installedRunning(configA());
    r.service.entry = {true, r.service.installedExePath, r.service.version};
}

}  // namespace

class tst_UninstallEntry : public QObject {
    Q_OBJECT
private slots:
    // D63: the key, its view and every value, exactly.
    void valuesExact()
    {
        RecordingRegistry reg;
        UninstallEntry entry(reg);
        IUninstallEntry::Facts f;
        f.installLocation = QStringLiteral(R"(C:\Program Files\AetherSerialBridge)");
        f.installedExe = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)");
        f.version = QStringLiteral("1.1.0");
        f.exeBytes = 5 * 1024 * 1024 + 1;
        f.date = QDate(2026, 10, 17);
        QVERIFY(!entry.present());
        QCOMPARE(entry.write(f), QString());
        const QString key = QStringLiteral(R"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AetherSerialBridge)");
        QCOMPARE(reg.keys.keys(), QStringList{key});
        const QString exe = QStringLiteral("\"C:\\Program Files\\AetherSerialBridge\\aether-serial-bridge.exe\"");
        const QList<IRegistry::Value> expected{
            sz("DisplayName", QStringLiteral("Aether Serial Bridge")),
            sz("DisplayVersion", QStringLiteral("1.1.0")),
            sz("Publisher", QStringLiteral("AetherSDR")),
            sz("InstallLocation", QStringLiteral(R"(C:\Program Files\AetherSerialBridge)")),
            sz("DisplayIcon", exe + QStringLiteral(",0")),
            sz("UninstallString", exe + QStringLiteral(" --uninstall-from-apps")),
            dword("NoModify", 1),
            dword("NoRepair", 1),
            dword("EstimatedSize", 5 * 1024 + 1),
            sz("InstallDate", QStringLiteral("20261017")),
        };
        QCOMPARE(reg.keys[key], expected);
        QVERIFY(entry.present());
        QCOMPARE(entry.remove(), QString());
        QVERIFY(reg.keys.isEmpty());
        QCOMPARE(entry.remove(), QString());   // not there: not an error
        reg.failWrite = 5;
        QCOMPARE(entry.write(f), QStringLiteral("access denied (5)"));
    }

    // INV-126: written by the install path, with this build's program, and only then.
    void writtenOnlyForThisBuild()
    {
        {
            Rig r;   // a fresh install writes it, naming the installed program
            QCOMPARE(applier(r).run(std::nullopt, /*install=*/true).outcome, Outcome::Ok);
            QCOMPARE(r.service.entryWrites, 1);
            QCOMPARE(r.service.entry.uninstallExe, r.service.installedExePath);
            QVERIFY(r.world.log.indexOf(QStringLiteral("service:installOrConfigure")) <
                    r.world.log.indexOf(QStringLiteral("service:writeAppsEntry")));
        }
        {
            Rig r;   // an update (install over Current) rewrites it
            r.installedRunning(configA());
            QCOMPARE(applier(r).run(std::nullopt, true).outcome, Outcome::Ok);
            QCOMPARE(r.service.entryWrites, 1);
        }
        {
            Rig r;   // Save & apply without install: not written
            r.installedRunning(configA());
            QCOMPARE(applier(r).run(std::nullopt, false).outcome, Outcome::Ok);
            QCOMPARE(r.service.entryWrites, 0);
        }
        {
            Rig r;   // the install copy refused (not this build's digest, or any failure): not written
            r.service.fail[QStringLiteral("installOrConfigure")] = QStringLiteral("the copy is not this build");
            QVERIFY(applier(r).run(std::nullopt, true).outcome != Outcome::Ok);
            QCOMPARE(r.service.entryWrites, 0);
            QVERIFY(!r.service.entry.present);
        }
    }

    // INV-126: keeping another installed build never writes this build's entry. Kills inv126-entry-on-keep-other-build.
    void notWrittenOnKeepOtherBuild()
    {
        Rig r;
        r.installedRunning(configA());
        r.service.installation.state = InstallState::IncompleteInstall;
        r.service.installation.otherBuild = true;
        SaveRequest req;
        req.config = configB();
        req.source.kind = Source::Committed;
        req.source.sha256 = digest(configA());
        req.keepInstalledBuild = true;
        const Result res = applier(r).run(req, /*install=*/false);
        QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
        QCOMPARE(r.service.entryWrites, 0);
        QVERIFY(!r.world.log.contains(QStringLiteral("service:writeAppsEntry")));
    }

    void notWrittenByStart()
    {
        Rig r;
        r.installedRunning(configA());
        QCOMPARE(applier(r).start().outcome, Outcome::Ok);
        QCOMPARE(r.service.entryWrites, 0);
        QVERIFY(!r.world.log.contains(QStringLiteral("service:writeAppsEntry")));
    }

    // INV-126: removed last: after the service, the program, the record and the settings.
    void removedLast()
    {
        Rig r;
        station(r);
        QCOMPARE(applier(r).uninstall({/*removeSettings=*/true}).outcome, Outcome::Ok);
        const QStringList all = r.world.log.all();
        const int entry = r.world.log.indexOf(QStringLiteral("service:removeAppsEntry"));
        QVERIFY(entry > 0);
        for (const QString& e : {QStringLiteral("service:uninstall"), QStringLiteral("service:removeProgram"),
                                 QStringLiteral("lockio:remove engine.lock.id"), QStringLiteral("io:remove config.json"),
                                 QStringLiteral("folder:removeIfEmpty")})
            QVERIFY2(r.world.log.indexOf(e) < entry, qPrintable(e));
        // Nothing else is removed under the lock after it (the holds then close; the empty folder goes after them).
        for (int i = entry + 1; i < all.size(); ++i)
            QVERIFY2(!all[i].startsWith(QLatin1String("service:")) || all[i] == QLatin1String("service:removeProgramFolder"),
                     qPrintable(all[i]));
        QVERIFY(!r.service.entry.present);
    }

    // INV-126: while the program is kept, so is the entry. Kills inv126-entry-removed-first.
    void keptWhenProgramKept()
    {
        Rig r;
        station(r);
        r.service.fail[QStringLiteral("removeProgram!")] = QStringLiteral("in use");
        QVERIFY(applier(r).uninstall({}).outcome != Outcome::Ok);
        QVERIFY(r.service.programPresent);
        QVERIFY(r.service.entry.present);
        QVERIFY(!r.world.log.contains(QStringLiteral("service:removeAppsEntry")));
    }

    // INV-126: a failure injected at every step of the uninstall (each fault a seam can report, and each call returning
    // after its deadline): whenever the entry remains, the program its UninstallString names exists.
    void keptEntryNamesExistingProgram()
    {
        using Fault = std::function<void(Rig&)>;
        QList<std::pair<QString, Fault>> faults{
            {"firewall connect", [](Rig& r) { r.firewall.fail[QStringLiteral("connect")] = QStringLiteral("x"); }},
            {"firewall removeAll", [](Rig& r) { r.firewall.fail[QStringLiteral("removeAll")] = QStringLiteral("x"); }},
            {"service settle", [](Rig& r) { r.service.fail[QStringLiteral("reconcileOutstandingJobs")] = QStringLiteral("x"); }},
            {"service uninstall", [](Rig& r) { r.service.fail[QStringLiteral("uninstall")] = QStringLiteral("x"); }},
            {"apply.lock mark", [](Rig& r) { r.world.applyLock.failMark = QStringLiteral("x"); }},
            {"engine.lock mark", [](Rig& r) { r.engineLock.failMark = QStringLiteral("x"); }},
            {"program kept", [](Rig& r) { r.service.fail[QStringLiteral("removeProgram!")] = QStringLiteral("x"); }},
            {"program kept, clear fails",
             [](Rig& r) {
                 r.service.fail[QStringLiteral("removeProgram!")] = QStringLiteral("x");
                 r.world.applyLock.failClear = QStringLiteral("y");
             }},
            {"engine.lock.id",
             [](Rig& r) {
                 static_cast<SpyFileIo&>(r.engineLock.io()).refuse = [](const QString& op, const QString& name) {
                     return op == QLatin1String("remove") && name == QLatin1String("engine.lock.id") ? QStringLiteral("x")
                                                                                                    : QString();
                 };
             }},
            {"settings file", [](Rig& r) { r.world.files.failAlways(FakeFileIo::Op::Remove); }},
            {"settings folder", [](Rig& r) { r.folder.failRemoveFolder = QStringLiteral("x"); }},
        };
        // Not in the list: the entry's own removal failing once the program is gone. Nothing can then keep the two
        // together; the result names it ("could not be removed"), and --uninstall from any copy of the program removes
        // it (uninstallEntryRemovalFailureIsNamed).
        // Lateness at every call of a clean run.
        QStringList events;
        {
            Rig r;
            station(r);
            applier(r).uninstall({true});
            events = r.world.log.all();
        }
        for (int i = 0; i < events.size(); ++i) {
            const QString event = events[i];
            faults << std::pair<QString, Fault>{QStringLiteral("late at %1 (%2)").arg(i).arg(event), [event](Rig& r) {
                                                    auto seen = std::make_shared<bool>(false);
                                                    r.world.log.onAdd = [&r, event, seen](const QString& e) {
                                                        if (*seen || e != event) return;
                                                        *seen = true;
                                                        r.world.clock.advance(std::chrono::seconds(400));
                                                    };
                                                }};
        }
        for (const auto& [name, fault] : faults) {
            Rig r;
            station(r);
            fault(r);
            const Result res = applier(r).uninstall({true});
            r.world.log.onAdd = {};
            const QString context = name + QStringLiteral(": ") + enumName(res.outcome) + QStringLiteral(" ") + res.message;
            QVERIFY2(r.service.entryNamesExistingProgram(), qPrintable(context));
            if (r.service.programPresent) QVERIFY2(r.service.entry.present, qPrintable(context));
            QVERIFY2(r.world.violations.isEmpty(), qPrintable(context + r.world.violations.join(QLatin1Char(','))));
        }
    }

    // The one failure keptEntryNamesExistingProgram cannot cover: the entry's own removal fails after the program went.
    // The result says so and how to finish.
    void uninstallEntryRemovalFailureIsNamed()
    {
        Rig r;
        station(r);
        r.service.fail[QStringLiteral("removeAppsEntry")] = QStringLiteral("access denied (5)");
        const Result res = applier(r).uninstall({});
        QVERIFY2(res.message.contains(QStringLiteral("The Apps & features entry could not be removed")), qPrintable(res.message));
        QVERIFY2(res.fileChanges.join(QLatin1Char('|')).contains(QStringLiteral("kept the Apps & features entry")),
                 qPrintable(res.fileChanges.join(QLatin1Char('|'))));
    }

    // INV-126: a write that fails does not fail the install; the result says so.
    void writeFailureDoesNotFailInstall()
    {
        Rig r;
        r.service.fail[QStringLiteral("writeAppsEntry!")] = QStringLiteral("access denied (5)");
        const Result res = applier(r).run(std::nullopt, true);
        QCOMPARE(res.outcome, Outcome::Ok);
        QVERIFY2(res.message.contains(QStringLiteral("Apps & features entry not written: access denied (5).")), qPrintable(res.message));
        QVERIFY(!r.service.entry.present);
    }

    // D64: the arguments of the relaunch are quoted by the rules CommandLineToArgvW reads back.
    void selfElevateQuotesByTheWindowsRules()
    {
        QCOMPARE(SelfElevate::commandLine({QStringLiteral("--uninstall-from-apps"), QStringLiteral("--confirmed")}),
                 QStringLiteral("--uninstall-from-apps --confirmed"));
        QCOMPARE(SelfElevate::quoteArgument(QString()), QStringLiteral("\"\""));
        // (moc's tokenizer cannot read raw strings with quotes in them: plain escapes here)
        QCOMPARE(SelfElevate::quoteArgument(QStringLiteral("C:\\a b\\")), QStringLiteral("\"C:\\a b\\\\\""));
        QCOMPARE(SelfElevate::quoteArgument(QStringLiteral("say \"hi\"")), QStringLiteral("\"say \\\"hi\\\"\""));
        QCOMPARE(SelfElevate::quoteArgument(QStringLiteral("a\\\"b")), QStringLiteral("\"a\\\\\\\"b\""));
    }
};

QTEST_GUILESS_MAIN(tst_UninstallEntry)
#include "tst_UninstallEntry.moc"
