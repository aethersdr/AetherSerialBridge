// The Linux engine lock and apply lock on a real file system (WS-8; INV-66 twins, INV-97 twin, INV-103,
// INV-110, INV-116). The owner the locks require is this test's own uid/gid (the build container is
// unprivileged); root ownership is proven in the systemd suite.

#include "core/LockIdentity.h"
#include "platform/posix/LinuxLocks.h"
#include "support/RecordingSys.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <vector>

using namespace asb;
using namespace asb::posix;
using asb::test::RecordingSys;

namespace {

constexpr long kExt4 = 0xEF53;
constexpr long kOverlay = 0x794C7630;
constexpr long kTmpfs = 0x01021994;
constexpr long kNfs = 0x6969;

QByteArray enc(const QString& p)
{
    return QFile::encodeName(p);
}

void writeFile(const QString& path, const QByteArray& bytes, mode_t mode)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
    f.close();
    QVERIFY(::chmod(enc(path).constData(), mode) == 0);
}

mode_t modeOf(const QString& path)
{
    struct stat st {};
    if (::lstat(enc(path).constData(), &st) != 0) return mode_t(-1);
    return st.st_mode & 07777;
}

struct Fixture {
    QTemporaryDir tmp;
    RecordingSys sys;
    QString dir() const { return tmp.path() + QStringLiteral("/aether-serial-bridge"); }
    QString file(const char* n) const { return dir() + QLatin1Char('/') + QLatin1String(n); }
    EngineLock::Options opts(uid_t owner = ::getuid()) const
    {
        EngineLock::Options o;
        o.directory = dir();
        o.owner = PosixOwner{owner, ::getgid()};
        return o;
    }
    Fixture()
    {
        ::umask(022);
        sys.fsType = kExt4;   // the build host's file system may be anything; INV-116 rows inject it
        QDir().mkdir(dir());
        ::chmod(enc(dir()).constData(), 0755);
    }
    // A matching pair published through the adapter's own FileIo (LockIdentity::publish, as install does).
    QByteArray publishPair(EngineLock& lock)
    {
        const QByteArray id = LockIdentity::newId();
        const WriteResult a = LockIdentity::publish(lock.io(), lock.dir(), QString::fromLatin1(LockIdentity::kLockName), id);
        const WriteResult b = LockIdentity::publish(lock.io(), lock.dir(), QString::fromLatin1(LockIdentity::kRecordName), id);
        if (!a.replaced || !a.durable || !b.replaced || !b.durable) return {};
        return id;
    }
};

// A clock that moves only when slept on, and records every sleep (acquireUntil's polling).
class SteppingClock final : public IClock {
public:
    Deadline now() const override { return t; }
    void sleepFor(std::chrono::milliseconds d) override
    {
        sleeps.push_back(d);
        t += d;
    }
    Deadline t = std::chrono::steady_clock::now();
    std::vector<std::chrono::milliseconds> sleeps;
};

}  // namespace

class tst_EngineLockPosix : public QObject {
    Q_OBJECT

private slots:
    void acquireReadAndRelease()
    {
        Fixture f;
        EngineLock lock(f.opts(), systemClock(), f.sys);
        const QByteArray id = f.publishPair(lock);
        QVERIFY(!id.isEmpty());
        LockResult why = LockResult::Busy;
        auto h = lock.tryAcquire(&why);
        QVERIFY2(h, qPrintable(lock.lastError()));
        QCOMPARE(why, LockResult::Acquired);
        QCOMPARE(LockIdentity::parse(lock.readHeldLock().bytes).value_or(QByteArray()), id);
        QCOMPARE(LockIdentity::classify(lock.readHeldLock(), lock.readRecord()), LockIdentity::PairState::Matching);
        h.reset();
        QCOMPARE(lock.readHeldLock().status, LockFileRead::Status::Failed);   // only through a live hold
        auto again = lock.tryAcquire(&why);
        QVERIFY(again);
    }

    // INV-110: flock(2), never fcntl(F_SETLK). A record lock would be dropped by closing ANY fd of the file,
    // and does not exclude the same process; flock belongs to the open file description, which no other fd
    // can release.
    void closingAnotherFdKeepsLock()
    {
        Fixture f;
        EngineLock lock(f.opts(), systemClock(), f.sys);
        QVERIFY(!f.publishPair(lock).isEmpty());
        LockResult why = LockResult::Busy;
        auto held = lock.tryAcquire(&why);
        QVERIFY(held);
        // Another fd on the same file, opened and closed in this process.
        const int other = ::open(enc(f.file("engine.lock")).constData(), O_RDONLY | O_CLOEXEC);
        QVERIFY(other >= 0);
        ::close(other);
        EngineLock second(f.opts(), systemClock(), f.sys);
        auto h2 = second.tryAcquire(&why);
        QVERIFY2(!h2, "a second acquisition succeeded while the first hold is alive");
        QCOMPARE(why, LockResult::Busy);
        held.reset();
        auto h3 = second.tryAcquire(&why);
        QVERIFY(h3);
        // The lock is a flock on the O_CLOEXEC fd (never F_SETLK).
        bool flocked = false;
        for (const QString& e : f.sys.log()) flocked = flocked || e.startsWith(QLatin1String("flock engine.lock"));
        QVERIFY(flocked);
    }

    // INV-110: the directory and the file are asserted by fd, exactly: root:root 0755 / 0644 in production.
    void modeAssertedDirectly()
    {
        Fixture f;
        EngineLock lock(f.opts(), systemClock(), f.sys);
        QVERIFY(!f.publishPair(lock).isEmpty());
        // What the adapter publishes is exactly 0644.
        QCOMPARE(modeOf(f.file("engine.lock")), mode_t(0644));
        QCOMPARE(modeOf(f.file("engine.lock.id")), mode_t(0644));
        LockResult why = LockResult::Busy;
        for (mode_t bad : {mode_t(0666), mode_t(0664), mode_t(0640), mode_t(0600), mode_t(04644)}) {
            QVERIFY(::chmod(enc(f.file("engine.lock")).constData(), bad) == 0);
            QVERIFY2(!lock.tryAcquire(&why), qPrintable(QString::number(bad, 8)));
            QCOMPARE(why, LockResult::InsecureFile);
            QCOMPARE(lock.peekLock().status, LockFileRead::Status::Failed);
        }
        QVERIFY(::chmod(enc(f.file("engine.lock")).constData(), 0644) == 0);
        QVERIFY(lock.tryAcquire(&why));
        for (mode_t bad : {mode_t(0775), mode_t(0700), mode_t(0757), mode_t(02755)}) {
            QVERIFY(::chmod(enc(f.dir()).constData(), bad) == 0);
            QVERIFY2(!lock.tryAcquire(&why), qPrintable(QString::number(bad, 8)));
            QCOMPARE(why, LockResult::Insecure);
        }
    }
    void ownerAssertedDirectly()
    {
        Fixture f;
        EngineLock mine(f.opts(), systemClock(), f.sys);
        QVERIFY(!f.publishPair(mine).isEmpty());
        // The directory passes for a lock that requires our uid; with another uid required, both fail.
        EngineLock other(f.opts(::getuid() + 1), systemClock(), f.sys);
        LockResult why = LockResult::Busy;
        QVERIFY(!other.tryAcquire(&why));
        QCOMPARE(why, LockResult::Insecure);   // the directory owner fails first: never replaced
        // A symbolic link at engine.lock in a good directory: InsecureFile (install/Repair replace it).
        const QString planted = f.dir() + QStringLiteral("/engine.lock");
        QVERIFY(QFile::remove(planted));
        QVERIFY(::symlink("/etc/passwd", enc(planted).constData()) == 0);
        QVERIFY(!mine.tryAcquire(&why));
        QCOMPARE(why, LockResult::InsecureFile);
    }
    void missingAndUnsupported()
    {
        Fixture f;
        EngineLock lock(f.opts(), systemClock(), f.sys);
        LockResult why = LockResult::Busy;
        QVERIFY(!lock.tryAcquire(&why));
        QCOMPARE(why, LockResult::Missing);
        QCOMPARE(lock.peekLock().status, LockFileRead::Status::Missing);
        QCOMPARE(lock.readRecord().status, LockFileRead::Status::Missing);
        QVERIFY(!f.publishPair(lock).isEmpty());
        for (long fs : {kOverlay, kTmpfs, kNfs}) {
            f.sys.fsType = fs;
            QVERIFY(!lock.tryAcquire(&why));
            QCOMPARE(why, LockResult::Unsupported);
            QString reason;
            QVERIFY(!lock.filesystemSupported(&reason));
            QVERIFY(!reason.isEmpty());
        }
        f.sys.fsType = kExt4;
        QString reason;
        QVERIFY(lock.filesystemSupported(&reason));
        // Missing directory: judged where install would create it (the parent).
        QVERIFY(QDir(f.dir()).removeRecursively());
        f.sys.fsType = kOverlay;
        QVERIFY(!lock.filesystemSupported(&reason));
        QVERIFY2(reason.contains(f.tmp.path()), qPrintable(reason));
    }
    void recordChecks()
    {
        Fixture f;
        EngineLock lock(f.opts(), systemClock(), f.sys);
        QVERIFY(!f.publishPair(lock).isEmpty());
        QCOMPARE(lock.readRecord().status, LockFileRead::Status::Ok);
        // Exactly 0644, like engine.lock: a record root can read but the DynamicUser service cannot (0600, 0640)
        // is no matching record, so install and Repair replace the pair.
        for (mode_t bad : {mode_t(0664), mode_t(0600), mode_t(0640), mode_t(0444)}) {
            QVERIFY(::chmod(enc(f.file("engine.lock.id")).constData(), bad) == 0);
            QCOMPARE(lock.readRecord().status, LockFileRead::Status::Failed);
        }
        QVERIFY(::chmod(enc(f.file("engine.lock.id")).constData(), 0644) == 0);
        QCOMPARE(lock.readRecord().status, LockFileRead::Status::Ok);
        writeFile(f.file("engine.lock.id"), QByteArray(100, 'x'), 0644);
        QCOMPARE(lock.readRecord().status, LockFileRead::Status::Failed);   // more than 64 bytes
    }

    // R17: a squatter (any process holding a flock on the world-readable file) is named from /proc/locks.
    void squatterNamedFromProcLocks()
    {
        Fixture f;
        EngineLock lock(f.opts(), systemClock(), f.sys);
        QVERIFY(!f.publishPair(lock).isEmpty());
        const int squat = ::open(enc(f.file("engine.lock")).constData(), O_RDONLY | O_CLOEXEC);
        QVERIFY(squat >= 0);
        QVERIFY(::flock(squat, LOCK_EX | LOCK_NB) == 0);
        LockResult why = LockResult::Acquired;
        QVERIFY(!lock.tryAcquire(&why));
        QCOMPARE(why, LockResult::Busy);
        QFile comm(QStringLiteral("/proc/self/comm"));
        QVERIFY(comm.open(QIODevice::ReadOnly));
        const QString self = QString::fromLocal8Bit(comm.readAll()).trimmed();
        QCOMPARE(lock.holderDescription(), QStringLiteral("pid %1 (%2)").arg(::getpid()).arg(self));
        QVERIFY2(lock.lastError().contains(lock.holderDescription()), qPrintable(lock.lastError()));
        ::close(squat);
    }
    void procLocksParsing()
    {
        const QByteArray text = "1: POSIX  ADVISORY  WRITE 100 08:01:42 0 EOF\n"
                                "2: FLOCK  ADVISORY  WRITE 200 fd:01:4242 0 EOF\n"
                                "2: -> FLOCK  ADVISORY  WRITE 300 fd:01:4242 0 EOF\n"
                                "3: FLOCK  ADVISORY  WRITE 400 08:01:42 0 EOF\n";
        QCOMPARE(procLocksHolder(text, makedev(0xfd, 1), 4242).value_or(-1), 200);
        QCOMPARE(procLocksHolder(text, makedev(8, 1), 42).value_or(-1), 400);   // the POSIX record lock is not it
        QVERIFY(!procLocksHolder(text, makedev(8, 1), 43));
    }

    // WS-5 round 3 (binding 2): acquireUntil honours the deadline it is given. The last poll sleeps only
    // what is left, so a Busy lock returns AT the deadline, not up to one poll later (a phase that ends on
    // it would otherwise be judged late); a deadline already passed costs one attempt and no sleep.
    void acquireUntilNeverSleepsPastTheDeadline()
    {
        Fixture f;
        SteppingClock clock;
        EngineLock lock(f.opts(), clock, f.sys);
        QVERIFY(!f.publishPair(lock).isEmpty());
        const int squat = ::open(enc(f.file("engine.lock")).constData(), O_RDONLY | O_CLOEXEC);
        QVERIFY(::flock(squat, LOCK_EX | LOCK_NB) == 0);
        const Deadline deadline = clock.t + std::chrono::milliseconds(250);
        LockResult why = LockResult::Acquired;
        QVERIFY(!lock.acquireUntil(deadline, &why));
        QCOMPARE(why, LockResult::Busy);
        QVERIFY(clock.t == deadline);
        const std::vector<std::chrono::milliseconds> expected{std::chrono::milliseconds(100), std::chrono::milliseconds(100),
                                                              std::chrono::milliseconds(50)};
        QVERIFY(clock.sleeps == expected);
        clock.sleeps.clear();
        why = LockResult::Acquired;
        QVERIFY(!lock.acquireUntil(clock.t - std::chrono::milliseconds(1), &why));
        QCOMPARE(why, LockResult::Busy);
        QVERIFY(clock.sleeps.empty());
        ::close(squat);
    }

    void acquireUntilPollsUntilTheDeadline()
    {
        Fixture f;
        EngineLock lock(f.opts(), systemClock(), f.sys);
        QVERIFY(!f.publishPair(lock).isEmpty());
        const int squat = ::open(enc(f.file("engine.lock")).constData(), O_RDONLY | O_CLOEXEC);
        QVERIFY(::flock(squat, LOCK_EX | LOCK_NB) == 0);
        const auto t0 = std::chrono::steady_clock::now();
        LockResult why = LockResult::Acquired;
        QVERIFY(!lock.acquireUntil(t0 + std::chrono::milliseconds(400), &why));
        QCOMPARE(why, LockResult::Busy);
        QVERIFY(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(400));
        ::close(squat);
        QVERIFY(lock.acquireUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2), &why));
    }

    // --- apply.lock (INV-66 / INV-97 twins) ----------------------------------------------------------
    void applyLockExcludesAndReleases()
    {
        Fixture f;
        FileApplyLock::Options o;
        o.directory = f.tmp.path();
        o.owner = PosixOwner::current();
        o.requireTmpfs = false;
        FileApplyLock a(o, f.sys), b(o, f.sys);
        auto h = a.acquire(std::chrono::seconds(1));
        QVERIFY2(h, qPrintable(a.refusalReason()));
        QCOMPARE(modeOf(f.tmp.path() + QStringLiteral("/aether-serial-bridge.apply.lock")), mode_t(0600));
        const auto t0 = std::chrono::steady_clock::now();
        QVERIFY(!b.acquire(std::chrono::seconds(1)));
        QVERIFY(b.refusalReason().isEmpty());   // a timeout, not a refusal
        QVERIFY(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(900));
        h.reset();
        QVERIFY(b.acquire(std::chrono::seconds(1)));
    }
    void applyLockModeResetAndReported()
    {
        Fixture f;
        QStringList reports;
        FileApplyLock::Options o;
        o.directory = f.tmp.path();
        o.owner = PosixOwner::current();
        o.requireTmpfs = false;
        o.report = [&](const QString& s) { reports << s; };
        writeFile(f.tmp.path() + QStringLiteral("/aether-serial-bridge.apply.lock"), "", 0666);
        FileApplyLock a(o, f.sys);
        QVERIFY(a.acquire(std::chrono::seconds(1)));
        QCOMPARE(modeOf(f.tmp.path() + QStringLiteral("/aether-serial-bridge.apply.lock")), mode_t(0600));
        QCOMPARE(reports.size(), 1);
    }
    void applyLockRefusals()
    {
        Fixture f;
        FileApplyLock::Options o;
        o.directory = f.tmp.path();
        o.owner = PosixOwner::current();
        o.requireTmpfs = true;
        f.sys.fsType = kExt4;
        FileApplyLock notTmpfs(o, f.sys);
        QVERIFY(!notTmpfs.acquire(std::chrono::seconds(1)));
        QVERIFY2(notTmpfs.refusalReason().contains(QLatin1String("not tmpfs")), qPrintable(notTmpfs.refusalReason()));
        o.requireTmpfs = false;
        QVERIFY(::symlink("/etc/passwd", enc(f.tmp.path() + QStringLiteral("/aether-serial-bridge.apply.lock")).constData()) == 0);
        FileApplyLock link(o, f.sys);
        QVERIFY(!link.acquire(std::chrono::seconds(1)));
        QVERIFY(!link.refusalReason().isEmpty());
        QVERIFY(QFile::remove(f.tmp.path() + QStringLiteral("/aether-serial-bridge.apply.lock")));
        o.owner = PosixOwner{::getuid() + 1, ::getgid()};
        FileApplyLock foreignDir(o, f.sys);
        QVERIFY(!foreignDir.acquire(std::chrono::seconds(1)));
        QVERIFY(!foreignDir.refusalReason().isEmpty());
    }
};

QTEST_GUILESS_MAIN(tst_EngineLockPosix)
#include "tst_EngineLockPosix.moc"
