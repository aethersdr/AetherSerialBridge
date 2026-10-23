// WS-6: WinFileIo and WinLogFile on the recording NtSeams fake: the six INV-15 FileIoContract checks (the WS-3
// hand-off; the native run is tests/win/integration/tst_WinFileIoNative), create-time security (INV-75, INV-96),
// rename and dispose by handle, the FILE_ADD_FILE directory flush (INV-77).
#include "core/AtomicJson.h"
#include "platform/win/WinFileIo.h"
#include "platform/win/WinLogFile.h"
#include "platform/win/WindowsDataFolder.h"
#include "support/FileIoContract.h"
#include "support/WinFakeFixture.h"

#include <QTest>

using namespace asb;
using namespace asb::win;
using namespace asb::test;
using namespace asb::test::win6;

namespace {

// FileIoContract over WinFileIo in a trusted fake data folder.
class FakeWinHarness final : public FileIoHarness {
public:
    FakeWinHarness()
    {
        setUpMachine(fs);
        folder.createLockedDown();
        insp = folder.inspect(TrustMode::Mutating);
    }
    FileIo& io() override { return m_io; }
    const DirHandle& dir() override { return folder.handle(); }
    void plantFile(const QString& name, const QByteArray& bytes) override { fs.plantFile(kData + QLatin1Char('\\') + name, bytes); }
    void plantSymlink(const QString& name, const QString& target) override
    {
        fs.plantLink(kData + QLatin1Char('\\') + name, kData + QLatin1Char('\\') + target, false);
    }
    void plantDirectory(const QString& name) override { fs.mkdir(kData + QLatin1Char('\\') + name); }
    std::optional<QByteArray> read(const QString& name) override
    {
        const FakeNtSeams::NodeP n = fs.node(kData + QLatin1Char('\\') + name);
        if (!n || n->kind != FakeNtSeams::Node::Kind::File) return std::nullopt;
        return n->data;
    }
    QStringList names() override
    {
        QStringList n = fs.namesIn(kData);
        n.sort();
        return n;
    }

    FakeNtSeams fs;
    WindowsDataFolder folder{fs, kData};
    Inspection insp;

private:
    WinFileIo m_io{fs, CreateSecurity::dataFolder()};
};

}  // namespace

class tst_WinFileIo : public QObject {
    Q_OBJECT

private slots:
    // ---- INV-15: the FileIo contract (WS-3 hand-off) ---------------------------------------------
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
        FakeWinHarness h;
        switch (check) {
        case 0: FileIoContract::plantedGeneratedNameNotReused(h); break;
        case 1: FileIoContract::plantedSymlinkNotFollowed(h); break;
        case 2: FileIoContract::tempRemovedOnFailure(h); break;
        case 3: FileIoContract::repeatedSavesLeaveOnlyTheTarget(h); break;
        case 4: FileIoContract::readBackThroughTheSameHandle(h); break;
        case 5: FileIoContract::openExistingMissingAndLinks(h); break;
        }
        QCOMPARE(h.fs.openHandles(), 1);   // only the folder handle stays open
    }

    void tempCreateIsExclusiveAndRelative()
    {
        FakeWinHarness h;
        QString err;
        auto t = h.io().createTemp(h.dir(), OwnerPolicy::Config, defaultNameGen(QStringLiteral("config.json")), &err);
        QVERIFY2(t, qPrintable(err));
        const FakeNtSeams::Call c = h.fs.callsOf(QStringLiteral("openRelative")).constLast();
        QCOMPARE(c.disposition, nt::kFileCreate);   // never FILE_OPEN_IF / FILE_SUPERSEDE
        QVERIFY(c.flags & nt::kOpenReparsePoint);
        QVERIFY(c.flags & nt::kNonDirectoryFile);
        QCOMPARE(c.handle, WinFileIo::nativeOf(h.dir()));
    }

    // ---- INV-75 / INV-96: the owner is set in the create call ------------------------------------
    void createFileReceivesOwnerSa_data()
    {
        QTest::addColumn<bool>("lockDirectory");
        QTest::addColumn<int>("policy");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("data folder, Config: Administrators") << false << int(OwnerPolicy::Config) << sd::ownerOnlyDescriptor(kBa);
        QTest::newRow("data folder, Runtime: SYSTEM") << false << int(OwnerPolicy::Runtime) << sd::ownerOnlyDescriptor(kSy);
        QTest::newRow("lock directory: protected SYSTEM + Administrators") << true << int(OwnerPolicy::Config) << sd::lockFileDescriptor();
    }
    void createFileReceivesOwnerSa()
    {
        QFETCH(bool, lockDirectory);
        QFETCH(int, policy);
        QFETCH(QByteArray, expected);
        FakeWinHarness h;
        h.fs.defaultOwner = kUser;   // the object-creator owner policy: without an explicit owner, the user owns it
        WinFileIo io(h.fs, lockDirectory ? CreateSecurity::lockDirectory() : CreateSecurity::dataFolder());
        const WriteResult r = atomicWrite(io, h.dir(), QStringLiteral("x.json"), "{}", OwnerPolicy(policy));
        QVERIFY2(r.replaced && r.durable, qPrintable(r.error));
        QList<FakeNtSeams::Call> creates;
        for (const FakeNtSeams::Call& c : h.fs.callsOf(QStringLiteral("openRelative")))
            if (c.disposition == nt::kFileCreate) creates << c;
        QCOMPARE(creates.size(), 1);
        QCOMPARE(creates[0].sd, expected);
        const sd::Descriptor got = parsed(h.fs.node(kData + QStringLiteral("\\x.json"))->sd);
        QCOMPARE(got.owner.value_or(QString()), parsed(expected).owner.value_or(QString()));
        QCOMPARE(h.fs.callCount(QStringLiteral("setSecurity")), 0);   // never "create, then chown"
        if (!lockDirectory) QCOMPARE(h.folder.inspect(TrustMode::Service).action, DirAction::Trust);
    }

    // ---- commit, durability, removal -------------------------------------------------------------
    void commitRenamesTheTempByHandleThenFlushesTheFolder()
    {
        FakeWinHarness h;
        h.fs.plantFile(kData + QStringLiteral("\\config.json"), "OLD", sd::ownerOnlyDescriptor(kBa));
        // A reader that shares delete does not block the replacement (POSIX semantics).
        Handle reader = 0;
        QVERIFY(h.fs.openRelative(WinFileIo::nativeOf(h.dir()), QStringLiteral("config.json"), nt::kGenericRead,
                                  nt::kShareAll, nt::kFileOpen, 0, {}, &reader).ok());
        h.fs.calls.clear();
        const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
        QVERIFY2(r.replaced && r.durable, qPrintable(r.error));
        QCOMPARE(h.fs.node(kData + QStringLiteral("\\config.json"))->data, QByteArray("NEW"));
        QStringList order;
        for (const FakeNtSeams::Call& c : h.fs.calls)
            if (c.op == QLatin1String("flush") || c.op == QLatin1String("rename")) order << c.op;
        QCOMPARE(order, (QStringList{QStringLiteral("flush"), QStringLiteral("rename"), QStringLiteral("flush")}));
        const QList<FakeNtSeams::Call> renames = h.fs.callsOf(QStringLiteral("rename"));
        QCOMPARE(renames[0].disposition, 1u);   // replace
        const FakeNtSeams::Call dirFlushOpen = [&] {
            FakeNtSeams::Call last;
            for (const FakeNtSeams::Call& c : h.fs.callsOf(QStringLiteral("openRelative")))
                if (c.name.isEmpty()) last = c;
            return last;
        }();
        QCOMPARE(dirFlushOpen.access, nt::kAddFile);   // the directory flush needs FILE_ADD_FILE (probed)
        h.fs.close(reader);
    }

    void exclusiveHolderBlocksTheReplace()
    {
        FakeWinHarness h;
        h.fs.plantFile(kData + QStringLiteral("\\engine.lock"), "OLD", sd::lockFileDescriptor());
        Handle holder = 0;
        QVERIFY(h.fs.openRelative(WinFileIo::nativeOf(h.dir()), QStringLiteral("engine.lock"), nt::kGenericRead, 0,
                                  nt::kFileOpen, 0, {}, &holder).ok());
        const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("engine.lock"), "NEW", OwnerPolicy::Config);
        QVERIFY(!r.replaced);
        QVERIFY2(r.error.contains(QLatin1String("sharing")), qPrintable(r.error));
        QCOMPARE(h.names(), QStringList{QStringLiteral("engine.lock")});   // the temp was disposed of
        h.fs.close(holder);
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("engine.lock"), "NEW", OwnerPolicy::Config).replaced);
    }

    void durabilityFailureReportedSeparately()
    {
        FakeWinHarness h;
        h.fs.inject = [](const FakeNtSeams::Call& c) -> std::optional<Status> {
            if (c.op == QLatin1String("flush") && c.path == kData) return Status::fail(Status::Kind::Other, 1, QStringLiteral("io"));
            return std::nullopt;
        };
        const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
        QVERIFY(r.replaced);
        QVERIFY(!r.durable);
    }

    void tempDisposedWhenTheWriteFails()
    {
        FakeWinHarness h;
        h.fs.inject = [](const FakeNtSeams::Call& c) -> std::optional<Status> {
            if (c.op == QLatin1String("write")) return Status::fail(Status::Kind::Other, 112, QStringLiteral("disk full"));
            return std::nullopt;
        };
        const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
        QVERIFY(!r.replaced);
        QVERIFY(h.names().isEmpty());
        QCOMPARE(h.fs.callCount(QStringLiteral("dispose")), 1);
    }

    void removeAndOpenExistingRefuseLinksAndFolders()
    {
        FakeWinHarness h;
        h.fs.plantFile(kData + QStringLiteral("\\target"), "T");
        h.fs.plantLink(kData + QStringLiteral("\\link"), kData + QStringLiteral("\\target"), false);
        h.fs.mkdir(kData + QStringLiteral("\\sub"));
        QString err;
        QVERIFY(!h.io().remove(h.dir(), QStringLiteral("link"), &err));
        QVERIFY(!h.io().remove(h.dir(), QStringLiteral("sub"), &err));
        QVERIFY(!h.io().openExisting(h.dir(), QStringLiteral("link"), &err));
        QVERIFY(!h.io().openExisting(h.dir(), QStringLiteral("sub"), &err));
        QVERIFY(!h.io().remove(h.dir(), QStringLiteral("absent"), &err));
        QCOMPARE(err, fileNotFoundError());
        QVERIFY(h.fs.node(kData + QStringLiteral("\\target")) && h.fs.node(kData + QStringLiteral("\\link")));
        QVERIFY(h.io().remove(h.dir(), QStringLiteral("target"), &err));
        QVERIFY(!h.fs.node(kData + QStringLiteral("\\target")));
        for (const char* bad : {"..", ".", "a\\b", "a/b", "x:stream", ""}) QVERIFY(!h.io().remove(h.dir(), QString::fromLatin1(bad), &err));
    }

    void foreignDirHandleRefused()
    {
        FakeNtSeams fs;
        WinFileIo io(fs, CreateSecurity::dataFolder());
        class Other final : public DirHandle {
        public:
            FileId id() const override { return {}; }
        } other;
        QString err;
        QVERIFY(!io.createTemp(other, OwnerPolicy::Config, defaultNameGen(), &err));
        QVERIFY(!io.syncDir(other, &err));
    }

    // ---- the log file for WS-7 -------------------------------------------------------------------
    void logFileCreatedWithTheRuntimeOwner()
    {
        FakeWinHarness h;
        h.fs.defaultOwner = kUser;
        WinLogFile log(h.fs, h.dir());
        QCOMPARE(log.size(), qint64(-1));
        QString err;
        QVERIFY2(log.append("one\n", &err), qPrintable(err));
        QVERIFY2(log.append("two\n", &err), qPrintable(err));
        const FakeNtSeams::NodeP n = h.fs.node(kData + QStringLiteral("\\bridge.log"));
        QCOMPARE(n->data, QByteArray("one\ntwo\n"));
        QCOMPARE(parsed(n->sd).owner.value_or(QString()), kSy);
        QCOMPARE(log.size(), qint64(8));
        QVERIFY2(log.rollover(&err), qPrintable(err));
        QVERIFY(h.fs.node(kData + QStringLiteral("\\bridge.log.1")) == n);
        QCOMPARE(log.size(), qint64(-1));
        QCOMPARE(h.folder.inspect(TrustMode::Service).action, DirAction::Trust);
        // A link planted at the log's name is never written through.
        h.fs.plantFile(kData + QStringLiteral("\\victim"), "V");
        h.fs.plantLink(kData + QStringLiteral("\\bridge.log"), kData + QStringLiteral("\\victim"), false);
        QVERIFY(!log.append("x", &err));
        QCOMPARE(h.fs.node(kData + QStringLiteral("\\victim"))->data, QByteArray("V"));
    }
};

QTEST_GUILESS_MAIN(tst_WinFileIo)
#include "tst_WinFileIo.moc"
