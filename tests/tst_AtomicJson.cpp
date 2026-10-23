// INV-15 (exclusive create, no reuse of a planted name, temp removed on failure), INV-77 (durability
// barriers in order) and INV-96 (explicit ownership policy), at the FileIo seam. A planted file uses
// the injected generated name. The same contract checks run against the
// native FileIo implementations in WS-6 and WS-8 (FileIoContract.h).

#include "core/AtomicJson.h"
#include "core/ConfigFiles.h"
#include "core/StatusStore.h"
#include "support/FakeFileIo.h"
#include "support/FileIoContract.h"

#include <QRegularExpression>
#include <QSet>
#include <QTest>

using namespace asb;
using asb::test::FakeDir;
using asb::test::FakeFileIo;

namespace {

const QString kTemp = QStringLiteral("config.json.0123456789abcdef0123456789abcdef.asbtmp");

class FakeHarness : public asb::test::FileIoHarness {
public:
    FileIo& io() override { return fake; }
    const DirHandle& dir() override { return d; }
    void plantFile(const QString& n, const QByteArray& b) override { fake.plantFile(n, b); }
    void plantSymlink(const QString& n, const QString& t) override { fake.plantSymlink(n, t); }
    void plantDirectory(const QString& n) override { fake.plantDirectory(n); }
    std::optional<QByteArray> read(const QString& n) override
    {
        if (fake.kind(n) != FakeFileIo::Kind::File) return std::nullopt;
        return fake.contents(n);
    }
    QStringList names() override { return fake.names(); }

    FakeFileIo fake;
    FakeDir d;
};

// The log without the generated part of temp names: "<base>.<hex>.asbtmp" -> "<base>.TMP".
QStringList normalised(const QStringList& log)
{
    static const QRegularExpression tmp(QStringLiteral(R"(\.[0-9a-f]{32}\.asbtmp)"));
    QStringList out;
    for (QString l : log) out << l.replace(tmp, QStringLiteral(".TMP"));
    return out;
}

}  // namespace

class tst_AtomicJson : public QObject {
    Q_OBJECT

private slots:
    // INV-15
    void plantedGeneratedNameNotReused();
    void plantedSymlinkNotFollowed();
    void tempRemovedOnFailure_data();
    void tempRemovedOnFailure();
    void repeatedSavesLeaveOnlyTheTarget();
    void generatorCalledOncePerWrite();
    void contractOnTheFake();
    void defaultNamesAreFreshAndRecognisable();
    // INV-77
    void durabilityBarriersInOrder();
    void replacementAndDurabilityReportedSeparately();
    void failureBeforeRenameLeavesTheTargetUntouched();
    // INV-96
    void runtimePolicyNeverChowns();
    void configPolicyChownsBeforeRename();
    void policyPassedThroughUnchanged_data();
    void policyPassedThroughUnchanged();
    // the fake's crash model (WS-5 builds its INV-108/113 fixtures on it)
    void crashKeepsOnlyDurableState();
};

// Kills: writing through a fixed or reused temp name (the planted file's owner and ACL would become the
// target's). With the native FileIo (WS-6/WS-8), also kills dropping O_EXCL / CREATE_NEW.
void tst_AtomicJson::plantedGeneratedNameNotReused()
{
    FakeHarness h;
    asb::test::FileIoContract::plantedGeneratedNameNotReused(h);
    if (QTest::currentTestFailed()) return;
    // Nothing was written to, truncated or removed at the planted name.
    for (const QString& l : h.fake.log) QVERIFY2(!l.startsWith(QStringLiteral("write ")), qPrintable(l));
    QCOMPARE(h.fake.owner(kTemp), QStringLiteral("attacker"));
    QCOMPARE(h.fake.names(), QStringList{kTemp});
}

void tst_AtomicJson::plantedSymlinkNotFollowed()
{
    FakeHarness h;
    asb::test::FileIoContract::plantedSymlinkNotFollowed(h);
}

// Kills: leaving the temp file behind when a later step fails.
void tst_AtomicJson::tempRemovedOnFailure_data()
{
    QTest::addColumn<int>("op");
    QTest::newRow("write") << int(FakeFileIo::Op::Write);
    QTest::newRow("flush") << int(FakeFileIo::Op::Flush);
    QTest::newRow("rename") << int(FakeFileIo::Op::Rename);
}

void tst_AtomicJson::tempRemovedOnFailure()
{
    QFETCH(int, op);
    FakeFileIo io;
    FakeDir dir;
    io.plantFile(QStringLiteral("config.json"), "OLD", QStringLiteral("root"));
    io.failNext(FakeFileIo::Op(op));
    const WriteResult r = atomicWrite(io, dir, QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
    QVERIFY(!r.replaced);
    QVERIFY(!r.durable);
    QCOMPARE(r.error, QStringLiteral("injected failure"));
    QCOMPARE(io.tempNames(), QStringList());
    QCOMPARE(io.names(), QStringList{QStringLiteral("config.json")});
    QCOMPARE(io.contents(QStringLiteral("config.json")), QByteArray("OLD"));
    QVERIFY(normalised(io.log).contains(QStringLiteral("removeTemp config.json.TMP")));

    // ... and a directory where the file should go (the rename fails).
    FakeHarness h;
    asb::test::FileIoContract::tempRemovedOnFailure(h);
}

void tst_AtomicJson::repeatedSavesLeaveOnlyTheTarget()
{
    FakeHarness h;
    asb::test::FileIoContract::repeatedSavesLeaveOnlyTheTarget(h);
}

void tst_AtomicJson::generatorCalledOncePerWrite()
{
    FakeFileIo io;
    FakeDir dir;
    io.plantFile(kTemp, "PLANTED");
    int calls = 0;
    const NameGen gen = [&]() {
        ++calls;
        return kTemp;
    };
    QVERIFY(!atomicWrite(io, dir, QStringLiteral("config.json"), "NEW", OwnerPolicy::Config, gen).replaced);
    QCOMPARE(calls, 1);   // no retry with the same (planted) name
}

void tst_AtomicJson::contractOnTheFake()
{
    {
        FakeHarness h;
        asb::test::FileIoContract::readBackThroughTheSameHandle(h);
    }
    if (QTest::currentTestFailed()) return;
    FakeHarness h;
    asb::test::FileIoContract::openExistingMissingAndLinks(h);
}

void tst_AtomicJson::defaultNamesAreFreshAndRecognisable()
{
    const NameGen gen = defaultNameGen(QStringLiteral("status.json"));
    QSet<QString> seen;
    static const QRegularExpression shape(QStringLiteral(R"(^status\.json\.[0-9a-f]{32}\.asbtmp$)"));
    for (int i = 0; i < 1000; ++i) {
        const QString n = gen();
        QVERIFY2(shape.match(n).hasMatch(), qPrintable(n));
        QVERIFY(isTempName(n));
        seen.insert(n);
    }
    QCOMPARE(seen.size(), 1000);
    QVERIFY(!isTempName(QStringLiteral("config.json")));
    QVERIFY(!isTempName(QStringLiteral("config.json.tmp")));
    QVERIFY(!isTempName(QStringLiteral(".0123456789abcdef0123456789abcdef.asbtmp")));

    // With no generator, atomicWrite names the temp after the target.
    FakeFileIo io;
    FakeDir dir;
    QVERIFY(atomicWrite(io, dir, QStringLiteral("config.pending.json"), "x", OwnerPolicy::Config).replaced);
    QVERIFY(normalised(io.log).contains(QStringLiteral("createTemp config.pending.json.TMP Config")));
}

// INV-77 seam order. Kills "rename before fsync" (and dropping either barrier).
void tst_AtomicJson::durabilityBarriersInOrder()
{
    FakeFileIo io;
    FakeDir dir;
    const WriteResult r = atomicWrite(io, dir, QStringLiteral("config.json"), "BYTES", OwnerPolicy::Config);
    QVERIFY(r.replaced);
    QVERIFY(r.durable);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(normalised(io.log), (QStringList{
                                     QStringLiteral("createTemp config.json.TMP Config"),
                                     QStringLiteral("chown config.json.TMP"),
                                     QStringLiteral("chmod config.json.TMP 0644"),
                                     QStringLiteral("write config.json.TMP 5"),
                                     QStringLiteral("flush config.json.TMP"),
                                     QStringLiteral("rename config.json.TMP -> config.json"),
                                     QStringLiteral("syncDir"),
                                 }));
    // And what the barriers buy: after a crash the new file is there with its data.
    io.crash();
    QCOMPARE(io.contents(QStringLiteral("config.json")), QByteArray("BYTES"));
}

void tst_AtomicJson::replacementAndDurabilityReportedSeparately()
{
    FakeFileIo io;
    FakeDir dir;
    io.failNext(FakeFileIo::Op::SyncDir, QStringLiteral("EIO"));
    const WriteResult r = atomicWrite(io, dir, QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
    QVERIFY(r.replaced);    // the rename happened: readers see the new bytes
    QVERIFY(!r.durable);    // ... but a power loss may undo it
    QCOMPARE(r.error, QStringLiteral("EIO"));
    QCOMPARE(io.contents(QStringLiteral("config.json")), QByteArray("NEW"));
    QCOMPARE(io.tempNames(), QStringList());
    io.crash();
    QVERIFY(!io.exists(QStringLiteral("config.json")));   // the unsynced rename is gone
}

void tst_AtomicJson::failureBeforeRenameLeavesTheTargetUntouched()
{
    FakeFileIo io;
    FakeDir dir;
    io.plantFile(QStringLiteral("config.json"), "OLD", QStringLiteral("root"));
    io.failNext(FakeFileIo::Op::CreateTemp, QStringLiteral("EACCES"));
    const WriteResult r = atomicWrite(io, dir, QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
    QVERIFY(!r.replaced && !r.durable);
    QCOMPARE(r.error, QStringLiteral("EACCES"));
    QCOMPARE(io.contents(QStringLiteral("config.json")), QByteArray("OLD"));
    QCOMPARE(io.names(), QStringList{QStringLiteral("config.json")});
}

// INV-96. Kills "Runtime policy chowns" (status written with the Config policy, or the policy dropped
// on the way to createTemp): the DynamicUser service could not chown and would fail with EPERM.
void tst_AtomicJson::runtimePolicyNeverChowns()
{
    FakeFileIo io;
    FakeDir dir;
    StatusReport report;
    report.pid = 42;
    const WriteResult r = StatusStore::write(io, dir, report);
    QVERIFY(r.replaced && r.durable);
    const QStringList log = normalised(io.log);
    QVERIFY2(log.contains(QStringLiteral("createTemp status.json.TMP Runtime")), qPrintable(log.join(QLatin1Char('|'))));
    for (const QString& l : log) QVERIFY2(!l.startsWith(QStringLiteral("chown")), qPrintable(l));
    QCOMPARE(io.owner(QStringLiteral("status.json")), QStringLiteral("process"));
    QCOMPARE(io.mode(QStringLiteral("status.json")), 0644);

    FakeFileIo direct;
    QVERIFY(atomicWrite(direct, dir, QStringLiteral("bridge.log"), "x", OwnerPolicy::Runtime).replaced);
    for (const QString& l : direct.log) QVERIFY2(!l.startsWith(QStringLiteral("chown")), qPrintable(l));
}

void tst_AtomicJson::configPolicyChownsBeforeRename()
{
    FakeFileIo io;
    FakeDir dir;
    QVERIFY(ConfigFiles::writePending(io, dir, "{}").replaced);
    const QStringList log = normalised(io.log);
    const qsizetype chown = log.indexOf(QStringLiteral("chown config.pending.json.TMP"));
    const qsizetype rename = log.indexOf(QStringLiteral("rename config.pending.json.TMP -> config.pending.json"));
    QVERIFY2(chown >= 0 && rename > chown, qPrintable(log.join(QLatin1Char('|'))));
    QCOMPARE(io.owner(QStringLiteral("config.pending.json")), QStringLiteral("root"));
}

void tst_AtomicJson::policyPassedThroughUnchanged_data()
{
    QTest::addColumn<int>("policy");
    QTest::newRow("Config") << int(OwnerPolicy::Config);
    QTest::newRow("Runtime") << int(OwnerPolicy::Runtime);
}

void tst_AtomicJson::policyPassedThroughUnchanged()
{
    QFETCH(int, policy);
    FakeFileIo io;
    FakeDir dir;
    QVERIFY(atomicWrite(io, dir, QStringLiteral("f"), "x", OwnerPolicy(policy)).replaced);
    QCOMPARE(normalised(io.log).first(),
             QStringLiteral("createTemp f.TMP %1").arg(policy == int(OwnerPolicy::Config) ? QStringLiteral("Config")
                                                                                          : QStringLiteral("Runtime")));
}

void tst_AtomicJson::crashKeepsOnlyDurableState()
{
    FakeFileIo io;
    FakeDir dir;
    QString err;
    io.plantFile(QStringLiteral("start.token"), "TOKEN", QStringLiteral("root"));
    auto f = io.openExisting(dir, QStringLiteral("start.token"), &err);
    QVERIFY(f);
    QVERIFY(f->truncate(&err));
    QVERIFY(f->writeAll("REVOKED", &err));
    // Not flushed, then removed without a directory sync: a crash brings back the old bytes.
    QVERIFY(f->removeByHandle(&err));
    QVERIFY(!io.exists(QStringLiteral("start.token")));
    io.crash();
    QCOMPARE(io.contents(QStringLiteral("start.token")), QByteArray("TOKEN"));

    // Flushed, then removed without a directory sync: the entry comes back holding the flushed bytes.
    f = io.openExisting(dir, QStringLiteral("start.token"), &err);
    QVERIFY(f);
    QVERIFY(f->truncate(&err) && f->writeAll("REVOKED", &err) && f->flush(&err) && f->removeByHandle(&err));
    io.crash();
    QCOMPARE(io.contents(QStringLiteral("start.token")), QByteArray("REVOKED"));

    // Removed and synced: gone for good.
    f = io.openExisting(dir, QStringLiteral("start.token"), &err);
    QVERIFY(f && f->removeByHandle(&err));
    QVERIFY(io.syncDir(dir, &err));
    io.crash();
    QVERIFY(!io.exists(QStringLiteral("start.token")));
}

QTEST_GUILESS_MAIN(tst_AtomicJson)
#include "tst_AtomicJson.moc"
