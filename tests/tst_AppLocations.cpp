// AppLocations and CommandLine::takeConfigOption.

#include "core/AppLocations.h"

#include <QDir>
#include <QFileInfo>
#include <QTest>

#include <functional>
#include <optional>

using namespace asb;

class tst_AppLocations : public QObject {
    Q_OBJECT

private slots:
    // Kills: Linux falling back to the Windows ProgramData layout, or status landing in /etc.
    void linuxUsesEtcForConfigAndRunForStatus()
    {
        const std::optional<AppLocations> linuxLoc = AppLocations::forOs(false);
        QVERIFY(linuxLoc);   // Linux has no lookup to fail
        const AppLocations& linux = *linuxLoc;
        QCOMPARE(linux.configFile, QStringLiteral("/etc/aether-serial-bridge/config.json"));
        QCOMPARE(linux.statusFile, QStringLiteral("/run/aether-serial-bridge/status.json"));
        QCOMPARE(linux.logFile, QStringLiteral("/run/aether-serial-bridge/bridge.log"));
        QCOMPARE(linux.dataDirectory(), QStringLiteral("/etc/aether-serial-bridge"));
    }

    // Kills: Windows losing its shared ProgramData folder (the GUI and service meet there).
    void windowsKeepsConfigStatusAndLogTogether()
    {
        const std::optional<AppLocations> winLoc = AppLocations::forOs(true);
        QVERIFY(winLoc);
        const AppLocations& win = *winLoc;
        QCOMPARE(QFileInfo(win.dataDirectory()).fileName(), QStringLiteral("AetherSerialBridge"));
        QCOMPARE(QFileInfo(win.statusFile).path(), win.dataDirectory());
        QCOMPARE(QFileInfo(win.logFile).path(), win.dataDirectory());
        QCOMPARE(QFileInfo(win.configFile).fileName(), QStringLiteral("config.json"));
        QCOMPARE(QFileInfo(win.dataDirectory()).path(), AppLocations::windowsProgramData());
    }

    // Kills: a failed ProgramData lookup falling back to the bare prepend, "/AetherSerialBridge", which
    // Windows roots on the current drive. The failure must be explicit (nullopt), never a path.
    void failedProgramDataLookupFailsClosed()
    {
        QVERIFY(!AppLocations::forOs(true, [] { return QString(); }));
        QVERIFY(!AppLocations::forOs(true, std::function<QString()>()));   // no lookup at all
        // A result that is not a drive-letter or UNC path is refused too: rooted on the current drive,
        // relative, or a bare drive.
        for (const char* bad : {"/ProgramData", "ProgramData", "C:ProgramData", "C:", "//", "///x", "1:/x", "\\\\h\\s"})
            QVERIFY2(!AppLocations::forOs(true, [bad] { return QString::fromLatin1(bad); }), bad);
        // The good shapes still work, and the lookup is the only input.
        const auto drive = AppLocations::forOs(true, [] { return QStringLiteral("D:/Data"); });
        QVERIFY(drive);
        QCOMPARE(drive->configFile, QStringLiteral("D:/Data/AetherSerialBridge/config.json"));
        QCOMPARE(drive->statusFile, QStringLiteral("D:/Data/AetherSerialBridge/status.json"));
        QCOMPARE(drive->logFile, QStringLiteral("D:/Data/AetherSerialBridge/bridge.log"));
        const auto unc = AppLocations::forOs(true, [] { return QStringLiteral("//srv/share"); });
        QVERIFY(unc);
        QCOMPARE(unc->dataDirectory(), QStringLiteral("//srv/share/AetherSerialBridge"));
        // Linux never consults the lookup, so it cannot fail.
        QVERIFY(AppLocations::forOs(false, [] { return QString(); }));
    }

    void windowsProgramDataIsTheKnownFolder()
    {
#ifdef Q_OS_WIN
        // From SHGetKnownFolderPath, not the (caller-controlled) environment.
        const QString dir = AppLocations::windowsProgramData();
        QVERIFY(!dir.isEmpty());
        QVERIFY(QDir::isAbsolutePath(dir));
        QVERIFY(QFileInfo(dir).isDir());
        qputenv("ProgramData", "C:\\planted");
        QCOMPARE(AppLocations::windowsProgramData(), dir);
        QCOMPARE(AppLocations::forThisOs(), AppLocations::forOs(true));
#else
        QCOMPARE(AppLocations::forThisOs(), AppLocations::forOs(false));
#endif
    }

    // Kills: --config moving only the config, so a test run still writes /run or ProgramData.
    void configOverrideMovesStatusBesideIt()
    {
        const QString dir = QDir::cleanPath(QDir::tempPath() + QStringLiteral("/asb-paths"));
        const AppLocations loc = AppLocations::beside(dir + QStringLiteral("/my.json"));
        QCOMPARE(loc.configFile, dir + QStringLiteral("/my.json"));
        QCOMPARE(loc.statusFile, dir + QStringLiteral("/status.json"));
        QCOMPARE(loc.logFile, dir + QStringLiteral("/bridge.log"));
    }

    void configOverrideIsMadeAbsolute()
    {
        const AppLocations loc = AppLocations::beside(QStringLiteral("sub/../x.json"));
        QCOMPARE(loc.configFile, QDir::cleanPath(QDir::currentPath() + QStringLiteral("/x.json")));
        QVERIFY(QDir::isAbsolutePath(loc.statusFile));
    }

    // Kills: --config only recognised in first position, or its value left behind as a command.
    void configOptionIsTakenFromAnywhere()
    {
        auto r = CommandLine::takeConfigOption({QStringLiteral("--run"), QStringLiteral("--config"), QStringLiteral("/tmp/x.json")});
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.configFile.value_or(QString()), QStringLiteral("/tmp/x.json"));
        QCOMPARE(r.rest, QStringList{QStringLiteral("--run")});

        r = CommandLine::takeConfigOption({QStringLiteral("--config"), QStringLiteral("a.json"), QStringLiteral("--add-port"),
                                           QStringLiteral("KAT500")});
        QCOMPARE(r.configFile.value_or(QString()), QStringLiteral("a.json"));
        QCOMPARE(r.rest, (QStringList{QStringLiteral("--add-port"), QStringLiteral("KAT500")}));

        r = CommandLine::takeConfigOption({QStringLiteral("--status")});
        QVERIFY(!r.configFile);
        QCOMPARE(r.rest, QStringList{QStringLiteral("--status")});

        r = CommandLine::takeConfigOption({QStringLiteral("--CONFIG"), QStringLiteral("b.json")});   // case-insensitive
        QCOMPARE(r.configFile.value_or(QString()), QStringLiteral("b.json"));
        QVERIFY(r.rest.isEmpty());
    }

    void configOptionWithoutAPathIsRejected()
    {
        QCOMPARE(CommandLine::takeConfigOption({QStringLiteral("--run"), QStringLiteral("--config")}).error,
                 QStringLiteral("--config needs a file path."));
        QCOMPARE(CommandLine::takeConfigOption({QStringLiteral("--config"), QStringLiteral("--run")}).error,
                 QStringLiteral("--config needs a file path."));
    }
};

QTEST_GUILESS_MAIN(tst_AppLocations)
#include "tst_AppLocations.moc"
