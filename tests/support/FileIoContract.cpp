#include "support/FileIoContract.h"

#include "core/AtomicJson.h"

#include <QTest>

namespace asb::test::FileIoContract {
namespace {

NameGen fixed(const QString& name)
{
    return [name]() { return name; };
}

const QString kPlantedTemp = QStringLiteral("config.json.0123456789abcdef0123456789abcdef.asbtmp");

}  // namespace

void plantedGeneratedNameNotReused(FileIoHarness& h)
{
    h.plantFile(kPlantedTemp, "PLANTED");
    const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "NEW", OwnerPolicy::Config,
                                      fixed(kPlantedTemp));
    QVERIFY2(!r.replaced, "a write through a planted temp name must fail");
    QVERIFY(!r.error.isEmpty());
    QCOMPARE(h.read(kPlantedTemp).value_or("<missing>"), QByteArray("PLANTED"));
    QVERIFY(!h.read(QStringLiteral("config.json")).has_value());
}

void plantedSymlinkNotFollowed(FileIoHarness& h)
{
    h.plantFile(QStringLiteral("canary.txt"), "CANARY");
    h.plantSymlink(kPlantedTemp, QStringLiteral("canary.txt"));
    const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "NEW", OwnerPolicy::Config,
                                      fixed(kPlantedTemp));
    QVERIFY2(!r.replaced, "a write through a planted link must fail");
    QCOMPARE(h.read(QStringLiteral("canary.txt")).value_or("<missing>"), QByteArray("CANARY"));
    QVERIFY(h.names().contains(kPlantedTemp));   // the link itself is not removed either
    QVERIFY(!h.read(QStringLiteral("config.json")).has_value());
}

void tempRemovedOnFailure(FileIoHarness& h)
{
    h.plantDirectory(QStringLiteral("config.json"));   // a directory where the file should go: the rename fails
    const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
    QVERIFY(!r.replaced);
    QVERIFY(!r.error.isEmpty());
    QCOMPARE(h.names(), QStringList{QStringLiteral("config.json")});
}

void repeatedSavesLeaveOnlyTheTarget(FileIoHarness& h)
{
    QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "ONE", OwnerPolicy::Config).replaced);
    const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "TWO", OwnerPolicy::Config);
    QVERIFY(r.replaced);
    QVERIFY(r.durable);
    QCOMPARE(h.names(), QStringList{QStringLiteral("config.json")});
    QCOMPARE(h.read(QStringLiteral("config.json")).value_or("<missing>"), QByteArray("TWO"));
}

void readBackThroughTheSameHandle(FileIoHarness& h)
{
    QString err;
    auto t = h.io().createTemp(h.dir(), OwnerPolicy::Config, defaultNameGen(QStringLiteral("x")), &err);
    QVERIFY2(t, qPrintable(err));
    QVERIFY(t->write("0123456789", &err));
    QVERIFY(t->flush(&err));
    QCOMPARE(t->readBack(10, &err), QByteArray("0123456789"));
    err.clear();
    QVERIFY(t->readBack(9, &err).isEmpty());
    QVERIFY2(!err.isEmpty(), "more than cap must be an error");
    t.reset();   // not committed: the temp goes away
    QVERIFY(h.names().isEmpty());
}

void openExistingMissingAndLinks(FileIoHarness& h)
{
    QString err;
    QVERIFY(!h.io().openExisting(h.dir(), QStringLiteral("start.token"), &err));
    QCOMPARE(err, fileNotFoundError());
    h.plantFile(QStringLiteral("target"), "T");
    h.plantSymlink(QStringLiteral("start.token"), QStringLiteral("target"));
    err.clear();
    QVERIFY(!h.io().openExisting(h.dir(), QStringLiteral("start.token"), &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err != fileNotFoundError());
    QCOMPARE(h.read(QStringLiteral("target")).value_or("<missing>"), QByteArray("T"));
}

}  // namespace asb::test::FileIoContract
