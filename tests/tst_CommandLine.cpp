// The command line (master §4.10, WS-9; INV-26, INV-90, INV-101): the parser's rules for both executables, the
// scratch-config rules (never the managed config.json, never inside the data folder, judged by file identity), the
// installed-image refusal of --run (a hard link is refused, a copy runs) and the loopback-only binding of a scratch
// run, asserted on the ACTUAL bound endpoints (QTcpServer::serverAddress and the kernel's socket table).

#include "app/Dispatch.h"
#include "app/ScratchEngine.h"
#include "app/WindowProgram.h"
#include "cli/CommandLine.h"
#include "cli/ScratchRun.h"
#ifdef __linux__
#include "cli/LinuxHost.h"
#endif
#include "support/EngineTestSupport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace asb;
using namespace asb::cli;

namespace {

Parsed p(const QStringList& args)
{
    return parse(args);
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

quint16 freePort(const QHostAddress& on)
{
    QTcpServer s;
    if (!s.listen(on, 0)) return 0;
    const quint16 port = s.serverPort();
    s.close();
    return port;
}

#ifdef __linux__
// The local addresses the kernel lists as LISTEN for `port` (/proc/net/tcp and /proc/net/tcp6), as QHostAddress.
QList<QHostAddress> kernelListeners(quint16 port)
{
    QList<QHostAddress> out;
    for (const char* table : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        QFile f(QString::fromLatin1(table));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QList<QByteArray> lines = f.readAll().split('\n');
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QList<QByteArray> cols = lines[i].simplified().split(' ');
            if (cols.size() < 4 || cols[3] != "0A") continue;   // 0A = LISTEN
            const QList<QByteArray> local = cols[1].split(':');
            if (local.size() != 2 || local[1].toUInt(nullptr, 16) != port) continue;
            const QByteArray hex = local[0];
            if (hex.size() == 8) {
                const quint32 le = hex.toUInt(nullptr, 16);   // network order bytes printed as a host-order word
                out << QHostAddress(qFromBigEndian(qToLittleEndian(le)));
            } else if (hex.size() == 32) {
                Q_IPV6ADDR a;
                for (int w = 0; w < 4; ++w) {   // four 32-bit words, each printed in host (little-endian) order
                    const quint32 word = hex.mid(w * 8, 8).toUInt(nullptr, 16);
                    for (int b = 0; b < 4; ++b) a[w * 4 + b] = quint8((word >> (8 * b)) & 0xff);
                }
                out << QHostAddress(a);
            }
        }
    }
    return out;
}
#endif

}  // namespace

class tst_CommandLine : public QObject {
    Q_OBJECT

private slots:
    // ---- syntax --------------------------------------------------------------------------------
    void eachCommandAlone_data()
    {
        QTest::addColumn<QString>("arg");
        QTest::addColumn<int>("command");
        const QList<QPair<QString, Command>> rows = {
            {"--install", Command::Install},   {"--apply", Command::Apply},         {"--start", Command::Start},
            {"--stop", Command::Stop},         {"--uninstall", Command::Uninstall}, {"--repair", Command::Repair},
            {"--status", Command::Status},     {"--show-config", Command::ShowConfig},
            {"--list-ports", Command::ListPorts}, {"--discard-pending", Command::DiscardPending},
            {"--service", Command::Service},   {"--help", Command::Help},           {"-h", Command::Help},
            {"/?", Command::Help},             {"--version", Command::Version},     {"--INSTALL", Command::Install},
        };
        for (const auto& r : rows) QTest::newRow(qPrintable(r.first)) << r.first << int(r.second);
    }
    void eachCommandAlone()
    {
        QFETCH(QString, arg);
        QFETCH(int, command);
        const Parsed r = p({arg});
        QVERIFY2(r.ok(), qPrintable(r.usageError));
        QCOMPARE(int(r.command), command);
    }

    // INV-26 (spec mutant "--config with --apply"): the service commands always use the installed config.json.
    void configRefusedWithServiceCommands_data()
    {
        QTest::addColumn<QString>("command");
        for (const char* c : {"--install", "--apply", "--start", "--stop", "--uninstall", "--repair", "--discard-pending",
                              "--list-ports", "--service", "--help", "--version"})
            QTest::newRow(c) << QString::fromLatin1(c);
    }
    void configRefusedWithServiceCommands()
    {
        QFETCH(QString, command);
        for (const QStringList& args : {QStringList{command, "--config", "/tmp/x.json"},
                                        QStringList{"--config", "/tmp/x.json", command}}) {
            const Parsed r = p(args);
            QVERIFY2(!r.ok(), qPrintable(args.join(' ')));
            QCOMPARE(r.command, Command::None);
        }
        const Parsed r = p({command, "--config", "/tmp/x.json"});
        if (command == QLatin1String("--apply"))
            QVERIFY2(r.usageError.contains("--config cannot be used with --apply"), qPrintable(r.usageError));
    }
    void configAllowedWithRunStatusShowAndEdits()
    {
        for (const QStringList& args : {QStringList{"--run", "--config", "a.json"}, QStringList{"--status", "--config", "a.json"},
                                        QStringList{"--config", "a.json", "--show-config"},
                                        QStringList{"--add-port", "KAT", "/dev/ttyUSB0", "4001", "--config", "a.json"},
                                        QStringList{"--remove-port", "KAT", "--config", "a.json"},
                                        QStringList{"--set-enabled", "KAT", "off", "--config", "a.json"}}) {
            const Parsed r = p(args);
            QVERIFY2(r.ok(), qPrintable(args.join(' ') + ": " + r.usageError));
            QCOMPARE(r.config.value_or(QString()), QStringLiteral("a.json"));
        }
    }
    // INV-90: --run never runs the installed config; it needs a scratch one.
    void runRequiresConfig()
    {
        const Parsed r = p({"--run"});
        QVERIFY(!r.ok());
        QVERIFY2(r.usageError.contains("--run needs --config"), qPrintable(r.usageError));
    }
    // INV-21 routing (WS-5 deferred): --force-recreate only with --install/--apply, --yes only with --repair.
    void forceRecreateAndYesRouting()
    {
        QVERIFY(p({"--install", "--force-recreate"}).forceRecreate);
        QVERIFY(p({"--force-recreate", "--apply"}).forceRecreate);
        QVERIFY(p({"--repair", "--yes"}).yes);
        for (const char* c : {"--start", "--stop", "--uninstall", "--repair", "--status"})
            QVERIFY2(!p({QString::fromLatin1(c), "--force-recreate"}).ok(), c);
        for (const char* c : {"--install", "--apply", "--stop"}) QVERIFY2(!p({QString::fromLatin1(c), "--yes"}).ok(), c);
        QVERIFY(!p({"--force-recreate"}).ok());
    }
    void conflictsAreUsageErrors()
    {
        const Parsed r = p({"--install", "--apply"});
        QVERIFY(!r.ok());
        QVERIFY2(r.usageError.contains("--install and --apply cannot be combined"), qPrintable(r.usageError));
        QVERIFY(!p({"--status", "--show-config"}).ok());
        QVERIFY(!p({"--run", "--config", "a", "--service"}).ok());
        QVERIFY(!p({"--help", "--install"}).ok());
    }
    void duplicatesAreUsageErrors()
    {
        for (const QStringList& args : {QStringList{"--run", "--config", "a", "--config", "b"},
                                        QStringList{"--install", "--force-recreate", "--force-recreate"},
                                        QStringList{"--repair", "--yes", "--yes"},
                                        QStringList{"--stop", "--stop"},
                                        QStringList{"--add-port", "K", "d", "1", "--baud", "9600", "--baud", "4800"},
                                        QStringList{"--add-port", "K", "d", "1", "--bind", "::1", "--BIND", "::1"}}) {
            const Parsed r = p(args);
            QVERIFY2(!r.ok(), qPrintable(args.join(' ')));
            QVERIFY2(r.usageError.contains("twice"), qPrintable(r.usageError));
        }
    }
    void unknownAndStray()
    {
        QVERIFY(p({"--bogus"}).usageError.contains("unknown option '--bogus'"));
        QVERIFY(p({"install"}).usageError.contains("unexpected argument 'install'"));
        QVERIFY(p({"--status", "x"}).usageError.contains("unexpected argument 'x'"));
        QVERIFY(!p({"--config", "a.json"}).ok());
        QVERIFY(p({}).ok());   // nothing: the executable decides (GUI, TUI or help)
    }
    // A value never starts with "--".
    void valuesNeverStartWithDashes()
    {
        QVERIFY(p({"--remove-port", "--stop"}).usageError.contains("--remove-port needs a port name"));
        QVERIFY(p({"--run", "--config", "--status"}).usageError.contains("--config needs a file path"));
        QVERIFY(p({"--add-port", "K", "/dev/ttyUSB0"}).usageError.contains("a TCP port"));
        QVERIFY(p({"--add-port", "K", "/dev/ttyUSB0", "40x1"}).usageError.contains("'40x1' is not a valid TCP port"));
        QVERIFY(p({"--add-port", "K", "/dev/ttyUSB0", "-1"}).usageError.contains("not a valid TCP port"));
        QVERIFY(p({"--add-port", "K", "d", "4001", "--baud", "+9600"}).usageError.contains("not a valid baud rate"));
    }
    void addPortValues()
    {
        const Parsed r = p({"--add-port", "KAT500", "/dev/serial/by-id/usb-x", "4001", "--baud", "9600", "--format",
                            "7e2", "--bind", "192.168.1.5", "--allow", "10.0.0.0/8"});
        QVERIFY2(r.ok(), qPrintable(r.usageError));
        QCOMPARE(r.addPort.name, QStringLiteral("KAT500"));
        QCOMPARE(r.addPort.device, QStringLiteral("/dev/serial/by-id/usb-x"));
        QCOMPARE(r.addPort.tcpPort, 4001);
        QCOMPARE(r.addPort.baud, 9600);
        QCOMPARE(r.addPort.format, QStringLiteral("7e2"));
        QCOMPARE(r.addPort.bind.value_or(QString()), QStringLiteral("192.168.1.5"));
        QCOMPARE(r.addPort.allow, QStringLiteral("10.0.0.0/8"));
        QVERIFY(!p({"--add-port", "K", "d", "1"}).addPort.bind.has_value());
        QVERIFY(!p({"--status", "--baud", "9600"}).ok());
    }
    void setEnabledValues()
    {
        for (const char* on : {"on", "ON", "true", "yes", "1"}) QVERIFY(p({"--set-enabled", "K", QString::fromLatin1(on)}).enable);
        for (const char* off : {"off", "false", "no", "0"}) {
            const Parsed r = p({"--set-enabled", "K", QString::fromLatin1(off)});
            QVERIFY(r.ok());
            QVERIFY(!r.enable);
        }
        QVERIFY(p({"--set-enabled", "K", "maybe"}).usageError.contains("expected on or off"));
    }
    // Both executables (INV-26 "both exes"): the same parser, and the GUI executable runs only the read-only views.
    void bothExecutables()
    {
        using namespace asb::app;
        DispatchEnv e;
        for (BuildKind b : {BuildKind::Headless, BuildKind::Gui}) {
            QCOMPARE(decide(b, e, {"--apply", "--config", "x.json"}).exitCode, 2);
            QCOMPARE(decide(b, e, {"--install", "--apply"}).exitCode, 2);
            QCOMPARE(decide(b, e, {"--run"}).exitCode, 2);
        }
        QCOMPARE(decide(BuildKind::Headless, e, {"--run", "--config", "x.json"}).action, Action::Command);
        QCOMPARE(decide(BuildKind::Gui, e, {"--run", "--config", "x.json"}).exitCode, 2);
    }

    // ---- scratch configs (INV-26, INV-90) --------------------------------------------------------
    void runRefusesManagedConfig()
    {
        QTemporaryDir t;
        QVERIFY(t.isValid());
        const QString data = t.filePath("data");
        QVERIFY(QDir().mkpath(data));
        const QString managed = data + "/config.json";
        QVERIFY(writeFile(managed, "{}"));
        // The managed file itself, by any spelling of its path.
        QVERIFY(!ScratchRun::refuseManagedPath(managed, managed, data).isEmpty());
        QVERIFY(!ScratchRun::refuseManagedPath(data + "/../data/./config.json", managed, data).isEmpty());
        // A copy elsewhere is a scratch config.
        QVERIFY(QDir().mkpath(t.filePath("scratch")));
        QVERIFY(writeFile(t.filePath("scratch/config.json"), "{}"));
        QCOMPARE(ScratchRun::refuseManagedPath(t.filePath("scratch/config.json"), managed, data), QString());
#ifndef _WIN32
        // A hard link to the managed file elsewhere: same identity, refused.
        QVERIFY(::link(QFile::encodeName(managed).constData(), QFile::encodeName(t.filePath("scratch/linked.json")).constData()) == 0);
        const QString why = ScratchRun::refuseManagedPath(t.filePath("scratch/linked.json"), managed, data);
        QVERIFY2(why.contains("is the installed config.json"), qPrintable(why));
#endif
    }
    void runRefusesPathInsideDataFolder()
    {
        QTemporaryDir t;
        const QString data = t.filePath("data");
        QVERIFY(QDir().mkpath(data + "/sub"));
        const QString managed = data + "/config.json";
        QVERIFY(!ScratchRun::refuseManagedPath(data + "/sub/x.json", managed, data).isEmpty());
        QVERIFY(!ScratchRun::refuseManagedPath(data + "/other.json", managed, data).isEmpty());
        // A data folder that does not exist yet is still refused by its path.
        QVERIFY(!ScratchRun::refuseManagedPath(t.filePath("missing/x.json"), t.filePath("missing/config.json"), t.filePath("missing")).isEmpty());
        // A sibling whose name only starts like the data folder is not inside it.
        QVERIFY(QDir().mkpath(t.filePath("data2")));
        QCOMPARE(ScratchRun::refuseManagedPath(t.filePath("data2/x.json"), managed, data), QString());
#ifndef _WIN32
        // Through a symbolic link to the data folder, or to a folder inside it: refused by identity.
        QVERIFY(QFile::link(data, t.filePath("alias")));
        QVERIFY(!ScratchRun::refuseManagedPath(t.filePath("alias/x.json"), managed, data).isEmpty());
        QVERIFY(QFile::link(data + "/sub", t.filePath("subalias")));
        QVERIFY(!ScratchRun::refuseManagedPath(t.filePath("subalias/x.json"), managed, data).isEmpty());
#endif
    }
    // INV-101 (spec mutant "compare paths by string"): the identity seam; a hard link is refused, a copy runs.
    void runRefusesInstalledImage()
    {
        QTemporaryDir t;
        const QString installed = t.filePath("installed-exe");
        QVERIFY(writeFile(installed, "program"));
        QVERIFY(!ScratchRun::refuseInstalledImage(installed, installed).isEmpty());
        QVERIFY(QFile::copy(installed, t.filePath("copy-exe")));
        QCOMPARE(ScratchRun::refuseInstalledImage(t.filePath("copy-exe"), installed), QString());
        // Nothing installed: nothing to refuse.
        QCOMPARE(ScratchRun::refuseInstalledImage(t.filePath("copy-exe"), t.filePath("absent")), QString());
        // The identity seam: a hard link (another path, the same file) is the installed program.
        int calls = 0;
        const ScratchRun::IdentityOf sameFile = [&](const QString&) {
            ++calls;
            return std::optional<ScratchRun::FileKey>(ScratchRun::FileKey{1, 42});
        };
        QVERIFY(!ScratchRun::refuseInstalledImage(t.filePath("hardlink-exe"), installed, sameFile).isEmpty());
        QVERIFY(calls >= 2);
#ifndef _WIN32
        QVERIFY(::link(QFile::encodeName(installed).constData(), QFile::encodeName(t.filePath("hardlink-exe")).constData()) == 0);
        const QString why = ScratchRun::refuseInstalledImage(t.filePath("hardlink-exe"), installed);
        QVERIFY2(why.contains("installed service program"), qPrintable(why));
#endif
    }

    // A scratch config may not be the run's status.json (by name; ASCII case-insensitively on Windows;
    // by identity).
    void statusFileNameRefused()
    {
        QTemporaryDir t;
        QVERIFY(!ScratchRun::refuseStatusFileName(t.filePath("status.json"), false).isEmpty());
        QVERIFY(!ScratchRun::refuseStatusFileName(t.filePath("STATUS.JSON"), true).isEmpty());
        QCOMPARE(ScratchRun::refuseStatusFileName(t.filePath("config.json"), false), QString());
        QCOMPARE(ScratchRun::refuseStatusFileName(t.filePath("status.json.bak"), true), QString());
        // Another name for the same file (a hard link, a Windows short name): refused by identity.
        const ScratchRun::IdentityOf same = [](const QString&) { return std::optional<ScratchRun::FileKey>(ScratchRun::FileKey{3, 9}); };
        QVERIFY(!ScratchRun::refuseStatusFileName(t.filePath("STATUS~1.JSO"), false, same).isEmpty());
    }

    // ---- loopback only (INV-101, v6/N2) -------------------------------------------------------------
    void runForcesLoopback_data()
    {
        QTest::addColumn<QString>("bind");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<bool>("warned");
        QTest::newRow("0.0.0.0") << "0.0.0.0" << "127.0.0.1" << true;
        QTest::newRow("::") << "::" << "::1" << true;
        QTest::newRow("2001:db8::5") << "2001:db8::5" << "::1" << true;
        QTest::newRow("::ffff:10.0.0.5") << "::ffff:10.0.0.5" << "127.0.0.1" << true;
        QTest::newRow("127.0.0.1") << "127.0.0.1" << "127.0.0.1" << false;
        QTest::newRow("::1") << "::1" << "::1" << false;
    }
    void runForcesLoopback()
    {
        QFETCH(QString, bind);
        QFETCH(QString, expected);
        QFETCH(bool, warned);
        const QHostAddress want(expected);
        const quint16 port = freePort(want);
        if (port == 0) QSKIP(qPrintable(QStringLiteral("%1 cannot be bound on this host").arg(expected)));
        BridgeConfig c;
        PortMapping m;
        m.name = QStringLiteral("scratch");
        m.comPort = QStringLiteral("/dev/null-not-opened");
        m.tcpPort = port;
        m.bindAddress = bind;
        c.ports << m;
        QVERIFY2(c.validate().isEmpty(), qPrintable(c.validate().join(' ')));
        QStringList warnings;
        const BridgeConfig loop = ScratchRun::forceLoopback(c, &warnings);
        QCOMPARE(loop.ports[0].bindAddress, expected);
        QCOMPARE(!warnings.isEmpty(), warned);
        if (warned) QVERIFY2(warnings[0].contains(expected) && warnings[0].contains(bind), qPrintable(warnings[0]));

        asb::test::RecordingSideChannel side;
        app::ScratchEngine engine(side);
        engine.start(loop);
        const QList<app::ScratchEngine::Endpoint> eps = engine.endpoints();
        QCOMPARE(eps.size(), 1);
        QVERIFY2(eps[0].listening, qPrintable(eps[0].error));
        QCOMPARE(eps[0].address, want);
        QVERIFY(eps[0].address != QHostAddress(QHostAddress::Any) && eps[0].address != QHostAddress(QHostAddress::AnyIPv6));
        QCOMPARE(eps[0].port, port);
#ifdef __linux__
        const QList<QHostAddress> table = kernelListeners(port);
        QVERIFY2(!table.isEmpty(), "the kernel lists no listener on the port");
        for (const QHostAddress& a : table) QVERIFY2(a.isEqual(want, QHostAddress::StrictConversion), qPrintable(a.toString()));
#endif
        bool stopped = false;
        engine.stop([&] { stopped = true; });
        QTRY_VERIFY(stopped);
    }

    // v1.1 WS-14 §2: the window's program is the one its own AppImage embeds; an APPDIR that is not this image's (one
    // inherited from another AppImage) is ignored, and outside an image it is the program beside the window.
    void windowProgramPathFollowsOwnImage()
    {
#ifdef _WIN32
        QCOMPARE(app::windowProgramPath(QStringLiteral("C:/x/AetherSerialBridge.exe"), QString()),
                 QStringLiteral("C:/x/aether-serial-bridge.exe"));
#else
        const QString gui = QStringLiteral("/tmp/.mount_abc/usr/bin/aether-serial-bridge-gui");
        QCOMPARE(app::windowProgramPath(gui, QStringLiteral("/tmp/.mount_abc")),
                 QStringLiteral("/tmp/.mount_abc/usr/libexec/aether-serial-bridge"));
        QCOMPARE(app::windowProgramPath(gui, QStringLiteral("/tmp/.mount_abc/")),
                 QStringLiteral("/tmp/.mount_abc/usr/libexec/aether-serial-bridge"));
        QCOMPARE(app::windowProgramPath(gui, QStringLiteral("/tmp/.mount_other")),
                 QStringLiteral("/tmp/.mount_abc/usr/bin/aether-serial-bridge"));
        QCOMPARE(app::windowProgramPath(QStringLiteral("/src/build/bin/aether-serial-bridge-gui"), QString()),
                 QStringLiteral("/src/build/bin/aether-serial-bridge"));
        QCOMPARE(app::windowProgramPath(QStringLiteral("/src/build/bin/aether-serial-bridge-gui"), QStringLiteral("/")),
                 QStringLiteral("/src/build/bin/aether-serial-bridge"));
#endif
    }

    // The probe hashes its file once and judges that digest against whatever digest each call passes
    // (not the first answer, whatever digest came later).
    void windowProgramProbeJudgesEveryCall()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("aether-serial-bridge"));
        const QByteArray bytes = "the program this window was built with";
        const auto hex = [](const QByteArray& b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex(); };
        auto write = [&path](const QByteArray& b) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(f.write(b), qint64(b.size()));
        };
        write(bytes);
        const QByteArray other = hex("another build");
        {
            app::WindowProgramProbe probe(path);
            app::WindowProgram w = probe.check(other);   // a wrong digest first...
            QVERIFY(!w.digestOk);
            QVERIFY2(w.problem.contains(QStringLiteral("this window expects")), qPrintable(w.problem));
            QCOMPARE(w.sha256.toLatin1(), hex(bytes));
            w = probe.check(hex(bytes));                  // ...does not stick to the right one
            QVERIFY2(w.digestOk, qPrintable(w.problem));
            QVERIFY(w.problem.isEmpty());
            QVERIFY(!probe.check(QByteArray()).digestOk);   // an unknown digest is never this build
            QVERIFY(probe.check(hex(bytes).toUpper()).digestOk);
            // Hashed once: a later change of the file is not read again.
            write("replaced bytes");
            QCOMPARE(probe.check(hex(bytes)).sha256.toLatin1(), hex(bytes));
        }
        {
            app::WindowProgramProbe probe(path);
            QVERIFY(!probe.check(hex(bytes)).digestOk);         // a fresh probe reads the file as it is now
            QVERIFY(probe.check(hex("replaced bytes")).digestOk);
        }
        app::WindowProgramProbe missing(dir.filePath(QStringLiteral("absent")));
        const app::WindowProgram w = missing.check(hex(bytes));
        QVERIFY(!w.digestOk);
        QVERIFY(w.sha256.isEmpty());
        QVERIFY2(w.problem.contains(QStringLiteral("cannot be read")), qPrintable(w.problem));
    }

    // v1.1 D61, INV-120: every production LinuxHost passes this build's program digest to detect(). The headless
    // program (CLI, TUI, the pkexec helper) hashes its own image, so the installed helper classifies its own
    // installation as Current; the window passes the digest compiled into it. thisBuild names it, as on Windows.
    void linuxHostPassesItsProgramDigest()
    {
#ifndef __linux__
        QSKIP("LinuxHost is the Linux host");
#else
        QFile self(QStringLiteral("/proc/self/exe"));
        QVERIFY(self.open(QIODevice::ReadOnly));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QVERIFY(hash.addData(&self));
        const QByteArray mine = hash.result().toHex();
        char arg0[] = "tst_CommandLine";
        char* argv[] = {arg0, nullptr};
        LinuxHost headless(1, argv);
        QString error;
        int code = 0;
        QVERIFY2(headless.platform(&error, &code), qPrintable(error));
        QCOMPARE(headless.linuxPlatform()->service().options().expectedSha256, mine);
        QCOMPARE(headless.programSha256(), mine);
        QVERIFY2(headless.thisBuild().endsWith(QStringLiteral(", program sha256 ") + QString::fromLatin1(mine.left(12))),
                 qPrintable(headless.thisBuild()));
        const QByteArray compiled = QByteArray("ABCDEF0123456789").repeated(4);
        LinuxHost window(1, argv, compiled);
        QVERIFY2(window.platform(&error, &code), qPrintable(error));
        QCOMPARE(window.linuxPlatform()->service().options().expectedSha256, compiled.toLower());
#endif
    }
};

QTEST_GUILESS_MAIN(tst_CommandLine)
#include "tst_CommandLine.moc"
