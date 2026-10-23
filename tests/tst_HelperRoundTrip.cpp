// tst_HelperRoundTrip (master §5.7, WS-9b): the GUI's client → the helper → the client, three cases:
//  (1) two whitespace-different but equivalent JSON documents produce two different digests, each echoed exactly in
//      persistedSha256 (and config.json holds exactly the bytes sent: nothing is reserialised);
//  (2) a staged failure (Uncommitted, persisted = Staged) clears dirty only for the matching digest;
//  (3) a digest mismatch keeps dirty.
//
// ASB_HELPER_ROUNDTRIP picks the helper:
//   unset / "inprocess"  runHelper itself over the WS-5 fakes (ctest, unprivileged);
//   "direct"             the installed program, /usr/local/bin/aether-serial-bridge --helper, run as root (the systemd
//                        suite's helper_round_trip; the only mode that can stage the failure of (2): pkexec clears the
//                        environment, so ASB_TEST_FAIL cannot reach the helper through it);
//   "pkexec"             `pkexec /usr/local/bin/aether-serial-bridge --helper` as an ordinary user under a test polkit
//                        rule that answers YES for the helper's action (proves the installed policy maps the program);
//   "pkexec-denied"      the same under a rule that answers NO: not authorized, nothing changed.
// The real modes use the installed program and the managed folder, so they run only inside the systemd suite's
// container (the case installs first). The process boundary here is a test copy of what gui/PkexecClient.cpp (WS-10)
// does with QProcess; the GUI-side half (BridgeWindow's dirty state) is WS-10's tst_BridgeWindow.

#include "cli/Helper.h"
#include "cli/HelperClient.h"
#include "core/ConfigJson.h"
#include "core/EditTracker.h"
#include "support/FakeCliHost.h"

#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

using namespace asb;
using namespace asb::cli;
using namespace asb::test;
using Kind = HelperClient::Outcome::Kind;

namespace {

const char* const kInstalled = "/usr/local/bin/aether-serial-bridge";
const char* const kManagedConfig = "/etc/aether-serial-bridge/config.json";

QString mode()
{
    const QString m = qEnvironmentVariable("ASB_HELPER_ROUNDTRIP");
    return m.isEmpty() ? QStringLiteral("inprocess") : m;
}

// Records every exit, so a test can judge the very same helper response against another submission.
class Recording : public IHelperProcess {
public:
    std::optional<Exit> last;
    // The staged route (v1.1) is the systemd suite's bootstrap_* cases (asb_stage_client); not driven here.
    void runStaged(const QByteArray&, const QByteArray&, const QByteArray&, std::function<void(const Exit&)>) override
    {
        qFatal("tst_HelperRoundTrip does not drive the staged route");
    }

protected:
    std::function<void(const Exit&)> recording(std::function<void(const Exit&)> done)
    {
        return [this, done = std::move(done)](const Exit& e) {
            last = e;
            done(e);
        };
    }
};

// runHelper in this process, over a FakeCliHost: stdin and stdout are files.
class InProcessHelper final : public Recording {
public:
    explicit InProcessHelper(FakeCliHost& h) : m_h(h) {}
    void run(const QByteArray& request, std::function<void(const Exit&)> done) override
    {
        const QString in = m_dir.filePath(QStringLiteral("in")), out = m_dir.filePath(QStringLiteral("out"));
        QFile f(in);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(request) != request.size()) qFatal("write");
        f.close();
        HelperOptions o;
        o.inputFd = ::open(QFile::encodeName(in).constData(), O_RDONLY | O_CLOEXEC);
        o.outputFd = ::open(QFile::encodeName(out).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        o.syslog = false;
        CapturingConsole c;
        Exit e;
        e.code = runHelper(m_h, c, o);
        ::close(o.inputFd);
        ::close(o.outputFd);
        QFile r(out);
        if (r.open(QIODevice::ReadOnly)) e.out = r.readAll();
        e.err = c.errText().toUtf8();
        auto finish = recording(std::move(done));
        QTimer::singleShot(0, [finish, e] { finish(e); });   // later, on this thread (IHelperProcess's contract)
    }

private:
    FakeCliHost& m_h;
    QTemporaryDir m_dir;
};

// The real program, as gui/PkexecClient.cpp will run it (WS-10), or directly as root.
class ProcessHelper final : public Recording {
public:
    ProcessHelper(QString program, QStringList args) : m_program(std::move(program)), m_args(std::move(args)) {}
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    void run(const QByteArray& request, std::function<void(const Exit&)> done) override
    {
        auto* p = new QProcess;
        p->setProcessEnvironment(env);
        auto finish = recording(std::move(done));
        QObject::connect(p, &QProcess::finished, p, [p, finish](int code, QProcess::ExitStatus st) {
            Exit e;
            e.code = code;
            e.crashed = st == QProcess::CrashExit;
            e.out = p->readAllStandardOutput();
            e.err = p->readAllStandardError();
            p->deleteLater();
            finish(e);
        });
        QObject::connect(p, &QProcess::errorOccurred, p, [p, finish](QProcess::ProcessError err) {
            if (err != QProcess::FailedToStart) return;
            Exit e;
            e.started = false;
            e.err = p->errorString().toUtf8();
            p->deleteLater();
            finish(e);
        });
        p->start(m_program, m_args);
        p->write(request);
        p->closeWriteChannel();
    }

private:
    QString m_program;
    QStringList m_args;
};

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A config the real service can run in the container, and the fakes accept.
QByteArray realConfig()
{
    return QByteArrayLiteral(R"({"version":1,"ports":[{"enabled":true,"name":"rt","comPort":"/dev/ttyS0","baudRate":9600,)"
                             R"("tcpPort":4031,"bindAddress":"127.0.0.1","allowedClients":""}]})");
}

}  // namespace

class tst_HelperRoundTrip : public QObject {
    Q_OBJECT

    // One backend per test: the fakes (in process) or the installed program.
    struct Backend {
        Rig rig;
        std::unique_ptr<FakeCliHost> host;
        std::unique_ptr<Recording> process;
        ProcessHelper* real = nullptr;
        QByteArray committed() const
        {
            return host ? rig.file(QStringLiteral("config.json")) : readFile(QString::fromLatin1(kManagedConfig));
        }
    };

    static std::unique_ptr<Backend> backend()
    {
        auto b = std::make_unique<Backend>();
        const QString m = mode();
        if (m == QLatin1String("inprocess")) {
            b->rig.installedRunning(configA());
            b->host = std::make_unique<FakeCliHost>(b->rig);
            b->process = std::make_unique<InProcessHelper>(*b->host);
        } else if (m == QLatin1String("direct")) {
            auto p = std::make_unique<ProcessHelper>(QString::fromLatin1(kInstalled), QStringList{QStringLiteral("--helper")});
            b->real = p.get();
            b->process = std::move(p);
        } else {
            auto p = std::make_unique<ProcessHelper>(QStringLiteral("pkexec"),
                                                     QStringList{QString::fromLatin1(kInstalled), QStringLiteral("--helper")});
            b->real = p.get();
            b->process = std::move(p);
        }
        return b;
    }

    static HelperClient::Outcome wait(std::optional<HelperClient::Outcome>& got)
    {
        if (!QTest::qWaitFor([&] { return got.has_value(); }, 180000)) qFatal("the helper did not finish");
        return *got;
    }

    static HelperClient::Outcome saveAndApply(HelperClient& client, Backend& b, const QByteArray& bytes, quint64 revision,
                                              bool supersede = true)
    {
        std::optional<HelperClient::Outcome> got;
        const QByteArray committed = b.committed();
        const Source source = committed.isEmpty() ? Source{} : Source{Source::Committed, sha256Hex(committed).toLatin1()};
        if (!client.saveAndApply(bytes, source, supersede, revision, [&](const HelperClient::Outcome& o) { got = o; }))
            qFatal("busy");
        return wait(got);
    }

    static QByteArray document(const Backend& b) { return b.host ? configB() : realConfig(); }
    // The same document, whitespace changed only.
    static QByteArray reformatted(const QByteArray& doc) { return QJsonDocument::fromJson(doc).toJson(QJsonDocument::Indented); }
    static QByteArray compacted(const QByteArray& doc) { return QJsonDocument::fromJson(doc).toJson(QJsonDocument::Compact); }

private slots:
    void initTestCase()
    {
        if (mode() != QLatin1String("inprocess") && !QFile::exists(QString::fromLatin1(kInstalled)))
            QSKIP("the real modes run inside the systemd suite's container, after --install");
    }

    // (1) Two equivalent documents that differ only in whitespace are two revisions with two digests, each echoed
    // exactly; config.json holds exactly the bytes that were sent.
    void whitespaceVariantsAreTwoDigestsEchoedExactly()
    {
        if (mode() == QLatin1String("pkexec-denied")) QSKIP("the denied mode runs deniedByPolicyChangesNothing only");
        auto b = backend();
        HelperClient client(*b->process);
        EditTracker t;
        const QByteArray one = compacted(document(*b));
        const QByteArray two = reformatted(one);
        QVERIFY(one != two);
        QCOMPARE(QJsonDocument::fromJson(one), QJsonDocument::fromJson(two));
        QList<QByteArray> echoed;
        for (const QByteArray& bytes : {one, two}) {
            t.markEdited();
            const HelperClient::Outcome o = saveAndApply(client, *b, bytes, t.revision());
            QVERIFY2(o.kind == Kind::Done && o.result.outcome == Outcome::Ok,
                     qPrintable(o.message + QStringLiteral(" | ") + QString::fromUtf8(b->process->last ? b->process->last->err : QByteArray())));
            QCOMPARE(o.result.persisted, Persisted::Committed);
            QCOMPARE(o.result.persistedSha256, sha256Hex(bytes).toLatin1());
            QCOMPARE(o.submittedSha256, sha256Hex(bytes).toLatin1());
            QCOMPARE(b->committed(), bytes);
            QVERIFY(o.clearsDirty);
            QVERIFY(t.markSaved(o.revision));
            echoed << o.result.persistedSha256;
        }
        QVERIFY(echoed[0] != echoed[1]);
    }

    // (2) A staged failure: Uncommitted with persisted = Staged clears dirty for the revision whose digest it echoes,
    // and only for it.
    void stagedFailureClearsDirtyOnlyForTheMatchingDigest()
    {
        if (mode().startsWith(QLatin1String("pkexec"))) QSKIP("pkexec clears the environment: the failure is staged in direct mode");
        auto b = backend();
        if (b->host) b->rig.firewall.fail[QStringLiteral("sync")] = QStringLiteral("staged failure");
        else b->real->env.insert(QStringLiteral("ASB_TEST_FAIL"), QStringLiteral("firewall:sync"));
        HelperClient client(*b->process);
        EditTracker t;
        t.markEdited();
        const QByteArray bytes = reformatted(document(*b)) + "\n\n";   // whitespace no other test sent
        const HelperClient::Outcome o = saveAndApply(client, *b, bytes, t.revision());
        QCOMPARE(o.kind, Kind::Done);
        QCOMPARE(o.result.outcome, Outcome::Uncommitted);
        QCOMPARE(o.result.persisted, Persisted::Staged);
        QCOMPARE(o.result.persistedSha256, sha256Hex(bytes).toLatin1());
        QVERIFY(b->committed() != bytes);
        QVERIFY(o.clearsDirty);
        // The very same response, judged for the editor's other revision (its bytes differ in whitespace only):
        // dirty stays.
        QVERIFY(b->process->last);
        const QByteArray other = compacted(bytes);
        EditTracker t2;
        t2.markEdited();
        const HelperClient::Outcome forOther = HelperClient::interpret(
            helper::Request{helper::Op::SaveAndApply, SaveRequest{other, Source{}, true}}, sha256Hex(other).toLatin1(),
            t2.revision(), *b->process->last);
        QCOMPARE(forOther.result.persisted, Persisted::Staged);
        QVERIFY(!forOther.clearsDirty);
        QVERIFY(t2.isDirty());
        QVERIFY(t.markSaved(o.revision));
        QVERIFY(!t.isDirty());
    }

    // (3) A digest mismatch keeps dirty: the helper persisted bytes other than the revision the editor now holds.
    // And a save the helper did not persist (pending precedence: NothingChanged, persisted None) clears nothing.
    void digestMismatchKeepsDirty()
    {
        if (mode() == QLatin1String("pkexec-denied")) QSKIP("the denied mode runs deniedByPolicyChangesNothing only");
        auto b = backend();
        HelperClient client(*b->process);
        const QByteArray sent = compacted(document(*b));
        EditTracker t;
        t.markEdited();
        const HelperClient::Outcome o = saveAndApply(client, *b, sent, t.revision());
        QCOMPARE(o.kind, Kind::Done);
        QCOMPARE(o.result.persistedSha256, sha256Hex(sent).toLatin1());
        // The editor's revision is the reformatted document: the response's digest is not its digest.
        const QByteArray held = reformatted(sent);
        const HelperClient::Outcome mismatch = HelperClient::interpret(
            helper::Request{helper::Op::SaveAndApply, SaveRequest{held, Source{}, true}}, sha256Hex(held).toLatin1(),
            t.revision(), *b->process->last);
        QCOMPARE(mismatch.kind, Kind::Done);
        QVERIFY(!mismatch.clearsDirty);
        QVERIFY(t.isDirty());

        // Not persisted at all: a pending file the editor did not load blocks the save (INV-102) unless superseded.
        std::optional<HelperClient::Outcome> got;
        QVERIFY(client.deactivate(reformatted(sent) + "\n", Source{Source::Committed, sha256Hex(b->committed()).toLatin1()},
                                  true, 0, [&](const HelperClient::Outcome& x) { got = x; }));
        const HelperClient::Outcome staged = wait(got);
        QCOMPARE(staged.result.outcome, Outcome::SavedNotApplied);
        t.markEdited();
        const HelperClient::Outcome blocked = saveAndApply(client, *b, sent + "\n", t.revision(), /*supersede=*/false);
        QCOMPARE(blocked.kind, Kind::Done);
        QCOMPARE(blocked.result.outcome, Outcome::NothingChanged);
        QCOMPARE(blocked.result.persisted, Persisted::None);
        QVERIFY(!blocked.clearsDirty);
        QVERIFY(t.isDirty());
        // Put the station back as the other cases expect it: apply what is committed.
        const HelperClient::Outcome restore = saveAndApply(client, *b, sent, t.revision());
        QCOMPARE(restore.result.outcome, Outcome::Ok);
    }

    // The helper's read-only view through the same path (pkexec: the policy maps the program to the action).
    void inspectRoundTrip()
    {
        if (mode() == QLatin1String("pkexec-denied")) QSKIP("the denied mode runs deniedByPolicyChangesNothing only");
        auto b = backend();
        HelperClient client(*b->process);
        std::optional<HelperClient::Outcome> got;
        QVERIFY(client.inspect([&](const HelperClient::Outcome& o) { got = o; }));
        const HelperClient::Outcome o = wait(got);
        QVERIFY2(o.kind == Kind::Done && o.inspect, qPrintable(o.message));
        QCOMPARE(o.inspect->trust, QStringLiteral("Trust"));
        QCOMPARE(o.inspect->installation, QStringLiteral("Current"));
    }

    // Under a NO rule pkexec refuses (127): the client says not authorized, and nothing changed.
    void deniedByPolicyChangesNothing()
    {
        if (mode() != QLatin1String("pkexec-denied")) QSKIP("runs under the suite's NO rule only");
        auto b = backend();
        const QByteArray before = b->committed();
        HelperClient client(*b->process);
        EditTracker t;
        t.markEdited();
        const HelperClient::Outcome o = saveAndApply(client, *b, reformatted(realConfig()) + " ", t.revision());
        QCOMPARE(o.kind, Kind::NotAuthorized);
        QVERIFY2(o.message.contains("nothing was changed"), qPrintable(o.message));
        QVERIFY(!o.clearsDirty);
        QCOMPARE(b->committed(), before);
    }
};

QTEST_GUILESS_MAIN(tst_HelperRoundTrip)
#include "tst_HelperRoundTrip.moc"
