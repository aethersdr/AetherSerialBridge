// The Linux window's routing (v1.1 WS-15 §3, INV-122, INV-123): every window action to its op and route over a fake
// process. Install, Update and Uninstall run the window's own program on the staged route, read and hashed on a worker thread and
// launched only when its bytes are this window's build; day-to-day actions use the installed helper with v1.0 ops;
// Discard sends the digest-checked op only to an installed program that carries a build stamp (v1.1 or later). The real pkexec, polkit and
// staging script are the systemd suite's bootstrap_* cases.

#include "cli/StagedHelper.h"
#include "core/ConfigJson.h"
#include "gui/HelperActions.h"

#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

using namespace asb;
using namespace asb::cli;
using namespace asb::gui;

namespace {

class FakeProcess final : public IHelperProcess {
public:
    struct Launch {
        bool staged = false;
        QByteArray expectedSha256, program, request;
    };
    QList<Launch> launches;
    std::optional<Exit> next;   // finish every launch with this (posted); nullopt: an empty response, exit 0

    void run(const QByteArray& request, std::function<void(const Exit&)> done) override
    {
        launches << Launch{false, {}, {}, request};
        finishLater(std::move(done));
    }
    void runStaged(const QByteArray& expectedSha256, const QByteArray& program, const QByteArray& request,
                   std::function<void(const Exit&)> done) override
    {
        launches << Launch{true, expectedSha256, program, request};
        finishLater(std::move(done));
    }

private:
    void finishLater(std::function<void(const Exit&)> done)
    {
        Exit e;
        if (next) {
            e = *next;
        } else {
            Result r;
            r.outcome = Outcome::Ok;
            r.message = QStringLiteral("ok");
            e.code = 0;
            e.out = helper::encodeResult(r);
        }
        QTimer::singleShot(0, [done, e] { done(e); });
    }
};

class Observer final : public IOperationObserver {
public:
    void interim(const Interim&) override {}
    void finished(const Result& r) override
    {
        results << r;
        if (onFinished) onFinished();
    }
    QList<Result> results;
    std::function<void()> onFinished;   // runs inside finished(), as a window refreshing its buttons would
};

const QByteArray kProgram("\x7f" "ELF the window's program");
const QString kProgramPath = QStringLiteral("/tmp/.mount_x/usr/libexec/aether-serial-bridge");

struct Setup {
    FakeProcess process;
    HelperClient client{process};
    std::optional<QString> installedVersion;
    QByteArray bytes = kProgram;
    QString problem;
    QString appImage;
    bool noReader = false;
    std::function<QByteArray(const QByteArray&)> hash;
    std::atomic<int> reads{0};
    std::unique_ptr<HelperActions> actions;

    void make()
    {
        HelperActions::Staged s;
        s.expectedSha256 = sha256Hex(kProgram).toLatin1();
        if (!noReader)
            s.read = [this] {
                ++reads;
                // A fresh buffer per read, as a file read gives: the buffer sent can be told from a second read.
                return HelperActions::ProgramFile{kProgramPath, QByteArray(bytes.constData(), bytes.size()), problem};
            };
        s.appImage = appImage;
        s.hash = hash;
        actions = std::make_unique<HelperActions>(client, s, [this] { return RouteFacts{installedVersion}; });
    }
};

SaveRequest editorPorts()
{
    return SaveRequest{QByteArray("{\"version\":1,\"ports\":[]}\n"), Source{Source::New, {}}, true};
}

}  // namespace

class tst_HelperActions : public QObject {
    Q_OBJECT

private slots:
    // Every row of §3 over a fake client: the op on the wire and the route it took.
    void routing_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("version");   // "<unread>" = nullopt
        QTest::addColumn<QString>("op");
        QTest::addColumn<bool>("staged");
        QTest::newRow("install") << "install" << "<unread>" << "install" << true;
        QTest::newRow("install-with-ports") << "install-ports" << "<unread>" << "install" << true;
        QTest::newRow("save-not-current") << "run-install" << "<unread>" << "install" << true;
        QTest::newRow("update") << "update" << "" << "install" << true;
        QTest::newRow("save-update") << "run-install" << "" << "install" << true;
        QTest::newRow("save-current") << "run" << "1.1.0" << "save-and-apply" << false;
        QTest::newRow("save-keep-v10") << "run" << "" << "save-and-apply" << false;
        QTest::newRow("untick") << "untick" << "" << "deactivate" << false;
        QTest::newRow("start") << "start" << "" << "start" << false;
        QTest::newRow("stop") << "stop" << "" << "stop" << false;
        QTest::newRow("repair") << "repair" << "" << "repair" << false;
        QTest::newRow("discard-v10") << "discard" << "" << "discard-pending" << false;
        QTest::newRow("discard-unread") << "discard" << "<unread>" << "discard-pending" << false;
        QTest::newRow("discard-dev-stamp") << "discard" << "0.1.0" << "discard-pending-if" << false;   // a v1.1 dev build
        QTest::newRow("discard-v11") << "discard" << "1.1.0" << "discard-pending-if" << false;
        QTest::newRow("discard-newer") << "discard" << "1.2.3" << "discard-pending-if" << false;
        QTest::newRow("uninstall") << "uninstall" << "1.1.0" << "uninstall" << true;
        QTest::newRow("uninstall-v10") << "uninstall" << "" << "uninstall" << true;
        QTest::newRow("uninstall-remove-settings") << "uninstall-remove" << "1.1.0" << "uninstall" << true;
    }
    void routing()
    {
        QFETCH(QString, action);
        QFETCH(QString, version);
        QFETCH(QString, op);
        QFETCH(bool, staged);
        Setup s;
        if (version != QLatin1String("<unread>")) s.installedVersion = version;
        s.make();
        Observer o;
        HelperActions& a = *s.actions;
        const QString pendingSha = QString(64, QLatin1Char('e'));
        bool submitted = false;
        if (action == QLatin1String("install")) submitted = a.install(std::nullopt, o);
        else if (action == QLatin1String("install-ports")) submitted = a.install(editorPorts(), o);
        else if (action == QLatin1String("update")) submitted = a.update(std::nullopt, o);
        else if (action == QLatin1String("run-install")) submitted = a.run(editorPorts(), true, o);
        else if (action == QLatin1String("run")) submitted = a.run(editorPorts(), false, o);
        else if (action == QLatin1String("untick")) submitted = a.untick(editorPorts(), o);
        else if (action == QLatin1String("start")) submitted = a.start(o);
        else if (action == QLatin1String("stop")) submitted = a.stop(o);
        else if (action == QLatin1String("repair")) submitted = a.repair(o);
        else if (action == QLatin1String("discard")) submitted = a.discardPending(pendingSha, o);
        else if (action == QLatin1String("uninstall")) submitted = a.uninstall(UninstallOptions{false}, o);
        else if (action == QLatin1String("uninstall-remove")) submitted = a.uninstall(UninstallOptions{true}, o);
        QVERIFY(submitted);
        QVERIFY(a.busy());
        QTRY_COMPARE(o.results.size(), 1);
        QVERIFY(!a.busy());
        QCOMPARE(s.process.launches.size(), 1);
        const FakeProcess::Launch& l = s.process.launches[0];
        QCOMPARE(l.staged, staged);
        QString why;
        const std::optional<helper::Request> req = helper::parseRequest(l.request, &why);
        QVERIFY2(req, qPrintable(why));
        QCOMPARE(helper::opName(req->op), op);
        // The staged route sends this window's program, read on the worker, with the digest compiled into the window;
        // the installed route reads nothing.
        QCOMPARE(s.reads.load(), staged ? 1 : 0);
        if (staged) {
            QCOMPARE(l.program, kProgram);
            QCOMPARE(l.expectedSha256, sha256Hex(kProgram).toLatin1());
        }
        if (op == QLatin1String("discard-pending-if")) QCOMPARE(req->sha256, pendingSha);
        if (action == QLatin1String("uninstall-remove")) QVERIFY(req->removeSettings);
        if (action == QLatin1String("uninstall")) QVERIFY(!req->removeSettings);
        if (action == QLatin1String("install-ports") || action.startsWith(QLatin1String("run")) || action == QLatin1String("untick"))
            QVERIFY(req->save && req->save->config == editorPorts().config);
        if (action == QLatin1String("install") || action == QLatin1String("update")) QVERIFY(!req->save);
        // The shared routing function says the same (the systemd suite's client uses it).
        const RouteFacts f{s.installedVersion};
        const WindowAction wa = action == QLatin1String("discard") ? WindowAction::Discard
                                : action.startsWith(QLatin1String("uninstall")) ? WindowAction::Uninstall
                                : action == QLatin1String("start") ? WindowAction::Start
                                : action == QLatin1String("stop") ? WindowAction::Stop
                                : action == QLatin1String("repair") ? WindowAction::Repair
                                : action == QLatin1String("untick") ? WindowAction::Untick
                                : action == QLatin1String("run") ? WindowAction::Apply
                                : action == QLatin1String("update") ? WindowAction::Update
                                                                    : WindowAction::Install;
        QCOMPARE(helper::opName(routeFor(wa, f).op), op);
        QCOMPARE(routeFor(wa, f).route == Route::Staged, staged);
    }
    // INV-122: a program whose bytes are not this window's build is never handed to pkexec.
    void localDigestMismatchNeverLaunches()
    {
        for (const char* which : {"install", "uninstall"}) {
            Setup s;
            s.bytes = QByteArray("#!/bin/sh\ntouch /run/asb-ran\n");
            s.make();
            Observer o;
            QVERIFY(qstrcmp(which, "install") == 0 ? s.actions->install(editorPorts(), o)
                                                   : s.actions->uninstall(UninstallOptions{false}, o));
            QTRY_COMPARE(o.results.size(), 1);
            QVERIFY(s.process.launches.isEmpty());
            QCOMPARE(o.results[0].outcome, Outcome::NothingChanged);
            QCOMPARE(o.results[0].message, HelperClient::fingerprintMismatchMessage());
            QVERIFY(!s.actions->busy());
        }
    }
    // The locator's reason (WS-14) ends a staged action before any launch; the installed-helper actions keep working.
    void programProblemDisablesOnlyTheStagedRoute()
    {
        Setup s;
        s.problem = QStringLiteral("the program beside this window is not the one it was built with");
        s.make();
        Observer o;
        QVERIFY(s.actions->update(std::nullopt, o));
        QTRY_COMPARE(o.results.size(), 1);
        QVERIFY(s.process.launches.isEmpty());
        QVERIFY2(o.results[0].message.contains(s.problem), qPrintable(o.results[0].message));
        QVERIFY(o.results[0].message.endsWith(QLatin1String("nothing was changed.")));
        Observer o2;
        QVERIFY(s.actions->start(o2));
        QTRY_COMPARE(o2.results.size(), 1);
        QCOMPARE(s.process.launches.size(), 1);
        QVERIFY(!s.process.launches[0].staged);
    }
    // One request at a time: while the program is read, nothing else starts.
    void busyWhileTheProgramIsRead()
    {
        Setup s;
        s.make();
        Observer o, o2;
        QVERIFY(s.actions->install(std::nullopt, o));
        QVERIFY(s.actions->busy());
        QVERIFY(!s.actions->start(o2));
        QVERIFY(!s.actions->uninstall(UninstallOptions{}, o2));
        QVERIFY(!s.actions->discardPending(QString(64, QLatin1Char('a')), o2));
        QTRY_COMPARE(o.results.size(), 1);
        QVERIFY(o2.results.isEmpty());
        QCOMPARE(s.process.launches.size(), 1);
    }
    // INV-124: the terminal command names $APPIMAGE when the window runs from one, else the program itself.
    void terminalCommandNamesTheAppImageOrTheProgram()
    {
        for (const QString& appImage : {QStringLiteral("/media/u/aether-serial-bridge-gui-x86_64.AppImage"), QString()}) {
            Setup s;
            s.appImage = appImage;
            s.make();
            IHelperProcess::Exit refused;
            refused.code = 127;
            refused.err = "Error executing command as another user: Not authorized\n";
            s.process.next = refused;
            Observer o;
            QVERIFY(s.actions->uninstall(UninstallOptions{true}, o));
            QTRY_COMPARE(o.results.size(), 1);
            const QString want = QStringLiteral("sudo \"%1\" --uninstall --remove-settings").arg(appImage.isEmpty() ? kProgramPath : appImage);
            QVERIFY2(o.results[0].message.endsWith(want), qPrintable(o.results[0].message));
            QCOMPARE(s.actions->lastOutcome()->kind, HelperClient::Outcome::Kind::NotAuthorized);
        }
    }
    // The 20-40 MB program is hashed on the worker thread, never the window's, and over the very buffer the
    // client then sends (one read, no copy re-read). The call returns, and the window's loop keeps running, while the
    // hash is still in progress; the digest compare then lets this window's build through.
    void hashedOffTheWindowThreadOverTheSentBytes()
    {
        for (const char* which : {"install", "uninstall"}) {
            Setup s;
            std::atomic<bool> entered{false}, release{false};
            std::thread::id hashThread;      // written before `entered`, read after it
            const char* hashed = nullptr;
            s.hash = [&](const QByteArray& b) {
                hashThread = std::this_thread::get_id();
                hashed = b.constData();
                entered = true;
                // Bounded, so a hash on the window's thread (the bug) fails below instead of hanging the test.
                for (int i = 0; i < 400 && !release.load(); ++i) QThread::msleep(5);
                return sha256Hex(b).toLatin1();
            };
            s.make();
            Observer o;
            QVERIFY(qstrcmp(which, "install") == 0 ? s.actions->install(std::nullopt, o)
                                                   : s.actions->uninstall(UninstallOptions{false}, o));
            QTRY_VERIFY(entered.load());
            QVERIFY(o.results.isEmpty());
            QVERIFY(s.actions->busy());
            release = true;
            QTRY_COMPARE(o.results.size(), 1);
            QVERIFY2(hashThread != std::this_thread::get_id(), "the program was hashed on the window's thread");
            QCOMPARE(s.reads.load(), 1);
            QCOMPARE(s.process.launches.size(), 1);
            QVERIFY(s.process.launches[0].staged);
            QCOMPARE(s.process.launches[0].program, kProgram);
            QVERIFY2(s.process.launches[0].program.constData() == hashed, "the bytes sent are not the buffer hashed");
        }
    }
    // Every staged ending arrives when the actions are no longer busy, so an observer that refreshes its
    // buttons (or starts a follow-up) from finished() sees them free.
    void notBusyInsideFinished()
    {
        for (const char* ending : {"no reader", "read problem", "digest mismatch", "launched"}) {
            Setup s;
            s.noReader = qstrcmp(ending, "no reader") == 0;
            if (qstrcmp(ending, "read problem") == 0) s.problem = QStringLiteral("it could not be read");
            if (qstrcmp(ending, "digest mismatch") == 0) s.bytes = QByteArray("not the window's build");
            s.make();
            Observer o;
            std::optional<bool> busyInside;
            o.onFinished = [&] { busyInside = s.actions->busy(); };
            QVERIFY(s.actions->install(std::nullopt, o));
            QVERIFY(s.actions->busy());
            QTRY_COMPARE(o.results.size(), 1);
            QVERIFY2(busyInside.has_value() && !*busyInside, ending);
            QCOMPARE(s.process.launches.size(), qstrcmp(ending, "launched") == 0 ? 1 : 0);
        }
    }
    // A window closed while the program is read: nothing is delivered afterwards, nothing crashes.
    void destroyedWhileReading()
    {
        Setup s;
        std::atomic<bool> release{false};
        HelperActions::Staged st;
        st.expectedSha256 = sha256Hex(kProgram).toLatin1();
        st.read = [&release] {
            while (!release.load()) QThread::msleep(5);
            return HelperActions::ProgramFile{kProgramPath, kProgram, QString()};
        };
        Observer o;
        std::thread releaser;
        {
            HelperActions a(s.client, st, [] { return RouteFacts{}; });
            QVERIFY(a.install(std::nullopt, o));
            releaser = std::thread([&release] {
                QThread::msleep(50);
                release = true;
            });
        }   // the destructor joins the reader
        releaser.join();
        QTest::qWait(100);
        QVERIFY(o.results.isEmpty());
        QVERIFY(s.process.launches.isEmpty());
    }
};

QTEST_GUILESS_MAIN(tst_HelperActions)
#include "tst_HelperActions.moc"
