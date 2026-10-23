// The Linux GUI's helper client (WS-9b; master §5.7, D57; INV-107, INV-118) over a fake process boundary: every way a
// pkexec run can end maps to one outcome (126 cancelled, 127 not authorized, 2 refused, a crash or an unreadable
// response unknown, 0/1 the helper's Result); dirty is cleared only for a persisted revision whose digest is the one
// this client computed; one request at a time; a slow helper produces one "still working" notice and the client keeps
// waiting for the result.

#include "cli/HelperClient.h"
#include "core/ConfigJson.h"
#include "core/EditTracker.h"

#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <optional>

using namespace asb;
using namespace asb::cli;
using Kind = HelperClient::Outcome::Kind;

namespace {

// Records the request; finishes when the test says so (or at once, with `next`).
class FakeProcess final : public IHelperProcess {
public:
    void run(const QByteArray& request, std::function<void(const Exit&)> done) override
    {
        requests << request;
        m_done = std::move(done);
        if (next) {
            const Exit e = *next;
            QTimer::singleShot(0, [this, e] { finish(e); });
        }
    }
    void finish(const Exit& e)
    {
        auto d = std::move(m_done);
        m_done = nullptr;
        if (d) d(e);
    }
    void runStaged(const QByteArray& expectedSha256, const QByteArray& program, const QByteArray& request,
                   std::function<void(const Exit&)> done) override
    {
        staged << StagedRun{expectedSha256, program, request};
        run(request, std::move(done));
    }
    bool running() const { return bool(m_done); }
    std::optional<Exit> next;
    QList<QByteArray> requests;
    struct StagedRun {
        QByteArray expectedSha256, program, request;
    };
    QList<StagedRun> staged;

private:
    std::function<void(const Exit&)> m_done;
};

IHelperProcess::Exit exitWith(int code, const QByteArray& out = {}, const QByteArray& err = {})
{
    IHelperProcess::Exit e;
    e.code = code;
    e.out = out;
    e.err = err;
    return e;
}

QByteArray response(Outcome outcome, Persisted persisted, const QByteArray& sha)
{
    Result r;
    r.outcome = outcome;
    r.persisted = persisted;
    r.persistedSha256 = sha;
    r.message = QStringLiteral("the helper's message");
    return helper::encodeResult(r);
}

helper::Request saveOf(const QByteArray& bytes)
{
    return helper::Request{helper::Op::SaveAndApply, SaveRequest{bytes, Source{}, false}};
}

const QByteArray kDoc = QByteArrayLiteral("{\"version\":1,\"ports\":[]}");
const QByteArray kDocSha = sha256Hex(kDoc).toLatin1();

}  // namespace

class tst_HelperClient : public QObject {
    Q_OBJECT

private slots:
    // ---- every ending of a pkexec run --------------------------------------------------------------
    void cancelledDialog()
    {
        const auto o = HelperClient::interpret(saveOf(kDoc), kDocSha, 7, exitWith(126, {}, "Error executing command as another user: Request dismissed\n"));
        QCOMPARE(o.kind, Kind::Cancelled);
        QCOMPARE(o.message, QStringLiteral("Cancelled; nothing was changed."));
        QCOMPARE(o.result.outcome, Outcome::NothingChanged);
        QCOMPARE(o.result.persisted, Persisted::None);
        QVERIFY(!o.clearsDirty);
    }
    void notAuthorized()
    {
        const auto o = HelperClient::interpret(saveOf(kDoc), kDocSha, 7, exitWith(127, {}, "Error executing command as another user: Not authorized\n\nThis incident has been reported.\n"));
        QCOMPARE(o.kind, Kind::NotAuthorized);
        // v1.1 (INV-124): the wording is polkit's refusal; with no view facts the installed route keeps v1.0's hint.
        QCOMPARE(o.message, QStringLiteral("polkit did not allow this; nothing was changed. Is the bridge installed (sudo "
                                           "aether-serial-bridge --install)?"));
        QCOMPARE(o.result.outcome, Outcome::NothingChanged);
        QVERIFY(!o.clearsDirty);
    }
    void refusedRequest()
    {
        const auto o = HelperClient::interpret(saveOf(kDoc), kDocSha, 7,
                                               exitWith(2, {}, "error: the request is too large (more than 262144 bytes); nothing was changed.\n"));
        QCOMPARE(o.kind, Kind::Refused);
        QCOMPARE(o.message, QStringLiteral("The helper refused the request (the request is too large (more than 262144 bytes)); nothing was "
                                           "changed."));
        QCOMPARE(o.result.outcome, Outcome::NothingChanged);
    }
    // Exit 5: the helper's guard could not be installed; it ends at mode entry, before the request.
    void guardNotInstalledChangesNothing()
    {
        const auto o = HelperClient::interpret(saveOf(kDoc), kDocSha, 7,
                                               exitWith(5, {}, "aether-serial-bridge: the process guard could not be installed: EINVAL\n"));
        QCOMPARE(o.kind, Kind::Refused);
        QVERIFY2(o.message.startsWith(QStringLiteral("The helper could not start safely")), qPrintable(o.message));
        QVERIFY(o.message.endsWith(QStringLiteral("nothing was changed.")));
        QCOMPARE(o.result.outcome, Outcome::NothingChanged);
        QCOMPARE(o.result.persisted, Persisted::None);
        QVERIFY(!o.clearsDirty);
    }
    void pkexecMissing()
    {
        IHelperProcess::Exit e;
        e.started = false;
        e.err = "pkexec: No such file or directory";
        const auto o = HelperClient::interpret(saveOf(kDoc), kDocSha, 7, e);
        QCOMPARE(o.kind, Kind::Refused);
        QVERIFY(o.message.startsWith("Could not run pkexec (pkexec: No such file or directory); nothing was changed."));
    }
    // The helper may have acted: the state is not known and dirty is kept.
    void unknownEndings_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("crashed");
        QTest::addColumn<QByteArray>("out");
        QTest::newRow("killed") << 0 << true << QByteArray();
        QTest::newRow("exit 0, no response") << 0 << false << QByteArray();
        QTest::newRow("exit 0, garbage") << 0 << false << QByteArray("Saved and applied\n");
        QTest::newRow("exit 0, ok:false") << 0 << false << response(Outcome::Uncommitted, Persisted::Staged, kDocSha);
        QTest::newRow("exit 1, ok:true") << 1 << false << response(Outcome::Ok, Persisted::Committed, kDocSha);
        QTest::newRow("exit 0, two responses") << 0 << false << (response(Outcome::Ok, Persisted::Committed, kDocSha) + response(Outcome::Ok, Persisted::Committed, kDocSha));
    }
    void unknownEndings()
    {
        QFETCH(int, code);
        QFETCH(bool, crashed);
        QFETCH(QByteArray, out);
        IHelperProcess::Exit e = exitWith(code, out);
        e.crashed = crashed;
        const auto o = HelperClient::interpret(saveOf(kDoc), kDocSha, 7, e);
        QCOMPARE(o.kind, Kind::Failed);
        QCOMPARE(o.result.outcome, Outcome::StateUnknown);
        QCOMPARE(o.result.persisted, Persisted::None);
        QVERIFY(!o.clearsDirty);
        QVERIFY2(o.message.contains("the bridge's state is not known"), qPrintable(o.message));
    }

    // ---- INV-107: dirty follows persisted and the client's own digest ------------------------------
    void clearsDirty_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<int>("outcome");
        QTest::addColumn<int>("persisted");
        QTest::addColumn<QByteArray>("echo");
        QTest::addColumn<bool>("clears");
        const QByteArray other = sha256Hex("{ \"version\":1,\"ports\":[] }").toLatin1();
        QTest::newRow("committed, matching") << 0 << int(Outcome::Ok) << int(Persisted::Committed) << kDocSha << true;
        QTest::newRow("staged failure, matching") << 1 << int(Outcome::Uncommitted) << int(Persisted::Staged) << kDocSha << true;
        QTest::newRow("saved not applied, matching") << 0 << int(Outcome::SavedNotApplied) << int(Persisted::Staged) << kDocSha << true;
        QTest::newRow("committed, another digest") << 0 << int(Outcome::Ok) << int(Persisted::Committed) << other << false;
        QTest::newRow("staged failure, another digest") << 1 << int(Outcome::Uncommitted) << int(Persisted::Staged) << other << false;
        QTest::newRow("committed, no digest") << 0 << int(Outcome::Ok) << int(Persisted::Committed) << QByteArray() << false;
        QTest::newRow("nothing persisted, matching") << 1 << int(Outcome::NothingChanged) << int(Persisted::None) << kDocSha << false;
        QTest::newRow("state unknown, nothing persisted") << 1 << int(Outcome::StateUnknown) << int(Persisted::None) << kDocSha << false;
    }
    void clearsDirty()
    {
        QFETCH(int, code);
        QFETCH(int, outcome);
        QFETCH(int, persisted);
        QFETCH(QByteArray, echo);
        QFETCH(bool, clears);
        const auto o = HelperClient::interpret(saveOf(kDoc), kDocSha, 7, exitWith(code, response(Outcome(outcome), Persisted(persisted), echo)));
        QCOMPARE(o.kind, Kind::Done);
        QCOMPARE(o.clearsDirty, clears);
        QCOMPARE(o.revision, quint64(7));
        QCOMPARE(o.message, QStringLiteral("the helper's message"));
    }
    // Only a save carries a revision: a Start that happens to report a digest clears nothing.
    void nonSaveOpsNeverClearDirty()
    {
        const auto o = HelperClient::interpret(helper::Request{helper::Op::Start, std::nullopt}, {}, 0,
                                               exitWith(0, response(Outcome::Ok, Persisted::Committed, kDocSha)));
        QCOMPARE(o.kind, Kind::Done);
        QVERIFY(!o.clearsDirty);
    }
    void inspectResponse()
    {
        const auto o = HelperClient::interpret(helper::Request{helper::Op::Inspect, std::nullopt}, {}, 0,
                                               exitWith(0, helper::encodeInspect({"Trust", "", "Current", "Running"}, true)));
        QCOMPARE(o.kind, Kind::Done);
        QVERIFY(o.inspect);
        QCOMPARE(o.inspect->service, QStringLiteral("Running"));
        QCOMPARE(o.message, QStringLiteral("Service: Running; settings folder: Trust."));   // never empty
        const auto u = HelperClient::interpret(helper::Request{helper::Op::Inspect, std::nullopt}, {}, 0,
                                               exitWith(0, helper::encodeInspect({"Recreate", "owned by mallory", "NotInstalled", "Absent"}, true)));
        QCOMPARE(u.message, QStringLiteral("Service: NotInstalled; settings folder: Recreate (owned by mallory)."));
    }
    // A run that ends after the client was destroyed is ignored: nothing of the client is touched and `done` is not
    // called. The client lives in storage the test keeps, so a missing guard would be seen as a call.
    void runEndingAfterTheClientIsGoneIsIgnored()
    {
        FakeProcess p;
        std::optional<HelperClient> client;
        client.emplace(p);
        int calls = 0;
        QVERIFY(client->saveAndApply(kDoc, Source{}, false, 3, [&](const HelperClient::Outcome&) { ++calls; }));
        QVERIFY(p.running());
        client.reset();
        p.finish(exitWith(0, response(Outcome::Ok, Persisted::Committed, kDocSha)));
        QCOMPARE(calls, 0);
    }

    // ---- the submission path ------------------------------------------------------------------------
    // The digest is the client's own, over the exact bytes it sent; the request carries them byte for byte.
    void submitsTheExactBytesAndDigestsThemItself()
    {
        FakeProcess p;
        HelperClient client(p);
        std::optional<HelperClient::Outcome> got;
        const QByteArray bytes("{ \"version\": 1, \"ports\": [] }\n");
        QVERIFY(client.saveAndApply(bytes, Source{Source::Pending, sha256Hex("p").toLatin1()}, true, 3,
                                    [&](const HelperClient::Outcome& o) { got = o; }));
        QCOMPARE(p.requests.size(), 1);
        QString why;
        const auto req = helper::parseRequest(p.requests[0], &why);
        QVERIFY2(req && req->save, qPrintable(why));
        QCOMPARE(req->save->config, bytes);
        QCOMPARE(req->save->supersedePending, true);
        p.finish(exitWith(0, response(Outcome::Ok, Persisted::Committed, sha256Hex(bytes).toLatin1())));
        QVERIFY(got);
        QCOMPARE(got->submittedSha256, sha256Hex(bytes).toLatin1());
        QVERIFY(got->clearsDirty);
        QCOMPARE(got->revision, quint64(3));
    }
    // One request at a time (the window refuses to close while busy, D57).
    void oneRequestAtATime()
    {
        FakeProcess p;
        HelperClient client(p);
        int done = 0;
        QVERIFY(client.stop([&](const HelperClient::Outcome&) { ++done; }));
        QVERIFY(client.busy());
        QVERIFY(!client.start([&](const HelperClient::Outcome&) { ++done; }));
        QCOMPARE(p.requests.size(), 1);
        p.finish(exitWith(0, response(Outcome::Ok, Persisted::None, {})));
        QCOMPARE(done, 1);
        QVERIFY(!client.busy());
        QVERIFY(client.start([&](const HelperClient::Outcome&) { ++done; }));
    }
    // The wait deadline gives one "still working" notice; the client keeps waiting for the helper's result.
    void slowHelperGivesOneNoticeThenTheResult()
    {
        FakeProcess p;
        HelperClient client(p, std::chrono::milliseconds(50));
        QStringList notices;
        std::optional<HelperClient::Outcome> got;
        EditTracker t;
        t.markEdited();
        QVERIFY(client.saveAndApply(kDoc, Source{}, false, t.revision(), [&](const HelperClient::Outcome& o) { got = o; },
                                    [&](const QString& m) { notices << m; }));
        QTRY_COMPARE_WITH_TIMEOUT(notices.size(), 1, 2000);
        QTest::qWait(200);
        QCOMPARE(notices.size(), 1);
        QVERIFY(!got);
        QVERIFY(client.busy());
        QVERIFY(t.isDirty());   // the notice is not a result: nothing is cleared
        QVERIFY(notices[0].contains("the result will say what state the bridge is in"));
        p.finish(exitWith(0, response(Outcome::Ok, Persisted::Committed, kDocSha)));
        QVERIFY(got && got->clearsDirty);
        QVERIFY(t.markSaved(got->revision));
    }
    // A result before the deadline: no notice afterwards.
    void noNoticeAfterTheResult()
    {
        FakeProcess p;
        p.next = exitWith(0, response(Outcome::Ok, Persisted::None, {}));
        HelperClient client(p, std::chrono::milliseconds(100));
        int notices = 0, done = 0;
        QVERIFY(client.start([&](const HelperClient::Outcome&) { ++done; }, [&](const QString&) { ++notices; }));
        QTRY_COMPARE(done, 1);
        QTest::qWait(250);
        QCOMPARE(notices, 0);
    }
    void deactivateWithAndWithoutEdits()
    {
        FakeProcess p;
        HelperClient client(p);
        QVERIFY(client.deactivate(kDoc, Source{Source::Committed, sha256Hex("c").toLatin1()}, false, 1, {}));
        p.finish(exitWith(0, response(Outcome::SavedNotApplied, Persisted::Staged, kDocSha)));
        QVERIFY(client.deactivate(std::nullopt, Source{}, false, 2, {}));
        QString why;
        QVERIFY(helper::parseRequest(p.requests[0], &why)->save);
        QVERIFY(!helper::parseRequest(p.requests[1], &why)->save);
    }

    // ---- v1.1 (WS-15, INV-124): the staged route's outcomes, from recorded stderr ------------------------------------
    // The pkexec texts are the ones CI recorded (polkit 122 in debian:bookworm, 125 in fedora:41; WS-15 report): a NO
    // rule or a wrong password says "Not authorized"; no agent and no terminal says "Error creating textual
    // authentication agent". Older polkit says "No authentication agent found".
    void bootstrapOutcomes_data()
    {
        QTest::addColumn<int>("route");
        QTest::addColumn<int>("code");
        QTest::addColumn<QByteArray>("err");
        QTest::addColumn<QByteArray>("out");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("message");   // exact, or "~" + a part that must appear
        const int S = int(Route::Staged), I = int(Route::Installed);
        const QByteArray notAuth = "Error executing command as another user: Not authorized\n\nThis incident has been reported.\n";
        const QByteArray noTty = "Error creating textual authentication agent: Error opening current controlling terminal for the "
                                 "process (`/dev/tty'): No such device or address\n";
        const QString cmd = QStringLiteral(" From a terminal: sudo \"/media/u/aether-serial-bridge-gui-x86_64.AppImage\" --install");
        QTest::newRow("staged 126") << S << 126 << QByteArray("Error executing command as another user: Request dismissed\n") << QByteArray()
                                    << int(Kind::Cancelled) << "Cancelled; nothing was changed.";
        QTest::newRow("staged 127 not authorized") << S << 127 << notAuth << QByteArray() << int(Kind::NotAuthorized)
                                                   << QStringLiteral("polkit did not allow this; nothing was changed.") + cmd;
        QTest::newRow("staged 127 no agent (polkit 122)") << S << 127 << noTty << QByteArray() << int(Kind::NoAgent)
                                                          << QStringLiteral("No polkit authentication agent is running in this desktop "
                                                                            "session, so nothing could ask for a password. Nothing was "
                                                                            "changed.") + cmd;
        QTest::newRow("staged 127 no agent (older polkit)")
            << S << 127 << QByteArray("Error executing command as another user: No authentication agent found.\n") << QByteArray()
            << int(Kind::NoAgent) << "~No polkit authentication agent is running";
        QTest::newRow("staged 127 anything else") << S << 127 << QByteArray("something new\n") << QByteArray() << int(Kind::NotAuthorized)
                                                  << QStringLiteral("polkit did not allow this; nothing was changed.") + cmd;
        QTest::newRow("staged 3") << S << 3 << QByteArray("asb-stage: the program does not match its digest\n") << QByteArray()
                                  << int(Kind::Refused) << "The program inside this window does not match its fingerprint; nothing was run.";
        QTest::newRow("staged 4 framing") << S << 4 << QByteArray("asb-stage: the payload is not 10+5 bytes\n") << QByteArray()
                                          << int(Kind::Refused) << "Nothing was changed: the payload is not 10+5 bytes.";
        QTest::newRow("staged 4 full") << S << 4 << QByteArray("head: error writing\nasb-stage: cannot store the payload\n") << QByteArray()
                                       << int(Kind::Refused) << "Nothing was changed: cannot store the payload.";
        QTest::newRow("staged 4 interrupted") << S << 4 << QByteArray("asb-stage: interrupted\n") << QByteArray()
                                              << int(Kind::Refused) << "Nothing was changed: interrupted.";
        QTest::newRow("staged 4 unsafe directory (R28)")
            << S << 4 << QByteArray("asb-stage: /usr/local/lib is not a directory only root can write\n") << QByteArray() << int(Kind::Refused)
            << "Nothing was changed: /usr/local/lib is not a directory only root can write. An administrator can make it writable by "
               "root only (sudo chmod g-w,o-w /usr/local/lib), or use the terminal command: sudo "
               "\"/media/u/aether-serial-bridge-gui-x86_64.AppImage\" --install";
        QTest::newRow("staged 4 unsafe root directory")
            << S << 4 << QByteArray("asb-stage: / is not a directory only root can write\n") << QByteArray() << int(Kind::Refused)
            << "Nothing was changed: / is not a directory only root can write. An administrator can make it writable by "
               "root only (sudo chmod g-w,o-w /), or use the terminal command: sudo "
               "\"/media/u/aether-serial-bridge-gui-x86_64.AppImage\" --install";
        QTest::newRow("staged 4, only the script's line counts")
            << S << 4 << QByteArray("error: the request is not valid: x; nothing was changed.\nasb-stage: bad lengths\n") << QByteArray()
            << int(Kind::Refused) << "Nothing was changed: bad lengths.";
        QTest::newRow("staged 6") << S << 6 << QByteArray("asb-stage: the staged program could not be executed (126)\n") << QByteArray()
                                  << int(Kind::Refused)
                                  << "The staged program could not be started (the staged program could not be executed (126)); nothing was changed.";
        QTest::newRow("staged 2") << S << 2 << QByteArray("error: the request is not valid: x; nothing was changed.\n") << QByteArray()
                                  << int(Kind::Refused) << "~The helper refused the request";
        QTest::newRow("staged 5") << S << 5 << QByteArray("error: guard\n") << QByteArray() << int(Kind::Refused) << "~could not start safely";
        QTest::newRow("staged 0") << S << 0 << QByteArray() << response(Outcome::Ok, Persisted::None, {}) << int(Kind::Done) << "the helper's message";
        QTest::newRow("staged 1") << S << 1 << QByteArray() << response(Outcome::Uncommitted, Persisted::None, {}) << int(Kind::Done) << "the helper's message";
        QTest::newRow("staged killed (137)") << S << 137 << QByteArray() << QByteArray() << int(Kind::Failed) << "~the bridge's state is not known";
        // The installed route: the helper never exits 3, 4 or 6, so those are not "nothing ran" there.
        QTest::newRow("installed 3") << I << 3 << QByteArray("asb-stage: the program does not match its digest\n") << QByteArray()
                                     << int(Kind::Failed) << "~the bridge's state is not known";
        QTest::newRow("installed 4") << I << 4 << QByteArray("asb-stage: bad lengths\n") << QByteArray() << int(Kind::Failed) << "~the bridge's state is not known";
        QTest::newRow("installed 6") << I << 6 << QByteArray() << QByteArray() << int(Kind::Failed) << "~the bridge's state is not known";
        QTest::newRow("installed 127, something installed") << I << 127 << notAuth << QByteArray() << int(Kind::NotAuthorized)
                                                            << "polkit did not allow this; nothing was changed.";
    }
    void bootstrapOutcomes()
    {
        QFETCH(int, route);
        QFETCH(int, code);
        QFETCH(QByteArray, err);
        QFETCH(QByteArray, out);
        QFETCH(int, kind);
        QFETCH(QString, message);
        helper::Request req;
        req.op = Route(route) == Route::Staged ? helper::Op::Install : helper::Op::Start;
        HelperClient::Context ctx;
        if (Route(route) == Route::Staged)
            ctx.terminalCommand = staged::terminalCommand(QStringLiteral("/media/u/aether-serial-bridge-gui-x86_64.AppImage"), req.op);
        ctx.nothingInstalled = false;
        const auto o = HelperClient::interpret(req, {}, 0, exitWith(code, out, err), Route(route), ctx);
        QCOMPARE(int(o.kind), kind);
        QCOMPARE(int(o.route), route);
        if (message.startsWith(QLatin1Char('~'))) QVERIFY2(o.message.contains(message.mid(1)), qPrintable(o.message));
        else QCOMPARE(o.message, message);
        QVERIFY(!o.message.contains(QLatin1String("Is the bridge installed")));
        QVERIFY(!o.clearsDirty);
        if (o.kind != Kind::Done && o.kind != Kind::Failed) QCOMPARE(o.result.outcome, Outcome::NothingChanged);
    }
    // The installed route keeps v1.0's hint only when the view says nothing is installed.
    void installedRouteHintOnlyWhenNothingInstalled()
    {
        const QByteArray notAuth = "Error executing command as another user: Not authorized\n";
        helper::Request req;
        req.op = helper::Op::Start;
        for (const bool nothing : {true, false}) {
            const auto o = HelperClient::interpret(req, {}, 0, exitWith(127, {}, notAuth), Route::Installed, HelperClient::Context{QString(), nothing});
            QCOMPARE(o.message.contains(QLatin1String("Is the bridge installed (sudo aether-serial-bridge --install)?")), nothing);
        }
    }
    // INV-107 on the staged route: an install with the editor's ports clears dirty only for the digest of the exact bytes
    // the client sent; an install without ports never does.
    void stagedClearsDirtyOnlyOnMatchingDigest()
    {
        FakeProcess p;
        HelperClient client(p);
        const QByteArray program("#!program bytes");
        const QByteArray sha = sha256Hex(program).toLatin1();
        const HelperClient::StagedProgram prog{program, sha, sha, QStringLiteral("/x/a.AppImage")};
        std::optional<HelperClient::Outcome> got;
        QVERIFY(client.install(kDoc, Source{}, false, 9, prog, [&](const HelperClient::Outcome& o) { got = o; }));
        QCOMPARE(p.staged.size(), 1);
        QCOMPARE(p.staged[0].program, program);
        QCOMPARE(p.staged[0].expectedSha256, prog.expectedSha256);
        p.finish(exitWith(0, response(Outcome::Ok, Persisted::Committed, kDocSha)));
        QVERIFY(got && got->clearsDirty && got->revision == 9 && got->route == Route::Staged);
        got.reset();
        QVERIFY(client.install(kDoc, Source{}, false, 10, prog, [&](const HelperClient::Outcome& o) { got = o; }));
        p.finish(exitWith(0, response(Outcome::Ok, Persisted::Committed, sha256Hex("other").toLatin1())));
        QVERIFY(got && got->kind == Kind::Done && !got->clearsDirty);
        got.reset();
        QVERIFY(client.install(std::nullopt, Source{}, false, 11, prog, [&](const HelperClient::Outcome& o) { got = o; }));
        p.finish(exitWith(0, response(Outcome::Ok, Persisted::None, {})));
        QVERIFY(got && got->kind == Kind::Done && !got->clearsDirty);
        QString why;
        QVERIFY(!helper::parseRequest(p.staged[2].request, &why)->save);
    }
    // D60: bytes that are not the window's build are never launched; nor are bytes without a digest, even when the
    // expected digest is missing too (a check that only compared the two would launch them).
    void stagedMismatchNeverLaunches_data()
    {
        QTest::addColumn<QByteArray>("bytesSha256");
        QTest::addColumn<QByteArray>("expectedSha256");
        const QByteArray real = sha256Hex("the real program").toLatin1();
        QTest::newRow("tampered bytes") << sha256Hex("tampered").toLatin1() << real;
        QTest::newRow("bytes not hashed") << QByteArray() << real;
        QTest::newRow("both digests empty") << QByteArray() << QByteArray();
        QTest::newRow("both digests short") << QByteArray("abc") << QByteArray("abc");
    }
    void stagedMismatchNeverLaunches()
    {
        QFETCH(QByteArray, bytesSha256);
        QFETCH(QByteArray, expectedSha256);
        FakeProcess p;
        HelperClient client(p);
        const HelperClient::StagedProgram prog{QByteArray("tampered"), bytesSha256, expectedSha256, QStringLiteral("/x/p")};
        std::optional<HelperClient::Outcome> got;
        QVERIFY(client.uninstall(false, prog, [&](const HelperClient::Outcome& o) { got = o; }));
        QVERIFY(client.busy());
        QVERIFY(!got);   // never inside the call
        QTRY_VERIFY(got.has_value());
        QVERIFY(p.staged.isEmpty() && p.requests.isEmpty());
        QCOMPARE(got->kind, Kind::Refused);
        QCOMPARE(got->message, HelperClient::fingerprintMismatchMessage());
        QVERIFY(!client.busy());
    }
    // INV-124: the command a 127 shows runs as shown: the AppImage (AppRun forwards both flags) or the program itself,
    // --remove-settings when the box was ticked, quoted for the shell.
    void fallbackCommandNamesRemoveSettings()
    {
        using staged::terminalCommand;
        const QString img = QStringLiteral("/media/u/Apps/aether-serial-bridge-gui-x86_64.AppImage");
        QCOMPARE(terminalCommand(img, helper::Op::Install), QStringLiteral("sudo \"%1\" --install").arg(img));
        QCOMPARE(terminalCommand(img, helper::Op::Uninstall, false), QStringLiteral("sudo \"%1\" --uninstall").arg(img));
        QCOMPARE(terminalCommand(img, helper::Op::Uninstall, true), QStringLiteral("sudo \"%1\" --uninstall --remove-settings").arg(img));
        QCOMPARE(terminalCommand(QStringLiteral("/tmp/a \"b\" $c`d\\e"), helper::Op::Install),
                 QStringLiteral("sudo \"/tmp/a \\\"b\\\" \\$c\\`d\\\\e\" --install"));
        // Through the client: an uninstall pkexec refused names the box's choice.
        FakeProcess p;
        HelperClient client(p);
        const QByteArray program("prog");
        std::optional<HelperClient::Outcome> got;
        QVERIFY(client.uninstall(true, HelperClient::StagedProgram{program, sha256Hex(program).toLatin1(), sha256Hex(program).toLatin1(), img},
                                 [&](const HelperClient::Outcome& o) { got = o; }));
        p.finish(exitWith(127, {}, "Error executing command as another user: Not authorized\n"));
        QVERIFY(got);
        QVERIFY2(got->message.endsWith(QStringLiteral(" From a terminal: sudo \"%1\" --uninstall --remove-settings").arg(img)),
                 qPrintable(got->message));
        QVERIFY(!got->message.contains(QLatin1String("Is the bridge installed")));
        QString why;
        QCOMPARE(helper::parseRequest(p.staged[0].request, &why)->removeSettings, true);
    }
};

QTEST_GUILESS_MAIN(tst_HelperClient)
#include "tst_HelperClient.moc"
