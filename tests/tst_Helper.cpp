// `--helper` (WS-9b; master §5.7; INV-50, 51, 89, 112, 118) over a fake Host and the WS-5 fakes: mode entry comes
// first (guard, probe, root), then the bounded request, and only an accepted request builds the Platform; a refused
// request reads nothing more, writes nothing on stdout and touches nothing; each op reaches its Applier operation;
// the response is the Applier's Result, written once, after `finished`, with persistedSha256 over the exact bytes
// sent. The native halves (real root, real pkexec, the 200 MB flood under an address-space limit) are the systemd
// suite's helper_* cases.

#include "cli/Helper.h"
#include "cli/HelperProtocol.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "support/FakeCliHost.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <condition_variable>
#include <mutex>

#include <fcntl.h>
#include <unistd.h>

using namespace asb;
using namespace asb::cli;
using namespace asb::test;

namespace {

const QString kPending = QString::fromLatin1(ConfigFiles::kPending);
const QString kCommitted = QStringLiteral("config.json");

struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void enter()
    {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return open; });
    }
    void release()
    {
        std::lock_guard g(m);
        open = true;
        cv.notify_all();
    }
};

QByteArray compact(const QByteArray& json)
{
    return QJsonDocument::fromJson(json).toJson(QJsonDocument::Compact);
}

QByteArray save(helper::Op op, const QByteArray& bytes, Source::Kind kind = Source::New, const QByteArray& sha = {},
                bool supersede = true)
{
    helper::Request r;
    r.op = op;
    r.save = SaveRequest{bytes, Source{kind, sha}, supersede};
    return helper::encodeRequest(r);
}

QByteArray plain(helper::Op op)
{
    return helper::encodeRequest(helper::Request{op, std::nullopt});
}

// One helper run: `input` is its stdin (a file: EOF at its end), its stdout is captured.
struct Run {
    int code = -1;
    QByteArray out;
    qint64 inputOffset = -1;   // how far the helper read
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
};

class Harness {
public:
    Harness() { QVERIFY(m_dir.isValid()); }
    Run run(FakeCliHost& h, CapturingConsole& c, const QByteArray& input, HelperOptions o = {})
    {
        const QString inPath = m_dir.filePath(QStringLiteral("in-%1").arg(m_n));
        const QString outPath = m_dir.filePath(QStringLiteral("out-%1").arg(m_n++));
        {
            QFile f(inPath);
            if (!f.open(QIODevice::WriteOnly) || f.write(input) != input.size()) qFatal("cannot write the input");
        }
        const int in = ::open(QFile::encodeName(inPath).constData(), O_RDONLY | O_CLOEXEC);
        const int out = ::open(QFile::encodeName(outPath).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        o.inputFd = in;
        o.outputFd = out;
        o.syslog = false;
        m_outPath = outPath;
        Run r;
        r.code = runHelper(h, c, o);
        r.inputOffset = ::lseek(in, 0, SEEK_CUR);
        ::close(in);
        ::close(out);
        QFile f(outPath);
        if (f.open(QIODevice::ReadOnly)) r.out = f.readAll();
        return r;
    }
    // The output file of the run in progress (read from a callback during the run).
    QByteArray currentOutput() const
    {
        QFile f(m_outPath);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

private:
    QTemporaryDir m_dir;
    QString m_outPath;
    int m_n = 0;
};

}  // namespace

class tst_Helper : public QObject {
    Q_OBJECT

private slots:
    void init() { qunsetenv("PKEXEC_UID"); }

    // ---- mode entry and authorization ------------------------------------------------------------
    void modeEntryComesFirst()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, plain(helper::Op::Stop));
        QCOMPARE(run.code, 0);
        QCOMPARE(h.calls, (QStringList{"guard", "probe", "elevated", "platform"}));
        QVERIFY(r.world.log.contains(QStringLiteral("service:requestStop")));
        QCOMPARE(run.json().value("ok").toBool(), true);
    }
    // INV-50: root only; PKEXEC_UID is never authority. Refused before a byte of the request is read.
    void notRootIsRefusedBeforeReading()
    {
        qputenv("PKEXEC_UID", "0");
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.isElevated = false;
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, plain(helper::Op::Stop));
        QCOMPARE(run.code, 2);
        QVERIFY(run.out.isEmpty());
        QCOMPARE(run.inputOffset, qint64(0));
        QCOMPARE(h.calls, (QStringList{"guard", "probe", "elevated"}));
        QVERIFY2(c.errText().contains("--helper runs only as root, started by pkexec; nothing was changed."), qPrintable(c.errText()));
        QVERIFY(r.world.log.all().isEmpty());
    }
    // The audit line names the requester from PKEXEC_UID (digits only); it decides nothing.
    void auditLineNamesTheRequester()
    {
        qputenv("PKEXEC_UID", "1000");
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        QCOMPARE(x.run(h, c, plain(helper::Op::Stop)).code, 0);
        QVERIFY2(c.errText().contains("aether-serial-bridge --helper: stop requested by uid 1000"), qPrintable(c.errText()));
        qputenv("PKEXEC_UID", "0; rm -rf /");
        CapturingConsole c2;
        QCOMPARE(x.run(h, c2, plain(helper::Op::Stop)).code, 0);
        QVERIFY2(c2.errText().contains("stop requested by a caller pkexec did not name"), qPrintable(c2.errText()));
    }

    // ---- the request (INV-89): refused input touches nothing ------------------------------------------
    void refusedRequestTouchesNothing_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QString>("why");
        const QByteArray b = compact(configB());
        const QByteArray ok = save(helper::Op::SaveAndApply, b);
        QTest::newRow("too large") << QByteArray(helper::kMaxRequestBytes + 1, ' ') << "the request is too large (more than 262144 bytes)";
        QTest::newRow("too large, valid prefix") << (plain(helper::Op::Stop) + QByteArray(helper::kMaxRequestBytes, ' '))
                                                 << "the request is too large";
        QTest::newRow("empty (the GUI died)") << QByteArray() << "the request is not valid";
        QTest::newRow("truncated (the GUI died)") << ok.left(ok.size() / 2) << "the request is not valid";
        QTest::newRow("two values") << QByteArray(R"({"op":"stop"}{"op":"start"})") << "not one JSON value";
        QTest::newRow("two values, spaced") << QByteArray("{\"op\":\"stop\"}\n{\"op\":\"start\"}\n") << "not one JSON value";
        QTest::newRow("trailing garbage") << QByteArray(R"({"op":"stop"} x)") << "not one JSON value";
        QTest::newRow("trailing NUL") << QByteArray("{\"op\":\"stop\"}\0", 14) << "not one JSON value";
        QTest::newRow("array") << QByteArray(R"([{"op":"stop"}])") << "not a JSON object";
        QTest::newRow("unknown op") << QByteArray(R"({"op":"format-disk"})") << "unknown op \"format-disk\"";
        QTest::newRow("op twice") << QByteArray(R"({"op":"inspect","op":"stop"})") << "\"op\" is given twice";
        QTest::newRow("member twice, escaped") << QByteArray(R"({"op":"stop","\u006fp":"start"})") << "\"op\" is given twice";
        QTest::newRow("unknown member") << QByteArray(R"({"op":"stop","force":true})") << "unknown member \"force\"";
        QTest::newRow("stop with a config") << QByteArray(R"({"op":"stop","configBytes":"e30="})") << "unknown member \"configBytes\"";
        QTest::newRow("save without source") << QByteArray(R"({"op":"save-and-apply","configBytes":"e30="})") << "source must be an object";
        QTest::newRow("save without bytes") << QByteArray(R"({"op":"save-and-apply","source":{"kind":"new"}})") << "configBytes must be a base64 string";
        QTest::newRow("base64 with a newline") << QByteArray("{\"op\":\"save-and-apply\",\"configBytes\":\"e30=\\n\",\"source\":{\"kind\":\"new\"}}") << "configBytes is not exact base64";
        QTest::newRow("base64 without padding") << QByteArray(R"({"op":"save-and-apply","configBytes":"e30","source":{"kind":"new"}})") << "configBytes is not exact base64";
        QTest::newRow("base64 url alphabet") << QByteArray(R"({"op":"save-and-apply","configBytes":"_-8=","source":{"kind":"new"}})") << "configBytes is not exact base64";
        QTest::newRow("bad kind") << QByteArray(R"({"op":"save-and-apply","configBytes":"e30=","source":{"kind":"disk"}})") << "source.kind";
        QTest::newRow("pending without sha") << QByteArray(R"({"op":"save-and-apply","configBytes":"e30=","source":{"kind":"pending"}})") << "source.sha256 is required";
        QTest::newRow("upper-case sha") << (QByteArray(R"({"op":"save-and-apply","configBytes":"e30=","source":{"kind":"committed","sha256":")") + QByteArray(64, 'A') + "\"}}") << "source.sha256 must be 64 lower-case hex digits";
        QTest::newRow("supersede not a bool") << QByteArray(R"({"op":"save-and-apply","configBytes":"e30=","source":{"kind":"new"},"supersedePending":1})") << "supersedePending must be true or false";
        QTest::newRow("deactivate source alone") << QByteArray(R"({"op":"deactivate","source":{"kind":"new"}})") << "source and supersedePending go with configBytes";
        QTest::newRow("unknown source member") << QByteArray(R"({"op":"save-and-apply","configBytes":"e30=","source":{"kind":"new","path":"/x"}})") << "unknown member \"path\"";
    }
    void refusedRequestTouchesNothing()
    {
        QFETCH(QByteArray, input);
        QFETCH(QString, why);
        Rig r;
        r.installedRunning(configA());
        r.world.log.clear();
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, input);
        QCOMPARE(run.code, 2);
        QVERIFY2(run.out.isEmpty(), run.out.constData());
        QVERIFY2(c.errText().contains(why) && c.errText().endsWith("; nothing was changed."), qPrintable(c.errText()));
        QVERIFY(!h.calls.contains(QStringLiteral("platform")));
        QVERIFY2(r.world.log.all().isEmpty(), qPrintable(r.world.log.all().join('\n')));
        QVERIFY(run.inputOffset <= helper::kMaxRequestBytes + 1);   // never past the cap
    }
    // The cap is cumulative and exact: 262,144 bytes are a request, one more is not.
    void requestOfExactlyTheCapIsAccepted()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        QByteArray input = plain(helper::Op::Stop);
        input += QByteArray(helper::kMaxRequestBytes - input.size(), ' ');
        QCOMPARE(input.size(), helper::kMaxRequestBytes);
        QCOMPARE(x.run(h, c, input).code, 0);
        QVERIFY(r.world.log.contains(QStringLiteral("service:requestStop")));
    }
    // One deadline for the whole request: an input that never ends is refused when it passes.
    void inputWithoutEofIsRefusedAtTheDeadline()
    {
        Rig r;
        r.installedRunning(configA());
        r.world.log.clear();
        FakeCliHost h(r);
        CapturingConsole c;
        int fds[2];
        QCOMPARE(::pipe(fds), 0);
        const QByteArray partial = plain(helper::Op::Stop);
        QCOMPARE(::write(fds[1], partial.constData(), size_t(partial.size())), ssize_t(partial.size()));   // no EOF
        HelperOptions o;
        o.inputFd = fds[0];
        o.outputFd = fds[1];   // never written: the request is refused
        o.inputDeadline = std::chrono::milliseconds(300);
        o.syslog = false;
        QElapsedTimer t;
        t.start();
        QCOMPARE(runHelper(h, c, o), 2);
        QVERIFY2(t.elapsed() >= 290 && t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));
        QVERIFY2(c.errText().contains("the request did not end within 0.3 s; nothing was changed."), qPrintable(c.errText()));
        QVERIFY(!h.calls.contains(QStringLiteral("platform")));
        QVERIFY(r.world.log.all().isEmpty());
        ::close(fds[0]);
        ::close(fds[1]);
    }

    // ---- the operations ---------------------------------------------------------------------------
    // The digest convention: persistedSha256 is over the exact bytes sent, and config.json holds exactly them
    // (two equivalent documents that differ in whitespace are two different revisions; nothing is reserialised).
    void saveAndApplyCommitsTheExactBytes()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        Harness x;
        const QByteArray one = compact(configB());
        const QByteArray two = configB();   // the same document, indented
        QVERIFY(one != two);
        for (const QByteArray& bytes : {one, two}) {
            CapturingConsole c;
            const Run run = x.run(h, c, save(helper::Op::SaveAndApply, bytes, Source::Committed, digest(r.file(kCommitted))));
            QCOMPARE(run.code, 0);
            const QJsonObject o = run.json();
            QCOMPARE(o.value("ok").toBool(), true);
            QCOMPARE(o.value("outcome").toString(), QStringLiteral("Ok"));
            QCOMPARE(o.value("persisted").toString(), QStringLiteral("Committed"));
            QCOMPARE(o.value("persistedSha256").toString().toLatin1(), digest(bytes));
            QCOMPARE(r.file(kCommitted), bytes);
            QVERIFY2(c.errText().contains(QStringLiteral("(config sha256 %1)").arg(QString::fromLatin1(digest(bytes)))), qPrintable(c.errText()));
        }
        QVERIFY(digest(one) != digest(two));
    }
    // Untick (INV-51's Applier half): the edits become pending, the service is stopped and blocked, never removed.
    void deactivateKeepsTheEditsAsPending()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, save(helper::Op::Deactivate, configB(), Source::Committed, digest(configA())));
        QCOMPARE(run.code, 0);
        QCOMPARE(run.json().value("outcome").toString(), QStringLiteral("SavedNotApplied"));
        QCOMPARE(run.json().value("persisted").toString(), QStringLiteral("Staged"));
        QCOMPARE(r.file(kPending), configB());
        QCOMPARE(r.file(kCommitted), configA());
        QVERIFY(r.world.log.contains(QStringLiteral("service:inhibitStarts")));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:uninstall")));
    }
    void deactivateWithoutEdits()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, plain(helper::Op::Deactivate));
        QCOMPARE(run.code, 0);
        QCOMPARE(run.json().value("persisted").toString(), QStringLiteral("None"));   // nothing submitted, nothing saved
        QVERIFY(r.world.log.contains(QStringLiteral("service:inhibitStarts")));
        QVERIFY(!r.has(kPending));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:uninstall")));
    }
    void eachOpReachesItsOperation_data()
    {
        QTest::addColumn<int>("op");
        QTest::addColumn<QString>("event");
        QTest::newRow("start") << int(helper::Op::Start) << "service:start";
        QTest::newRow("stop") << int(helper::Op::Stop) << "service:requestStop";
        QTest::newRow("repair") << int(helper::Op::Repair) << "folder:inspect";
        QTest::newRow("discard-pending") << int(helper::Op::DiscardPending) << "applyLock:acquired";
    }
    void eachOpReachesItsOperation()
    {
        QFETCH(int, op);
        QFETCH(QString, event);
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, plain(helper::Op(op)));
        QVERIFY2(r.world.log.contains(event), qPrintable(r.world.log.all().join('\n')));
        bool ok = false;
        QString why;
        const std::optional<Result> res = helper::parseResultResponse(run.out, &ok, &why);
        QVERIFY2(res, qPrintable(why));
        QCOMPARE(run.code, ok ? 0 : 1);
        QVERIFY(!r.world.applyLock.held());
    }
    // A failed operation is a response with "ok":false and exit 1 (not a refusal).
    void failedOperationIsAResponse()
    {
        Rig r;
        r.installedRunning(configA());
        r.firewall.fail[QStringLiteral("sync")] = QStringLiteral("injected");
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        const QByteArray b = compact(configB());
        const Run run = x.run(h, c, save(helper::Op::SaveAndApply, b));
        QCOMPARE(run.code, 1);
        const QJsonObject o = run.json();
        QCOMPARE(o.value("ok").toBool(), false);
        QCOMPARE(o.value("outcome").toString(), QStringLiteral("Uncommitted"));
        QCOMPARE(o.value("persisted").toString(), QStringLiteral("Staged"));
        QCOMPARE(o.value("persistedSha256").toString().toLatin1(), digest(b));
        QVERIFY(!o.value("step").toString().isEmpty());
    }
    // inspect is read only: no apply lock, no operation, no write.
    void inspectIsReadOnly()
    {
        Rig r;
        r.installedRunning(configA());
        r.world.checkLock = false;   // inspect deliberately runs without the apply lock
        r.world.log.clear();
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, plain(helper::Op::Inspect));
        QCOMPARE(run.code, 0);
        bool ok = false;
        QString why;
        const std::optional<helper::InspectReport> rep = helper::parseInspectResponse(run.out, &ok, &why);
        QVERIFY2(rep && ok, qPrintable(why + run.out));
        QCOMPARE(rep->trust, QStringLiteral("Trust"));
        QCOMPARE(rep->installation, QStringLiteral("Current"));
        QCOMPARE(rep->service, QStringLiteral("Running"));
        QCOMPARE(QJsonDocument::fromJson(run.out).object().keys(),
                 (QStringList{"installation", "ok", "reason", "service", "trust"}));
        QVERIFY(!r.world.log.contains(QStringLiteral("applyLock:")));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:requestStop")));
        r.folder.owner = QStringLiteral("attacker");
        CapturingConsole c2;
        const Run untrusted = x.run(h, c2, plain(helper::Op::Inspect));
        QCOMPARE(QJsonDocument::fromJson(untrusted.out).object().value("trust").toString(), QStringLiteral("Recreate"));
        QVERIFY(!QJsonDocument::fromJson(untrusted.out).object().value("reason").toString().isEmpty());
    }
    // What the adapters reported (lock files, the unit, the binary, the policy) joins fileChanges once.
    void adapterReportsJoinFileChangesOnce()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        h.reports << QStringLiteral("wrote /usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy")
                  << QStringLiteral("wrote /usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy");
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, plain(helper::Op::Stop));
        QCOMPARE(run.json().value("fileChanges").toArray().size(), 1);
    }
    void platformFailureIsAResponse()
    {
        Rig r;
        FakeCliHost h(r);
        h.platformError = QStringLiteral("cannot determine the data folder");
        CapturingConsole c;
        Harness x;
        const Run run = x.run(h, c, plain(helper::Op::Start));
        QCOMPARE(run.code, 1);
        QCOMPARE(run.json().value("outcome").toString(), QStringLiteral("NothingChanged"));
        QVERIFY(run.json().value("message").toString().contains("cannot determine the data folder"));
    }


    // ---- v1.1 (WS-15): the new ops; install and uninstall only in a staged program (INV-123) ---------------------
    void lifecycleOpsOnlyStaged_data()
    {
        QTest::addColumn<QString>("own");
        QTest::addColumn<bool>("runs");
        const QString root = QStringLiteral("/usr/local/lib/aether-serial-bridge/");
        QTest::newRow("staged") << root + "staging.ab12CD34/aether-serial-bridge" << true;
        QTest::newRow("staged, digits only") << root + "staging.01234567/aether-serial-bridge" << true;
        QTest::newRow("installed program") << QStringLiteral("/usr/local/bin/aether-serial-bridge") << false;
        QTest::newRow("7-character suffix") << root + "staging.ab12CD3/aether-serial-bridge" << false;
        QTest::newRow("9-character suffix") << root + "staging.ab12CD345/aether-serial-bridge" << false;
        QTest::newRow("punctuation in the suffix") << root + "staging.ab12CD-4/aether-serial-bridge" << false;
        QTest::newRow("an extra path level") << root + "staging.ab12CD34/x/aether-serial-bridge" << false;
        QTest::newRow("another file name") << root + "staging.ab12CD34/aether-serial-bridge-gui" << false;
        QTest::newRow("another root") << QStringLiteral("/tmp/aether-serial-bridge/staging.ab12CD34/aether-serial-bridge") << false;
        QTest::newRow("a relative path") << QStringLiteral("usr/local/lib/aether-serial-bridge/staging.ab12CD34/aether-serial-bridge") << false;
        QTest::newRow("a deleted file") << root + "staging.ab12CD34/aether-serial-bridge (deleted)" << false;
        QTest::newRow("unknown") << QString() << false;
    }
    void lifecycleOpsOnlyStaged()
    {
        QFETCH(QString, own);
        QFETCH(bool, runs);
        helper::Request uninstall;
        uninstall.op = helper::Op::Uninstall;
        helper::Request installBare;
        installBare.op = helper::Op::Install;
        const QByteArray inputs[] = {helper::encodeRequest(uninstall), helper::encodeRequest(installBare),
                                     save(helper::Op::Install, compact(configB()))};
        for (const QByteArray& input : inputs) {
            Rig r;
            r.installedRunning(configA());
            r.world.log.clear();
            FakeCliHost h(r);
            h.own = own;
            CapturingConsole c;
            Harness x;
            const Run run = x.run(h, c, input);
            if (runs) {
                QVERIFY2(run.code == 0 || run.code == 1, qPrintable(c.errText()));
                QVERIFY(h.calls.contains(QStringLiteral("platform")));
                QVERIFY2(c.errText().contains(QStringLiteral("aether-serial-bridge --helper, staged: ")), qPrintable(c.errText()));
                bool ok = false;
                QString why;
                QVERIFY2(helper::parseResultResponse(run.out, &ok, &why), qPrintable(why));
                QVERIFY2(r.world.log.contains(input.contains("uninstall") ? QStringLiteral("service:uninstall")
                                                                          : QStringLiteral("service:installOrConfigure")),
                         qPrintable(r.world.log.all().join('\n')));
            } else {
                QCOMPARE(run.code, 2);
                QVERIFY2(run.out.isEmpty(), run.out.constData());
                QVERIFY2(c.errText().contains(QStringLiteral("error: install and uninstall run only from the window's staged "
                                                             "program; nothing was changed.")),
                         qPrintable(c.errText()));
                QCOMPARE(h.calls, (QStringList{"guard", "probe", "elevated"}));   // no Platform
                QVERIFY2(r.world.log.all().isEmpty(), qPrintable(r.world.log.all().join('\n')));
            }
        }
    }
    // The other new op runs in an installed helper too (a 1.1+ installation's Discard, INV-127).
    void discardPendingIfRunsInTheInstalledHelper()
    {
        Rig r;
        r.installedRunning(configA());
        const QByteArray b = compact(configB());
        r.world.files.plantFile(kPending, b, QStringLiteral("root"));
        FakeCliHost h(r);
        h.own = QStringLiteral("/usr/local/bin/aether-serial-bridge");
        Harness x;
        helper::Request other;
        other.op = helper::Op::DiscardPendingIf;
        other.sha256 = QString(64, QLatin1Char('0'));
        CapturingConsole c;
        const Run kept = x.run(h, c, helper::encodeRequest(other));
        QCOMPARE(kept.code, 1);
        QCOMPARE(kept.json().value("outcome").toString(), QStringLiteral("NothingChanged"));
        QVERIFY(r.has(kPending));
        helper::Request mine = other;
        mine.sha256 = QString::fromLatin1(digest(b));
        CapturingConsole c2;
        const Run gone = x.run(h, c2, helper::encodeRequest(mine));
        QCOMPARE(gone.code, 0);
        QVERIFY(!r.has(kPending));
        QVERIFY2(c2.errText().contains(QStringLiteral("aether-serial-bridge --helper: discard-pending-if requested by")),
                 qPrintable(c2.errText()));
    }
    // INV-124: the helper exits only 0, 1, 2 or 5, every op through every exit path the fakes reach: a refusal (not
    // root, an invalid request, a lifecycle op outside a staged program), a Platform failure, an operation failure and
    // a success. (5, the guard, ends the process inside the Host's enterGuardedMode: guard_installed_before_first_object
    // _helper is that path.) So 3, 4 and 6 can only be the staging script's.
    void exitCodesWithinContract()
    {
        const QString staged = QStringLiteral("/usr/local/lib/aether-serial-bridge/staging.Zz09Zz09/aether-serial-bridge");
        QList<QByteArray> requests;
        for (helper::Op op : {helper::Op::Start, helper::Op::Stop, helper::Op::Repair, helper::Op::DiscardPending,
                              helper::Op::Inspect, helper::Op::Deactivate, helper::Op::Install})
            requests << plain(op);
        requests << save(helper::Op::SaveAndApply, compact(configB())) << save(helper::Op::Deactivate, compact(configB()))
                 << save(helper::Op::Install, compact(configB()));
        for (const bool remove : {false, true}) {
            helper::Request u;
            u.op = helper::Op::Uninstall;
            u.removeSettings = remove;
            requests << helper::encodeRequest(u);
        }
        helper::Request d;
        d.op = helper::Op::DiscardPendingIf;
        d.sha256 = QString(64, QLatin1Char('1'));
        requests << helper::encodeRequest(d);
        enum Path { Success, OperationFails, PlatformFails, NotRoot, Invalid, NotStaged };
        const QStringList failing{"detect", "query", "installOrConfigure", "uninstall", "start", "requestStop", "removeProgram",
                                  "mask", "unmask", "enable", "disable"};
        int ran = 0;
        for (const QByteArray& request : requests) {
            for (Path path : {Success, OperationFails, PlatformFails, NotRoot, Invalid, NotStaged}) {
                Rig r;
                r.installedRunning(configA());
                if (path == OperationFails)
                    for (const QString& m : failing) r.service.fail[m] = QStringLiteral("injected");
                FakeCliHost h(r);
                h.own = path == NotStaged ? QStringLiteral("/usr/local/bin/aether-serial-bridge") : staged;
                h.isElevated = path != NotRoot;
                if (path == PlatformFails) h.platformError = QStringLiteral("injected");
                QByteArray input = request;
                if (path == Invalid) input.insert(1, "\"x\":1,");
                CapturingConsole c;
                Harness x;
                const Run run = x.run(h, c, input);
                QVERIFY2(run.code == 0 || run.code == 1 || run.code == 2 || run.code == 5,
                         qPrintable(QStringLiteral("%1 (path %2): exit %3; %4")
                                        .arg(QString::fromLatin1(request.trimmed())).arg(int(path)).arg(run.code).arg(c.errText())));
                if (path == NotRoot || path == Invalid) QCOMPARE(run.code, 2);
                ++ran;
            }
        }
        QCOMPARE(ran, requests.size() * 6);
    }

    // ---- lifetime (D57, INV-118): the response only after `finished` --------------------------------
    void responseOnlyAfterFinal()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Harness x;
        Gate gate;
        QByteArray outputAtInterim = "<no interim>";
        // The firewall sync outlasts its 60 s phase deadline (fake time), then blocks until the interim was printed.
        r.firewall.onCall = [&](const QString& m) {
            if (m != QLatin1String("sync")) return;
            r.world.clock.advance(std::chrono::seconds(61));
            gate.enter();
        };
        c.onErr = [&](const QString& line) {
            if (!line.contains(QLatin1String("still processing"))) return;
            outputAtInterim = x.currentOutput();
            gate.release();
        };
        struct OpenOnExit {
            Gate& g;
            ~OpenOnExit() { g.release(); }
        } openOnExit{gate};
        const Run run = x.run(h, c, save(helper::Op::SaveAndApply, compact(configB())));
        QCOMPARE(outputAtInterim, QByteArray());          // nothing on stdout at the interim
        QCOMPARE(run.out.count('\n'), 1);                 // one response, after the final
        QCOMPARE(run.json().value("outcome").toString(), QStringLiteral("StateUnknown"));
        QVERIFY2(!run.json().value("step").toString().isEmpty() && !run.json().value("postcondition").toString().isEmpty(),
                 run.out.constData());   // the Applier's final, with what its cleanup proved
        QCOMPARE(run.code, 1);
        QVERIFY(!r.world.applyLock.held());
    }
};

QTEST_GUILESS_MAIN(tst_Helper)
#include "tst_Helper.moc"
