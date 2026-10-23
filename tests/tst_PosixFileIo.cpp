// PosixFileIo on a real file system (WS-8): the six FileIoContract checks (WS-3 hand-off, INV-15), the
// ownership step before the rename (INV-75 POSIX row, INV-96 POSIX rows) and the durability barrier order
// (INV-77 native row), observed on the real calls through RecordingSys.
//
// The build container is unprivileged, so the Config owner is this test's own uid/gid (PosixOwner::current):
// the code path (fchown on the private fd, then fchmod, before the rename) is the production one; root
// ownership itself is proven in the systemd suite.

#include "core/AtomicJson.h"
#include "platform/posix/PosixFileIo.h"
#include "support/FileIoContract.h"
#include "support/RecordingSys.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace asb;
using namespace asb::posix;
using asb::test::RecordingSys;

namespace {

class PosixHarness final : public asb::test::FileIoHarness {
public:
    PosixHarness() : m_io(m_sys, PosixOwner::current(), 0644)
    {
        const QByteArray p = QFile::encodeName(m_tmp.path());
        const int fd = ::open(p.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        m_dir.reset(m_sys, fd);
        m_sys.label(fd, QStringLiteral("DIR"));
    }
    FileIo& io() override { return m_io; }
    const DirHandle& dir() override { return m_dir; }
    void plantFile(const QString& name, const QByteArray& bytes) override
    {
        QFile f(path(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
    void plantSymlink(const QString& name, const QString& target) override
    {
        QVERIFY(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(path(name)).constData()) == 0);
    }
    void plantDirectory(const QString& name) override { QVERIFY(QDir().mkdir(path(name))); }
    std::optional<QByteArray> read(const QString& name) override
    {
        struct stat st {};
        if (::lstat(QFile::encodeName(path(name)).constData(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
        QFile f(path(name));
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        return f.readAll();
    }
    QStringList names() override
    {
        return QDir(m_tmp.path()).entryList(QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    }
    QString path(const QString& name) const { return m_tmp.path() + QLatin1Char('/') + name; }
    RecordingSys& sys() { return m_sys; }
    PosixDirHandle& handle() { return m_dir; }

private:
    QTemporaryDir m_tmp;
    RecordingSys m_sys;
    PosixFileIo m_io;
    PosixDirHandle m_dir;
};

int indexOf(const QStringList& log, const QString& prefix, int from = 0)
{
    for (int i = from; i < log.size(); ++i)
        if (log[i].startsWith(prefix)) return i;
    return -1;
}

mode_t modeOf(const QString& path)
{
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) return mode_t(-1);
    return st.st_mode & 07777;
}

}  // namespace

class tst_PosixFileIo : public QObject {
    Q_OBJECT

private slots:
    // --- INV-15: the FileIo contract on the native POSIX FileIo (WS-3 hand-off) ------------------
    void fileIoContract_data()
    {
        QTest::addColumn<int>("check");
        QTest::newRow("plantedGeneratedNameNotReused") << 0;
        QTest::newRow("plantedSymlinkNotFollowed") << 1;
        QTest::newRow("tempRemovedOnFailure") << 2;
        QTest::newRow("repeatedSavesLeaveOnlyTheTarget") << 3;
        QTest::newRow("readBackThroughTheSameHandle") << 4;
        QTest::newRow("openExistingMissingAndLinks") << 5;
    }
    void fileIoContract()
    {
        QFETCH(int, check);
        PosixHarness h;
        switch (check) {
        case 0: asb::test::FileIoContract::plantedGeneratedNameNotReused(h); break;
        case 1: asb::test::FileIoContract::plantedSymlinkNotFollowed(h); break;
        case 2: asb::test::FileIoContract::tempRemovedOnFailure(h); break;
        case 3: asb::test::FileIoContract::repeatedSavesLeaveOnlyTheTarget(h); break;
        case 4: asb::test::FileIoContract::readBackThroughTheSameHandle(h); break;
        case 5: asb::test::FileIoContract::openExistingMissingAndLinks(h); break;
        }
    }
    // The temp is created exclusively, without following links, never inheritable (O_EXCL is what the
    // drop-O_EXCL mutant removes; the contract's plantedGeneratedNameNotReused is its killing test).
    void tempCreateIsExclusiveAndPrivate()
    {
        PosixHarness h;
        QString err;
        auto t = h.io().createTemp(h.dir(), OwnerPolicy::Config, defaultNameGen(QStringLiteral("config.json")), &err);
        QVERIFY2(t, qPrintable(err));
        const QStringList log = h.sys().log();
        const int c = indexOf(log, QStringLiteral("openat config.json."));
        QVERIFY2(c >= 0 && log[c].endsWith(QLatin1String(" create")), qPrintable(log.join(QLatin1Char('\n'))));
    }

    // --- INV-75 / INV-96 (POSIX rows): ownership on the private fd, before the rename ------------
    void ownerSetBeforeRename()
    {
        PosixHarness h;
        const WriteResult w = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "{}", OwnerPolicy::Config);
        QVERIFY(w.replaced && w.durable);
        const QStringList log = h.sys().log();
        const int create = indexOf(log, QStringLiteral("openat config.json."));
        const int chown = indexOf(log, QStringLiteral("fchown config.json."));
        const int chmod = indexOf(log, QStringLiteral("fchmod config.json."));
        const int write = indexOf(log, QStringLiteral("write config.json."));
        const int rename = indexOf(log, QStringLiteral("renameat config.json."));
        QVERIFY2(create >= 0 && chown > create && chmod > chown && write > chmod && rename > write,
                 qPrintable(log.join(QLatin1Char('\n'))));
        QCOMPARE(modeOf(h.path(QStringLiteral("config.json"))), mode_t(0644));
    }
    void configPolicyChownsBeforeRename()
    {
        PosixHarness h;
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("start.token"), "x", OwnerPolicy::Config).replaced);
        const QStringList log = h.sys().log();
        QVERIFY(indexOf(log, QStringLiteral("fchown start.token.")) >= 0);
        QVERIFY(indexOf(log, QStringLiteral("fchown start.token.")) < indexOf(log, QStringLiteral("renameat start.token.")));
    }
    void runtimePolicyNeverChowns()
    {
        PosixHarness h;
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("status.json"), "{}", OwnerPolicy::Runtime).replaced);
        const QStringList log = h.sys().log();
        QCOMPARE(indexOf(log, QStringLiteral("fchown")), -1);
        QVERIFY(indexOf(log, QStringLiteral("fchmod status.json.")) >= 0);
        QCOMPARE(modeOf(h.path(QStringLiteral("status.json"))), mode_t(0644));
    }
    void chownFailureLeavesNothing()
    {
        PosixHarness h;
        h.sys().failWith(QStringLiteral("fchown"), EPERM);
        const WriteResult w = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "{}", OwnerPolicy::Config);
        QVERIFY(!w.replaced);
        QVERIFY2(w.error.contains(QLatin1String("fchown")), qPrintable(w.error));
        QCOMPARE(h.names(), QStringList{});
    }

    // --- INV-77 (native row): fsync(temp) before renameat, fsync(dir) after ------------------------
    void durabilityBarriersInOrderNative()
    {
        PosixHarness h;
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "{}", OwnerPolicy::Config).durable);
        const QStringList log = h.sys().log();
        const int fsyncTemp = indexOf(log, QStringLiteral("fsync config.json."));
        const int rename = indexOf(log, QStringLiteral("renameat config.json."));
        const int fsyncDir = indexOf(log, QStringLiteral("fsync DIR"));
        QVERIFY2(fsyncTemp >= 0 && rename > fsyncTemp && fsyncDir > rename, qPrintable(log.join(QLatin1Char('\n'))));
    }
    void durabilityFailureReportedSeparately()
    {
        PosixHarness h;
        h.sys().failWith(QStringLiteral("fsync DIR"), EIO);
        const WriteResult w = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "{}", OwnerPolicy::Config);
        QVERIFY(w.replaced);
        QVERIFY(!w.durable);
        QVERIFY(h.read(QStringLiteral("config.json")).has_value());
    }

    // --- remove / openExisting: regular files only, never followed ---------------------------------
    void removeRefusesLinksAndDirectories()
    {
        PosixHarness h;
        h.plantFile(QStringLiteral("target"), "T");
        h.plantSymlink(QStringLiteral("link"), QStringLiteral("target"));
        h.plantDirectory(QStringLiteral("dir"));
        QString err;
        QVERIFY(!h.io().remove(h.dir(), QStringLiteral("link"), &err));
        QVERIFY(!h.io().remove(h.dir(), QStringLiteral("dir"), &err));
        QVERIFY(!h.io().remove(h.dir(), QStringLiteral("missing"), &err));
        QCOMPARE(err, fileNotFoundError());
        QVERIFY(h.io().remove(h.dir(), QStringLiteral("target"), &err));
        QCOMPARE(h.names(), (QStringList{QStringLiteral("dir"), QStringLiteral("link")}));
    }
    void openExistingRefusesFifo()
    {
        PosixHarness h;
        QVERIFY(::mkfifo(QFile::encodeName(h.path(QStringLiteral("start.token"))).constData(), 0600) == 0);
        QString err;
        QVERIFY(!h.io().openExisting(h.dir(), QStringLiteral("start.token"), &err));   // never blocks on a FIFO
        QVERIFY(!err.isEmpty());
    }
    void existingFileRemovedOnlyIfStillTheSame()
    {
        PosixHarness h;
        h.plantFile(QStringLiteral("start.token"), "OLD");
        QString err;
        auto f = h.io().openExisting(h.dir(), QStringLiteral("start.token"), &err);
        QVERIFY2(f, qPrintable(err));
        // Replaced after the open: removeByHandle must not delete the newcomer.
        h.plantFile(QStringLiteral("new"), "NEW");
        QVERIFY(::rename(QFile::encodeName(h.path(QStringLiteral("new"))).constData(),
                         QFile::encodeName(h.path(QStringLiteral("start.token"))).constData()) == 0);
        QVERIFY(!f->removeByHandle(&err));
        QCOMPARE(h.read(QStringLiteral("start.token")).value_or("<missing>"), QByteArray("NEW"));
    }
    void plainNamesOnly()
    {
        PosixHarness h;
        QString err;
        QVERIFY(!h.io().remove(h.dir(), QStringLiteral("../x"), &err));
        QVERIFY(!h.io().createTemp(h.dir(), OwnerPolicy::Config, [] { return QStringLiteral("a/b"); }, &err));
    }
};

QTEST_GUILESS_MAIN(tst_PosixFileIo)
#include "tst_PosixFileIo.moc"
