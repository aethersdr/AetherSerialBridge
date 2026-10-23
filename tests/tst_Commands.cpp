// The commands (WS-9; master §4.10, §4.6): runCommand over a fake Host and the WS-5 fakes. Each command reaches
// the Applier operation the table names; mode entry comes first (guard, then elevation, then the Platform:
// INV-112); a refused command builds nothing; exit codes 0/1/2/3; edits of the managed config go to
// config.pending.json through Applier::edit and nothing else; read-only commands never write (INV-44); --config
// paths obey the scratch rules (INV-26, INV-90, INV-101).

#include "cli/Commands.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "support/FakeCliHost.h"

#include <QDir>
#include <QFile>
#include <QTest>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace asb;
using namespace asb::cli;
using namespace asb::test;

namespace {

const QString kPending = QString::fromLatin1(ConfigFiles::kPending);

int run(FakeCliHost& h, CapturingConsole& c, const QStringList& args)
{
    return runCommand(args, h, c);
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

class tst_Commands : public QObject {
    Q_OBJECT

private slots:
    // ---- mode entry and routing -----------------------------------------------------------------
    void mutatingCommandEntersGuardFirst()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--stop"}), 0);
        QCOMPARE(h.calls, (QStringList{"guard", "probe", "elevated", "platform"}));
        QVERIFY(r.world.log.contains(QStringLiteral("service:requestStop")));
        QVERIFY2(!c.outText().isEmpty(), qPrintable(c.errText()));
    }
    void notElevatedBuildsNothing()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isElevated = false;
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--apply"}), 1);
        QVERIFY(!h.calls.contains("platform"));
        QVERIFY2(c.errText().contains("must run as root: sudo aether-serial-bridge --apply"), qPrintable(c.errText()));
        QVERIFY(r.world.log.all().isEmpty());
    }
    void routesEachCommandToItsOperation_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("event");
        QTest::newRow("--apply") << QStringList{"--apply"} << "service:inhibitStarts";
        QTest::newRow("--start") << QStringList{"--start"} << "service:start";
        QTest::newRow("--stop") << QStringList{"--stop"} << "service:requestStop";
        QTest::newRow("--uninstall") << QStringList{"--uninstall"} << "service:uninstall";
        QTest::newRow("--repair --yes") << QStringList{"--repair", "--yes"} << "folder:inspect";
    }
    void routesEachCommandToItsOperation()
    {
        QFETCH(QStringList, args);
        QFETCH(QString, event);
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        const int code = run(h, c, args);
        QVERIFY2(r.world.log.contains(event), qPrintable(r.world.log.all().join('\n')));
        QCOMPARE(h.results.size(), 1);
        const bool good = h.results[0].outcome == Outcome::Ok || h.results[0].outcome == Outcome::SavedNotApplied;
        QCOMPARE(code, good ? 0 : 1);
        QVERIFY(c.outText().contains(h.results[0].message) || c.errText().contains(h.results[0].message));
    }
    void installOnAFreshMachine()
    {
        Rig r;
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--install"}), 0);
        QVERIFY(r.world.log.contains(QStringLiteral("service:installOrConfigure")));
        QCOMPARE(h.results.value(0).outcome, Outcome::Ok);
    }
    void uninstallSaysTheConfigIsKept()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall"}), 0);
        QVERIFY2(c.outText().endsWith(QStringLiteral("The settings are kept in %1.").arg(QDir::toNativeSeparators(h.dataDir))),
                 qPrintable(c.outText()));
        QCOMPARE(r.file(QStringLiteral("config.json")), configA());
    }

    // ---- WS-16: --uninstall [--remove-settings], --uninstall-from-apps (INV-125, INV-126) ----------------------
    // --remove-settings only with --uninstall (usage, exit 2, nothing built); with it, the known files go.
    void removeSettingsOnlyWithUninstall()
    {
        {
            Rig r;
            r.installedRunning(configA());
            FakeCliHost h(r);
            CapturingConsole c;
            QCOMPARE(run(h, c, {"--stop", "--remove-settings"}), 2);
            QVERIFY2(c.errText().contains("--remove-settings can only be used with --uninstall"), qPrintable(c.errText()));
            QVERIFY(h.calls.isEmpty());
        }
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall", "--remove-settings"}), 0);
        QVERIFY(!r.has(QStringLiteral("config.json")));
        QVERIFY2(c.outText().endsWith(QStringLiteral("The settings were removed.")), qPrintable(c.outText()));
        QCOMPARE(h.results.size(), 1);
    }

    // The last line follows what the uninstall did with the settings: a file this program does not write keeps the
    // folder but the settings are still gone; never "kept in" after a removal.
    void removeSettingsLastLineFollowsTheOutcome()
    {
        Rig r;
        r.installedRunning(configA());
        r.world.files.plantFile(QStringLiteral("notes.txt"), "mine", QStringLiteral("root"));
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall", "--remove-settings"}), 0);
        QVERIFY(!r.has(QStringLiteral("config.json")));
        QVERIFY(r.has(QStringLiteral("notes.txt")));
        QVERIFY2(c.outText().endsWith(QStringLiteral("The settings were removed.")), qPrintable(c.outText()));
        QVERIFY2(!c.outText().contains(QStringLiteral("The settings are kept in")), qPrintable(c.outText()));
    }

    // INV-125: the ports that go down and the connected clients are printed before anything changes.
    void uninstallPrintsPreviewFirst()
    {
        Rig r;
        r.installedRunning(configA());
        StatusReport s;
        PortStatus port;
        port.name = QStringLiteral("Amp");
        port.tcpPort = 4001;
        port.state = BridgeState::Connected;
        port.client = QStringLiteral("192.0.2.10:50000");
        s.ports << port;
        r.service.status = s;
        FakeCliHost h(r);
        CapturingConsole c;
        r.world.log.onAdd = [&c](const QString& e) {
            if (e == QLatin1String("applyLock:acquired")) c.out(QStringLiteral("<lock>"));
        };
        QCOMPARE(run(h, c, {"--uninstall"}), 0);
        r.world.log.onAdd = {};
        QVERIFY2(!c.outLines.isEmpty() &&
                     c.outLines.first() ==
                         QStringLiteral("Stopping port 4001 (Amp): the client at 192.0.2.10:50000 is connected and will be disconnected."),
                 qPrintable(c.outText()));
        QCOMPARE(c.outLines.value(1), QStringLiteral("<lock>"));   // printed before the operation took its lock
    }

    // INV-126: Settings starts the console exe NOT elevated. It takes no lock, installs no guard and runs no operation
    // before elevating; a ReadOnly Platform is built for the question; then its own image is relaunched with exactly
    // the fixed arguments, and the child's exit code is its own. Kills inv126-apps-mode-runs-applier-before-elevation.
    void uninstallFromAppsUnelevatedTakesNoLockNoApplier()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isWindows = true;
        h.isElevated = false;
        h.boxAnswer = true;
        h.relaunchResult = {true, false, 7, {}};
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps"}), 7);
        QVERIFY(!h.calls.contains(QStringLiteral("guard")));
        QVERIFY(!h.calls.contains(QStringLiteral("probe")));
        QVERIFY2(!r.world.log.contains(QStringLiteral("applyLock:")), qPrintable(r.world.log.all().join('\n')));
        QVERIFY(h.results.isEmpty());   // no operation finished: none ran
        QCOMPARE(r.service.installation.state, InstallState::Current);
        QVERIFY(h.calls.indexOf(QStringLiteral("detachConsole")) < h.calls.indexOf(QStringLiteral("messageBox")));
        QVERIFY(h.calls.indexOf(QStringLiteral("messageBox")) < h.calls.indexOf(QStringLiteral("relaunch")));
        QCOMPARE(h.boxes.size(), 1);
        QVERIFY2(h.boxes[0].startsWith(QStringLiteral("Uninstall Aether Serial Bridge? This stops the bridge: port 4001 (Amp).")),
                 qPrintable(h.boxes[0]));
        QVERIFY(h.boxes[0].contains(QStringLiteral("Windows will ask for administrator permission.")));
    }
    void uninstallFromAppsNoChangesNothing()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isWindows = true;
        h.isElevated = false;
        h.boxAnswer = false;
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps"}), 0);
        QVERIFY(h.relaunched.isEmpty());
        QVERIFY(!r.world.log.contains(QStringLiteral("applyLock:")));
        QCOMPARE(r.service.installation.state, InstallState::Current);
    }
    void uninstallFromAppsUacCancelled()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isWindows = true;
        h.isElevated = false;
        h.boxAnswer = true;
        h.relaunchResult = {false, true, 0, {}};
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps"}), 1);
        QCOMPARE(h.boxes.last(), QStringLiteral("Cancelled; nothing was changed."));
        QCOMPARE(r.service.installation.state, InstallState::Current);
    }
    void uninstallFromAppsRelaunchArgsExact()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isWindows = true;
        h.isElevated = false;
        h.boxAnswer = true;
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps"}), 0);
        QCOMPARE(h.relaunched, (QList<QStringList>{{QStringLiteral("--uninstall-from-apps"), QStringLiteral("--confirmed")}}));
    }
    // --confirmed in a process that is not elevated means nothing: it still asks.
    void confirmedIgnoredWhenNotElevated()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isWindows = true;
        h.isElevated = false;
        h.boxAnswer = false;
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps", "--confirmed"}), 0);
        QCOMPARE(h.boxes.size(), 1);
        QVERIFY(h.relaunched.isEmpty());
        QCOMPARE(r.service.installation.state, InstallState::Current);
    }
    // The elevated half is the ordinary uninstall (guard first, the settings kept), then one box with the outcome. Started
    // elevated without --confirmed it asks first.
    void uninstallFromAppsElevatedHalfIsTheOrdinaryUninstall()
    {
        {
            Rig r;
            r.installedRunning(configA());
            FakeCliHost h(r);
            h.isWindows = true;
            CapturingConsole c;
            QCOMPARE(run(h, c, {"--uninstall-from-apps", "--confirmed"}), 0);
            // The elevation check comes first here (it decides the half; a token query, no object), then mode entry: the
            // guard before the Platform and any operation (INV-112).
            QCOMPARE(h.calls.mid(0, 3), (QStringList{"elevated", "guard", "probe"}));
            QVERIFY(h.calls.indexOf(QStringLiteral("guard")) < h.calls.indexOf(QStringLiteral("platform")));
            QVERIFY(!h.calls.contains(QStringLiteral("relaunch")));
            QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
            QCOMPARE(r.file(QStringLiteral("config.json")), configA());
            QCOMPARE(h.boxes.size(), 1);   // no question, one outcome
            QVERIFY2(h.boxes[0].startsWith(QStringLiteral("Uninstalled:")), qPrintable(h.boxes[0]));
            QVERIFY(h.boxes[0].contains(QStringLiteral("The settings are kept in")));
        }
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isWindows = true;
        h.boxAnswer = false;
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps"}), 0);
        QCOMPARE(h.boxes.size(), 1);
        QVERIFY(!h.boxes[0].contains(QStringLiteral("administrator permission")));
        QCOMPARE(r.service.installation.state, InstallState::Current);
    }
    void uninstallFromAppsOnlyOnWindows()
    {
        Rig r;
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps"}), 2);
        QVERIFY(h.calls.isEmpty());
    }
    // What the adapters reported outside Result (lock files, the service's files) is printed once, after it.
    void reportsAreFolded()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.reports << QStringLiteral("wrote /usr/local/lib/systemd/system/aether-serial-bridge.service");
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--stop"}), 0);
        QVERIFY2(c.outText().contains("Also changed: wrote /usr/local/lib/systemd/system/aether-serial-bridge.service."),
                 qPrintable(c.outText()));
        QVERIFY(h.results.value(0).fileChanges.contains(QStringLiteral("wrote /usr/local/lib/systemd/system/aether-serial-bridge.service")));
    }
    // A program the uninstall keeps (here: renamed aside, neither scheduled nor renamed back) is named
    // with its way out in the Apps & features dialog, and the entry stays for the retry.
    void uninstallFromAppsDialogNamesTheKeptProgram()
    {
        Rig r;
        r.installedRunning(configA());
        const QString why = QStringLiteral("rename it back to aether-serial-bridge.exe and uninstall again, or delete it "
                                           "after the next restart");
        r.service.fail[QStringLiteral("removeProgram!")] = why;
        FakeCliHost h(r);
        h.isWindows = true;
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall-from-apps", "--confirmed"}), 1);
        QCOMPARE(h.boxes.size(), 1);
        QVERIFY2(h.boxes[0].contains(QStringLiteral("Kept: %1 (%2)").arg(r.service.installedExePath, why)), qPrintable(h.boxes[0]));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:removeAppsEntry")));   // the entry is not touched
    }

    // What the Result's fileChanges hold beyond the message (an uninstall's program, entry and settings
    // files) is printed with the adapters' reports, each line once, before the settings line; a line the message
    // already says is not repeated.
    void fileChangesArePrinted()
    {
        Rig r;
        r.installedRunning(configA());
        r.world.files.plantFile(QStringLiteral("notes.txt"), "x", QStringLiteral("root"));
        FakeCliHost h(r);
        h.reports << QStringLiteral("removed the Apps & features entry") << QStringLiteral("wrote an adapter line");
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--uninstall", "--remove-settings"}), 0);
        const Result res = h.results.value(0);
        const QString exe = QStringLiteral("removed %1").arg(r.service.installedExePath);
        QVERIFY2(res.fileChanges.contains(exe), qPrintable(res.fileChanges.join(QLatin1Char('|'))));
        const QString out = c.outText();
        for (const QString& line : {exe, QStringLiteral("removed the Apps & features entry"),
                                    QStringLiteral("kept notes.txt (not a file this program writes)"),
                                    QStringLiteral("wrote an adapter line")})
            QVERIFY2(out.count(line) == 1, qPrintable(line + QStringLiteral("\n") + out));
        qsizetype also = -1;
        for (qsizetype i = 0; i < c.outLines.size(); ++i)
            if (c.outLines[i].startsWith(QLatin1String("Also changed: "))) also = i;
        QVERIFY2(also >= 0 && also == c.outLines.size() - 2, qPrintable(out));   // before the settings line, the last
        QVERIFY2(c.outLines.last().startsWith(QLatin1String("The settings were removed")), qPrintable(out));

        // A line the message already holds (an install's "Folder changes: ...") is not printed again.
        Rig i;
        FakeCliHost hi(i);
        CapturingConsole ci;
        QCOMPARE(run(hi, ci, {"--install"}), 0);
        const Result installed = hi.results.value(0);
        QVERIFY(!installed.fileChanges.isEmpty());
        for (const QString& line : installed.fileChanges)
            QVERIFY2(ci.outText().count(line) == 1, qPrintable(line + QStringLiteral("\n") + ci.outText()));
    }
    void platformFailureExitCode()
    {
        Rig r;
        FakeCliHost h(r);
        h.platformError = QStringLiteral("Cannot determine the Windows ProgramData folder");
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--stop"}), 3);
        QVERIFY(c.errText().contains("ProgramData"));
    }

    // ---- exit 3: an untrusted data folder is refused before the Applier is asked --------------------
    void untrustedDataFolderIsExit3()
    {
        for (const QStringList& args : {QStringList{"--apply"}, QStringList{"--start"}, QStringList{"--install"},
                                        QStringList{"--add-port", "K", "/dev/ttyUSB0", "4009"}}) {
            Rig r;
            r.installedRunning(configA());
            r.folder.owner = QStringLiteral("attacker");
            FakeCliHost h(r);
            CapturingConsole c;
            QCOMPARE(run(h, c, args), 3);
            QVERIFY2(c.errText().contains("is not trusted") && c.errText().contains("Nothing was changed"), qPrintable(c.errText()));
            QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
            QVERIFY(!r.world.applyLock.held());
            QCOMPARE(r.folder.mutations(), 0);
        }
        // --install/--apply point at --force-recreate; with it (and with --repair) the Applier decides.
        Rig r;
        r.installedRunning(configA());
        r.folder.owner = QStringLiteral("attacker");
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--apply"}), 3);
        QVERIFY(c.errText().contains("--apply --force-recreate, or --repair"));
        CapturingConsole c2;
        run(h, c2, {"--apply", "--force-recreate"});
        QVERIFY(r.world.log.contains(QStringLiteral("folder:renameAside")));
    }

    // ---- --repair asks first ----------------------------------------------------------------------
    void repairAsksUnlessYes()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        // No terminal to ask on: a usage error, and nothing happens.
        QCOMPARE(run(h, c, {"--repair"}), 2);
        QVERIFY(c.errText().contains("add --yes"));
        QVERIFY(!h.calls.contains("platform"));
        h.answer = false;
        CapturingConsole c2;
        QCOMPARE(run(h, c2, {"--repair"}), 1);
        QVERIFY(c2.outText().contains("Nothing was changed."));
        QVERIFY(!h.calls.contains("platform"));
        h.answer = true;
        CapturingConsole c3;
        run(h, c3, {"--repair"});
        QVERIFY(h.calls.contains("platform"));
        QCOMPARE(h.results.size(), 1);
    }

    // ---- edits of the managed config: config.pending.json through Applier::edit ----------------------
    void editsGoToPending()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--add-port", "KAT500", "/dev/ttyUSB7", "4009", "--bind", "127.0.0.1"}), 0);
        QVERIFY2(c.outText().contains("Added KAT500: /dev/ttyUSB7 at 38400 8N1 on 127.0.0.1:4009."), qPrintable(c.outText()));
        QVERIFY(c.outText().contains("run --apply to apply it"));
        QVERIFY(r.has(kPending));
        const auto pending = fromJson(r.file(kPending), nullptr);
        QVERIFY(pending);
        QCOMPARE(pending->ports.size(), 2);
        QCOMPARE(r.file(QString::fromLatin1(ConfigFiles::kConfig)), configA());   // committed untouched
        QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
        // A second edit starts from the pending file.
        CapturingConsole c2;
        QCOMPARE(run(h, c2, {"--set-enabled", "kat500", "off"}), 0);
        QVERIFY(!fromJson(r.file(kPending), nullptr)->ports[1].enabled);
        QVERIFY(c2.outText().contains("KAT500 is now disabled."));
    }
    void refusedEditWritesNothing()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--remove-port", "nope"}), 1);
        QVERIFY2(c.errText().contains("error: No port named 'nope'. See --show-config."), qPrintable(c.errText()));
        QVERIFY(!r.has(kPending));
        CapturingConsole c2;
        QCOMPARE(run(h, c2, {"--add-port", "Amp", "/dev/ttyUSB1", "4100"}), 1);   // configA's port is named Amp
        QVERIFY2(c2.errText().contains("already exists"), qPrintable(c2.errText()));
        QVERIFY(!r.has(kPending));
    }
    // An edit the editor refuses still says what the Applier's prologue changed (the folder secured).
    void refusedEditStillReportsFolderChanges()
    {
        Rig r;
        r.installedRunning(configA());
        r.folder.normaliseChanges = {QStringLiteral("data folder mode set to 0755")};
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--remove-port", "nope"}), 1);
        QVERIFY2(c.errText().contains("error: No port named 'nope'"), qPrintable(c.errText()));
        QVERIFY2(c.errText().contains("Changed anyway: data folder mode set to 0755."), qPrintable(c.errText()));
        QVERIFY(!r.has(kPending));
    }
    // The adapters' reports join what the refused edit says it changed anyway, each line once (one of them
    // repeats a line the Result already names), and the result hook still receives them.
    void refusedEditFoldsAdapterReportsOnce()
    {
        Rig r;
        r.installedRunning(configA());
        r.folder.normaliseChanges = {QStringLiteral("data folder mode set to 0755")};
        FakeCliHost h(r);
        h.reports << QStringLiteral("data folder mode set to 0755") << QStringLiteral("apply.lock mode set to 0600")
                  << QStringLiteral("apply.lock mode set to 0600");
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--remove-port", "nope"}), 1);
        QVERIFY2(c.errText().contains("Changed anyway: data folder mode set to 0755; apply.lock mode set to 0600."),
                 qPrintable(c.errText()));
        QCOMPARE(h.results.size(), 1);
        QVERIFY(h.reports.isEmpty());   // taken
        QVERIFY(!r.has(kPending));
    }
    void discardPending()
    {
        Rig r;
        r.installedRunning(configA());
        r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--discard-pending"}), 0);
        QVERIFY(!r.has(kPending));
    }

    // ---- read only (INV-44) -------------------------------------------------------------------------
    void statusShowsServiceAndAck()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--status"}), 0);
        QVERIFY2(c.outText().contains("Service: running (pid "), qPrintable(c.outText()));
        QVERIFY2(c.outText().contains("Config: the bridge reported loading the saved config.json."), qPrintable(c.outText()));
        QVERIFY(!h.calls.contains("guard"));   // read only: no mode entry
        QCOMPARE(r.folder.mutations(), 0);
        QVERIFY(!r.world.log.contains(QStringLiteral("io:")));
        QVERIFY(!r.world.applyLock.held());
    }
    // Documented: --status exits 0 when the service manager cannot be queried, printing
    // "Service: unknown (...)" and whatever else it can.
    void statusUnqueryableIsExit0_data()
    {
        QTest::addColumn<QString>("failing");
        QTest::newRow("detect") << "detect";
        QTest::newRow("query") << "query";
    }
    void statusUnqueryableIsExit0()
    {
        QFETCH(QString, failing);
        Rig r;
        r.installedRunning(configA());
        r.service.fail[failing] = QStringLiteral("the service manager is not answering");
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--status"}), 0);
        QVERIFY2(c.outText().contains("Service: unknown (") && c.outText().contains("not answering"), qPrintable(c.outText()));
    }
    void statusNotInstalled()
    {
        Rig r;
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--status"}), 0);
        QVERIFY(c.outText().contains("Service: not installed"));
        QVERIFY(c.outText().contains("No status reported yet."));
    }
    void statusUntrustedFolderIsExit3()
    {
        Rig r;
        r.installedRunning(configA());
        r.folder.owner = QStringLiteral("attacker");
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--status"}), 3);
        QCOMPARE(r.folder.mutations(), 0);
    }
    void showConfigNeverWrites()
    {
        Rig r;
        r.installedRunning(configA());
        r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--show-config"}), 0);
        QVERIFY2(c.outText().contains("Config: ") && c.outText().contains("Amp"), qPrintable(c.outText()));
        QVERIFY(c.outText().contains("Saved, not applied (config.pending.json)"));
        QCOMPARE(r.folder.mutations(), 0);
        QVERIFY(!r.world.log.contains(QStringLiteral("io:")));
        QVERIFY(r.has(kPending));
    }

    // ---- --config: the scratch rules come first (INV-26, INV-90, INV-101) -------------------------------
    void configWithServiceCommandBuildsNothing()
    {
        Rig r;
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--apply", "--config", "/tmp/x.json"}), 2);
        QVERIFY(h.calls.isEmpty());
        QVERIFY(c.errText().contains("--config cannot be used with --apply"));
    }
    void runRefusesManagedConfig()
    {
        Rig r;
        FakeCliHost h(r);
        QVERIFY(writeFile(h.managedConfigFile(), "{\"version\":1,\"ports\":[]}"));
        for (const QStringList& args : {QStringList{"--run", "--config", h.managedConfigFile()},
                                        QStringList{"--run", "--config", h.dataFolder() + "/scratch.json"},
                                        QStringList{"--add-port", "K", "d", "1", "--config", h.dataFolder() + "/x.json"},
                                        QStringList{"--show-config", "--config", h.managedConfigFile()}}) {
            CapturingConsole c;
            QCOMPARE(run(h, c, args), 2);
            QVERIFY2(c.errText().contains("data folder") || c.errText().contains("installed config.json"), qPrintable(c.errText()));
        }
        QVERIFY(h.calls.isEmpty());
    }
    void runRefusesInstalledImage()
    {
        Rig r;
        FakeCliHost h(r);
        const QString scratch = h.tmp.filePath("scratch");
        QVERIFY(QDir().mkpath(scratch));
        QVERIFY(writeFile(scratch + "/c.json", "{\"version\":1,\"ports\":[]}"));
        h.installed = h.tmp.filePath("installed-exe");
        QVERIFY(writeFile(h.installed, "exe"));
        h.own = h.installed;   // running from the installed program
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--run", "--config", scratch + "/c.json"}), 2);
        QVERIFY2(c.errText().contains("installed service program"), qPrintable(c.errText()));
        QVERIFY(h.calls.isEmpty());
    }
    void runForcesLoopbackAndRunsTheScratchConfig()
    {
#ifdef _WIN32
        QSKIP("scratch-folder cases use the POSIX adapters (the Windows scratch path is covered by tst_CommandLine)");
#else
        Rig r;
        FakeCliHost h(r);
        const QString scratch = h.tmp.filePath("scratch");
        QVERIFY(QDir().mkpath(scratch));
        ::chmod(QFile::encodeName(scratch).constData(), 0700);
        QVERIFY(writeFile(scratch + "/c.json", R"({"version":1,"ports":[{"name":"K","comPort":"/dev/ttyUSB0","tcpPort":4001,"bindAddress":"0.0.0.0"}]})"));
        h.own = h.tmp.filePath("copy-exe");
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--run", "--config", scratch + "/c.json"}), 0);
        QVERIFY(h.scratchConfig);
        QCOMPARE(h.scratchConfig->ports[0].bindAddress, QStringLiteral("127.0.0.1"));
        QVERIFY2(c.errText().contains("listens on 127.0.0.1 only"), qPrintable(c.errText()));
        // Mode entry (INV-112): the guard, the probe and the blocked stop signals come before anything
        // else the run builds; no elevation and no managed Platform (no Applier, no managed state).
        QCOMPARE(h.calls.mid(0, 3), (QStringList{"guard", "probe", "signals"}));
        QVERIFY(h.calls.indexOf("scratchFolder") > 2);
        QVERIFY(!h.calls.contains("elevated") && !h.calls.contains("platform"));
#endif
    }
    void runRefusesInvalidOrMissingScratch()
    {
#ifdef _WIN32
        QSKIP("scratch-folder cases use the POSIX adapters (the Windows scratch path is covered by tst_CommandLine)");
#else
        Rig r;
        FakeCliHost h(r);
        const QString scratch = h.tmp.filePath("scratch");
        QVERIFY(QDir().mkpath(scratch));
        ::chmod(QFile::encodeName(scratch).constData(), 0700);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--run", "--config", scratch + "/absent.json"}), 1);
        QVERIFY(c.errText().contains("does not exist"));
        QVERIFY(writeFile(scratch + "/bad.json", R"({"version":1,"ports":[{"name":"K","comPort":"x","tcpPort":0}]})"));
        CapturingConsole c2;
        QCOMPARE(run(h, c2, {"--run", "--config", scratch + "/bad.json"}), 1);
        QVERIFY2(c2.errText().contains("is not valid"), qPrintable(c2.errText()));
        QVERIFY(!h.calls.join(' ').contains("runScratch"));
#endif
    }
    void untrustedScratchFolderIsExit3()
    {
#ifdef _WIN32
        QSKIP("scratch-folder cases use the POSIX adapters (the Windows scratch path is covered by tst_CommandLine)");
#else
        Rig r;
        FakeCliHost h(r);
        const QString scratch = h.tmp.filePath("open");
        QVERIFY(QDir().mkpath(scratch));
        ::chmod(QFile::encodeName(scratch).constData(), 0777);
        QVERIFY(writeFile(scratch + "/c.json", "{\"version\":1,\"ports\":[]}"));
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--show-config", "--config", scratch + "/c.json"}), 3);
        QVERIFY2(c.errText().contains("not trusted for a scratch config"), qPrintable(c.errText()));
#endif
    }
    void scratchEditsTouchOnlyTheScratchFile()
    {
#ifdef _WIN32
        QSKIP("scratch-folder cases use the POSIX adapters (the Windows scratch path is covered by tst_CommandLine)");
#else
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        const QString scratch = h.tmp.filePath("scratch");
        QVERIFY(QDir().mkpath(scratch));
        ::chmod(QFile::encodeName(scratch).constData(), 0700);
        const QString file = scratch + "/test.json";
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--add-port", "KAT500", "/dev/ttyUSB0", "4001", "--config", file}), 0);
        QVERIFY2(c.outText().contains("Saved " + file), qPrintable(c.outText() + c.errText()));
        const auto saved = fromJson(readFile(file), nullptr);
        QVERIFY(saved);
        QCOMPARE(saved->ports.size(), 1);
        CapturingConsole c2;
        QCOMPARE(run(h, c2, {"--show-config", "--config", file}), 0);
        QVERIFY(c2.outText().contains("KAT500"));
        CapturingConsole c3;
        QCOMPARE(run(h, c3, {"--remove-port", "KAT500", "--config", file}), 0);
        QCOMPARE(fromJson(readFile(file), nullptr)->ports.size(), 0);
        // The managed state was never looked at: no mode entry, no Platform, no Applier.
        QVERIFY(!h.calls.contains("platform") && !h.calls.contains("guard"));
        QVERIFY(r.world.log.all().isEmpty());
        QVERIFY(!r.has(kPending));
#endif
    }
    void scratchStatusReadsBesideTheConfig()
    {
#ifdef _WIN32
        QSKIP("scratch-folder cases use the POSIX adapters (the Windows scratch path is covered by tst_CommandLine)");
#else
        Rig r;
        FakeCliHost h(r);
        const QString scratch = h.tmp.filePath("scratch");
        QVERIFY(QDir().mkpath(scratch));
        ::chmod(QFile::encodeName(scratch).constData(), 0700);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--status", "--config", scratch + "/c.json"}), 0);
        QVERIFY(c.outText().contains("No status reported yet."));
        QVERIFY(!h.calls.contains("platform"));
#endif
    }

    // A scratch config named status.json would be replaced by the run's first status publication; it is
    // refused before anything runs, and its bytes stay as they were.
    void runRefusesStatusFileAsConfig()
    {
        Rig r;
        FakeCliHost h(r);
        const QString scratch = h.tmp.filePath("scratch");
        QVERIFY(QDir().mkpath(scratch));
        const QByteArray cfg = R"({"version":1,"ports":[{"name":"K","comPort":"/dev/ttyUSB0","tcpPort":4001,"bindAddress":"127.0.0.1"}]})";
        QVERIFY(writeFile(scratch + "/status.json", cfg));
        h.own = h.tmp.filePath("copy-exe");
        for (const QStringList& args : {QStringList{"--run", "--config", scratch + "/status.json"},
                                        QStringList{"--add-port", "X", "/dev/ttyUSB1", "4002", "--config", scratch + "/status.json"}}) {
            CapturingConsole c;
            QCOMPARE(run(h, c, args), 2);
            QVERIFY2(c.errText().contains("where the diagnostic run writes its status"), qPrintable(c.errText()));
        }
        QCOMPARE(readFile(scratch + "/status.json"), cfg);   // the input bytes are unchanged
        QVERIFY(h.calls.isEmpty());                            // refused before mode entry and before any folder
    }

    void helpAndUsage()
    {
        Rig r;
        FakeCliHost h(r);
        CapturingConsole c;
        QCOMPARE(run(h, c, {"--help"}), 0);
        QVERIFY(c.outText().contains("--install [--force-recreate]"));
        QVERIFY(c.outText().contains("Exit codes: 0 done, 1 failed, 2 usage error, 3 the data folder is not trusted."));
        CapturingConsole c2;
        QCOMPARE(run(h, c2, {"--install", "--stop"}), 2);
        QVERIFY(c2.errText().contains("Run --help"));
        QCOMPARE(run(h, c2, {"--list-ports"}), 0);
        QVERIFY(h.calls.contains("listPorts"));
    }
};

QTEST_GUILESS_MAIN(tst_Commands)
#include "tst_Commands.moc"
