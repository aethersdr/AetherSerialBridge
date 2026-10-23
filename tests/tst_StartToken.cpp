// start.token (S18, D52, INV-113), at the FileIo seam: the exact format, revocation order and failure
// classes, resurrection after a power loss, the atomic fresh write, and the read checks.

#include "core/ConfigFiles.h"
#include "core/StartToken.h"
#include "support/Fakes.h"

#include <QTest>

using namespace asb;
using namespace asb::test;

namespace {
const QString kToken = QString::fromLatin1(ConfigFiles::kStartToken);
const FirewallGeneration kGen{QByteArrayLiteral("0123456789abcdef")};
const QByteArray kValid = StartToken::format(QByteArrayLiteral("0123456789abcdef0123456789abcdef"), kGen);

struct TokenRig {
    FakeWorld world;
    FakeFolder folder{world};
    FileStartToken token{folder, world.files};
    TokenRig() { world.checkLock = false; }
};
}  // namespace

class tst_StartToken : public QObject {
    Q_OBJECT
private slots:
    void parseAcceptsOnlyTheExactForm_data();
    void parseAcceptsOnlyTheExactForm();
    void revocationOrder();
    void missingTokenIsNothingToRevoke();
    void aLinkIsNotOpenedAndNothingChanges();
    void aFailedFlushIsNotFlushed_data();
    void aFailedFlushIsNotFlushed();
    void revocationSurvivesResurrection();
    void aFailedDirectorySyncStillCountsAsRevoked();
    void writeFreshIsAnAtomicConfigPolicyWrite();
    void writeFreshReportsDurabilitySeparately();
    void readRefusesUnsafeFiles_data();
    void readRefusesUnsafeFiles();
    void readRefusesAnUntrustedFolder();
};

void tst_StartToken::parseAcceptsOnlyTheExactForm_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("ok");
    QTest::newRow("valid") << kValid << true;
    QTest::newRow("revoked") << QByteArray(StartToken::kRevoked) << false;
    QTest::newRow("empty") << QByteArray() << false;
    QTest::newRow("half-written") << kValid.left(30) << false;
    QTest::newRow("trailing newline") << kValid + '\n' << false;
    QTest::newRow("spaces") << QByteArray(R"({ "token":"0123456789abcdef0123456789abcdef","generation":"0123456789abcdef"})") << false;
    QTest::newRow("upper hex") << QByteArray(R"({"token":"0123456789ABCDEF0123456789abcdef","generation":"0123456789abcdef"})") << false;
    QTest::newRow("short generation") << QByteArray(R"({"token":"0123456789abcdef0123456789abcdef","generation":"0123"})") << false;
    QTest::newRow("reordered") << QByteArray(R"({"generation":"0123456789abcdef","token":"0123456789abcdef0123456789abcdef"})") << false;
    QTest::newRow("oversized") << kValid + QByteArray(300, ' ') << false;
    QTest::newRow("NUL") << QByteArray(kValid).replace(0, 1, QByteArray(1, '\0')) << false;
}

void tst_StartToken::parseAcceptsOnlyTheExactForm()
{
    QFETCH(QByteArray, bytes);
    QFETCH(bool, ok);
    QString why;
    const auto t = StartToken::parse(bytes, &why);
    QCOMPARE(t.has_value(), ok);
    if (ok) {
        QCOMPARE(t->token, QByteArrayLiteral("0123456789abcdef0123456789abcdef"));
        QCOMPARE(t->generation, kGen);
    } else {
        QVERIFY(!why.isEmpty());
    }
}

// INV-113 (N9-4). Kills: deleting before the file flush, or any reordering of the revocation.
void tst_StartToken::revocationOrder()
{
    TokenRig r;
    r.world.files.plantFile(kToken, kValid, QStringLiteral("root"));
    r.world.files.log.clear();
    QString err;
    QCOMPARE(r.token.revoke({}, &err), IStartToken::Revocation::Done);
    QCOMPARE(r.world.files.log, (QStringList{QStringLiteral("openExisting start.token"), QStringLiteral("truncate start.token"),
                                             QStringLiteral("writeAll start.token 16"), QStringLiteral("flushExisting start.token"),
                                             QStringLiteral("removeByHandle start.token"), QStringLiteral("syncDir")}));
    QVERIFY(!r.world.files.exists(kToken));
}

void tst_StartToken::missingTokenIsNothingToRevoke()
{
    TokenRig r;
    QString err;
    QCOMPARE(r.token.revoke({}, &err), IStartToken::Revocation::Absent);
}

void tst_StartToken::aLinkIsNotOpenedAndNothingChanges()
{
    TokenRig r;
    r.world.files.plantSymlink(kToken, QStringLiteral("/somewhere/else"));
    QString err;
    QCOMPARE(r.token.revoke({}, &err), IStartToken::Revocation::NotOpened);
    QCOMPARE(r.world.files.kind(kToken), FakeFileIo::Kind::Symlink);
}

// INV-113. Kills: reporting a failed truncate/write/flush as "revoked" or as "nothing changed".
void tst_StartToken::aFailedFlushIsNotFlushed_data()
{
    QTest::addColumn<int>("op");
    QTest::newRow("truncate") << int(FakeFileIo::Op::Truncate);
    QTest::newRow("write") << int(FakeFileIo::Op::WriteAll);
    QTest::newRow("flush") << int(FakeFileIo::Op::FlushExisting);
}

void tst_StartToken::aFailedFlushIsNotFlushed()
{
    QFETCH(int, op);
    TokenRig r;
    r.world.files.plantFile(kToken, kValid, QStringLiteral("root"));
    r.world.files.failNext(FakeFileIo::Op(op));
    QString err;
    QCOMPARE(r.token.revoke({}, &err), IStartToken::Revocation::NotFlushed);
    QVERIFY(!err.isEmpty());
    QVERIFY(!r.world.files.log.join(QLatin1Char(' ')).contains(QStringLiteral("removeByHandle")));
}

// INV-113. Kills: omitting the file flush (the resurrected entry would hold the old, valid token).
void tst_StartToken::revocationSurvivesResurrection()
{
    TokenRig r;
    r.world.files.plantFile(kToken, kValid, QStringLiteral("root"));
    r.world.files.failNext(FakeFileIo::Op::SyncDir);   // the deletion never becomes durable
    QString err;
    QCOMPARE(r.token.revoke({}, &err), IStartToken::Revocation::Done);
    QVERIFY(!r.world.files.exists(kToken));
    r.world.files.crash();
    QVERIFY(r.world.files.exists(kToken));                       // resurrected ...
    QCOMPARE(r.world.files.contents(kToken), QByteArray(StartToken::kRevoked));   // ... in the revoked form
    QString why;
    QVERIFY(!r.token.read(&why));
    QVERIFY(why.contains(QStringLiteral("revoked")));
}

void tst_StartToken::aFailedDirectorySyncStillCountsAsRevoked()
{
    TokenRig r;
    r.world.files.plantFile(kToken, kValid, QStringLiteral("root"));
    r.world.files.failNext(FakeFileIo::Op::RemoveByHandle);
    QString err;
    QCOMPARE(r.token.revoke({}, &err), IStartToken::Revocation::Done);
    QCOMPARE(r.world.files.contents(kToken), QByteArray(StartToken::kRevoked));
}

void tst_StartToken::writeFreshIsAnAtomicConfigPolicyWrite()
{
    TokenRig r;
    const IStartToken::Written w = r.token.writeFresh(kGen, {});
    QVERIFY(w.replaced && w.durable);
    QCOMPARE(w.token.size(), 32);
    QCOMPARE(r.world.files.owner(kToken), QStringLiteral("root"));
    const auto t = StartToken::parse(r.world.files.contents(kToken), nullptr);
    QVERIFY(t);
    QCOMPARE(t->token, w.token);
    QCOMPARE(t->generation, kGen);
    const QString log = r.world.files.log.join(QLatin1Char('|'));
    QVERIFY(log.contains(QStringLiteral("Config")));
    QVERIFY(log.indexOf(QStringLiteral("flush ")) < log.indexOf(QStringLiteral("rename ")));
    QVERIFY(r.token.writeFresh(kGen, {}).token != w.token);
}

void tst_StartToken::writeFreshReportsDurabilitySeparately()
{
    TokenRig r;
    r.world.files.failNext(FakeFileIo::Op::SyncDir);
    const IStartToken::Written w = r.token.writeFresh(kGen, {});
    QVERIFY(w.replaced);
    QVERIFY(!w.durable);
    r.world.files.crash();
    QVERIFY(!r.world.files.exists(kToken));   // fails closed: no token -> exit 6
}

// INV-113 (the --service read): wrong owner, writable by others, link, directory, oversized.
void tst_StartToken::readRefusesUnsafeFiles_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("missing") << QStringLiteral("missing");
    QTest::newRow("wrong owner") << QStringLiteral("owner");
    QTest::newRow("writable") << QStringLiteral("writable");
    QTest::newRow("link") << QStringLiteral("link");
    QTest::newRow("oversized") << QStringLiteral("oversized");
}

void tst_StartToken::readRefusesUnsafeFiles()
{
    QFETCH(QString, kind);
    TokenRig r;
    if (kind == QLatin1String("owner")) r.world.files.plantFile(kToken, kValid, QStringLiteral("process"));
    if (kind == QLatin1String("writable")) {
        r.world.files.plantFile(kToken, kValid, QStringLiteral("root"));
        r.folder.writableByOthers << kToken;
    }
    if (kind == QLatin1String("link")) r.world.files.plantSymlink(kToken, QStringLiteral("elsewhere"));
    if (kind == QLatin1String("oversized")) r.world.files.plantFile(kToken, kValid + QByteArray(300, ' '), QStringLiteral("root"));
    // "process" is a trusted owner of the folder (the service's own files) but not the Config owner the
    // token needs; the folder fake re-checks per file, as the adapters do.
    if (kind == QLatin1String("owner")) r.folder.policy.trustedOwners = {QStringLiteral("root")};
    QString why;
    QVERIFY(!r.token.read(&why));
    QVERIFY(!why.isEmpty());
}

void tst_StartToken::readRefusesAnUntrustedFolder()
{
    TokenRig r;
    r.world.files.plantFile(kToken, kValid, QStringLiteral("root"));
    r.folder.owner = QStringLiteral("attacker");
    QString why;
    QVERIFY(!r.token.read(&why));
    QVERIFY(why.contains(QStringLiteral("not trusted")));
}

QTEST_GUILESS_MAIN(tst_StartToken)
#include "tst_StartToken.moc"
