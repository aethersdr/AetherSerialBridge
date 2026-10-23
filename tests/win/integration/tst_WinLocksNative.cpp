// WS-6 native rows on windows-latest (admin), label win-integration: apply.lock and engine.lock between real
// processes and real accounts (INV-66, INV-97, INV-104), plus the engine-lock replace ordering of the WS-5 fold.
#include "WinNativeSupport.h"

#include "core/BridgeRunner.h"
#include "core/LockIdentity.h"
#include "core/StartToken.h"
#include "platform/win/EngineLock.h"
#include "platform/win/FileApplyLock.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/SecurityDescriptor.h"
#include "platform/win/WindowsDataFolder.h"
#include "support/Fakes.h"

#include <QFileInfo>
#include <QProcess>
#include <QTest>

#include <algorithm>

using namespace asb;
using namespace asb::win;
using namespace asb::test;
using namespace asb::test::native;

namespace {
const QString kLockSddl = QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)");
const QString kWide = QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;BU)");

QString helper()
{
    return qEnvironmentVariable("ASB_WIN_LOCK_HELPER");
}

// Starts the helper and waits for its first line.
bool startHolder(QProcess& p, const QStringList& args, QByteArray* line)
{
    p.start(helper(), args);
    if (!p.waitForStarted(10000)) return false;
    for (int i = 0; i < 300 && !p.canReadLine(); ++i) p.waitForReadyRead(100);
    *line = p.readLine().trimmed();
    return true;
}
}  // namespace

class tst_WinLocksNative : public QObject {
    Q_OBJECT

    QString m_root, m_lockDir;
    NtSeams& seams() { return nativeSeams(); }

    void publishPair(const QByteArray& id)
    {
        EngineLock applier(seams(), m_lockDir, LockRole::Applier);
        for (const char* name : {LockIdentity::kLockName, LockIdentity::kRecordName}) {
            const WriteResult w = LockIdentity::publish(applier.io(), applier.dir(), QString::fromLatin1(name), id);
            QVERIFY2(w.replaced && w.durable, qPrintable(w.error));
        }
    }
    // The lock directory, created as the Applier creates it (FileApplyLock), then apply.lock released.
    void prepareLockDirectory()
    {
        FileApplyLock lock(seams(), m_lockDir);
        QVERIFY2(lock.acquire(std::chrono::seconds(5)), qPrintable(lock.lastError()));
    }

private slots:
    void initTestCase() { QVERIFY2(QFileInfo::exists(helper()), "ASB_WIN_LOCK_HELPER must name asb_win_lock_helper.exe"); }
    void init()
    {
        m_root = scratch(QStringLiteral("lk"));
        m_lockDir = m_root + QStringLiteral("\\AetherSerialBridge");
    }
    void cleanup() { removeTree(m_root); }

    // ---- INV-97: created with the protected admin-only DACL --------------------------------------
    void apply_lock_created_protected()
    {
        prepareLockDirectory();
        QCOMPARE(sddlOf(m_lockDir), QStringLiteral("O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"));
        QCOMPARE(sddlOf(m_lockDir + QStringLiteral("\\apply.lock")), kLockSddl);
    }

    // ---- INV-66: one holder at a time across processes -------------------------------------------
    void lock_three_process_handoff()
    {
        prepareLockDirectory();
        QProcess p[3];
        QString out[3];
        for (int i = 0; i < 3; ++i) {
            out[i] = m_root + QStringLiteral("\\hold-%1.txt").arg(i);
            p[i].start(helper(), {QStringLiteral("apply"), m_lockDir, QStringLiteral("700"), out[i]});
        }
        QList<QPair<qint64, qint64>> spans;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(p[i].waitForFinished(90000));
            QCOMPARE(p[i].exitCode(), 0);
            const QList<QByteArray> f = readFile(out[i]).value_or(QByteArray()).trimmed().split(' ');
            QCOMPARE(f.size(), 2);
            spans.append({f[0].toLongLong(), f[1].toLongLong()});
        }
        std::sort(spans.begin(), spans.end());
        for (int i = 1; i < 3; ++i)
            QVERIFY2(spans[i].first >= spans[i - 1].second,
                     qPrintable(QStringLiteral("holders overlapped: %1..%2 and %3..%4")
                                    .arg(spans[i - 1].first).arg(spans[i - 1].second).arg(spans[i].first).arg(spans[i].second)));
    }

    void lock_holder_killed()
    {
        prepareLockDirectory();
        QProcess holder;
        QByteArray line;
        QVERIFY(startHolder(holder, {QStringLiteral("hold-apply"), m_lockDir}, &line));
        QCOMPARE(line, QByteArray("held"));
        FileApplyLock lock(seams(), m_lockDir);
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        holder.kill();   // TerminateProcess: no clean-up code runs in the holder
        QVERIFY(holder.waitForFinished(10000));
        QVERIFY2(lock.acquire(std::chrono::seconds(10)), qPrintable(lock.lastError()));   // the kernel released it
    }

    // ---- INV-97 / INV-66: a widened DACL is reset through a WRITE_DAC handle, and reported -------
    void wide_dacl_lock_reset_and_reported()
    {
        prepareLockDirectory();
        const QString path = m_lockDir + QStringLiteral("\\apply.lock");
        QVERIFY(setSddl(path, kWide, DACL_SECURITY_INFORMATION));
        QStringList reports;
        FileApplyLock lock(seams(), m_lockDir, systemClock(), [&](const QString& r) { reports << r; });
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(5));
        QVERIFY2(hold, qPrintable(lock.lastError()));
        QCOMPARE(sddlOf(path), kLockSddl);
        QCOMPARE(reports.size(), 1);
        QVERIFY(reports[0].contains(QLatin1String("apply.lock")));
    }
    void wide_dacl_reset_through_write_dac_handle()
    {
        // Same file object, repaired while the share-0 lock handle is open (WRITE_DAC is not a shared right).
        prepareLockDirectory();
        const QString path = m_lockDir + QStringLiteral("\\apply.lock");
        QVERIFY(setSddl(path, kWide, DACL_SECURITY_INFORMATION));
        FILE_ID_INFO before{};
        {
            HANDLE h = CreateFileW(W(path), FILE_READ_ATTRIBUTES, 7, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(h != INVALID_HANDLE_VALUE);
            QVERIFY(GetFileInformationByHandleEx(h, FileIdInfo, &before, sizeof before));
            CloseHandle(h);
        }
        FileApplyLock lock(seams(), m_lockDir);
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(5));
        QVERIFY(hold);
        FILE_ID_INFO after{};
        HANDLE h = CreateFileW(W(path), FILE_READ_ATTRIBUTES, 7, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        QVERIFY(GetFileInformationByHandleEx(h, FileIdInfo, &after, sizeof after));
        CloseHandle(h);
        QVERIFY(memcmp(&before, &after, sizeof before) == 0);   // repaired in place, not replaced
        QCOMPARE(sddlOf(path), kLockSddl);
    }

    void lock_handles_not_inheritable()
    {
        prepareLockDirectory();
        FileApplyLock lock(seams(), m_lockDir);
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(5));
        QVERIFY(hold);
        DWORD flags = 0xFF;
        QVERIFY(GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<WinLockHold*>(hold.get())->native()), &flags));
        QCOMPARE(flags & HANDLE_FLAG_INHERIT, DWORD(0));
        publishPair("0123456789abcdef0123456789abcdef");
        EngineLock engine(seams(), m_lockDir, LockRole::Service);
        LockResult why = LockResult::Busy;
        std::unique_ptr<LockHold> e = engine.tryAcquire(&why);
        QVERIFY(e);
        flags = 0xFF;
        QVERIFY(GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<WinLockHold*>(e.get())->native()), &flags));
        QCOMPARE(flags & HANDLE_FLAG_INHERIT, DWORD(0));
    }

    // ---- the account helper never touches an account it did not create -----------------
    // An account that exists before the helper runs (made here with NetUserAdd directly, with a known
    // password) keeps its password, its groups and its existence when the helper is asked for that name.
    void test_accounts_never_touch_an_existing_account()
    {
        struct Existing {   // removed even when a check below fails
            QString name = freshAccountName();
            QString password = QStringLiteral("Pre-%1-k7!Z").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
            NET_API_STATUS added = NERR_Success;
            Existing()
            {
                USER_INFO_1 ui{};
                ui.usri1_name = const_cast<LPWSTR>(W(name));
                ui.usri1_password = const_cast<LPWSTR>(W(password));
                ui.usri1_priv = USER_PRIV_USER;
                ui.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
                DWORD perr = 0;
                added = NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&ui), &perr);
            }
            ~Existing()
            {
                if (added == NERR_Success) NetUserDel(nullptr, W(name));
            }
            bool exists() const
            {
                LPBYTE info = nullptr;
                const NET_API_STATUS st = NetUserGetInfo(nullptr, W(name), 0, &info);
                if (info) NetApiBufferFree(info);
                return st == NERR_Success;
            }
            bool logsOnWithItsPassword() const
            {
                HANDLE t = nullptr;
                const bool ok = LogonUserW(W(name), L".", W(password), LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &t) != FALSE;
                if (t) CloseHandle(t);
                return ok;
            }
            QStringList groups() const
            {
                LPBYTE buf = nullptr;
                DWORD read = 0, total = 0;
                QStringList out;
                if (NetUserGetLocalGroups(nullptr, W(name), 0, LG_INCLUDE_INDIRECT, &buf, MAX_PREFERRED_LENGTH, &read, &total) == NERR_Success) {
                    const auto* g = reinterpret_cast<const LOCALGROUP_USERS_INFO_0*>(buf);
                    for (DWORD i = 0; i < read; ++i) out << QString::fromWCharArray(g[i].lgrui0_name);
                }
                if (buf) NetApiBufferFree(buf);
                return out;
            }
        } existing;
        QCOMPARE(existing.added, NET_API_STATUS(NERR_Success));
        QVERIFY(existing.logsOnWithItsPassword());
        const QStringList groupsBefore = existing.groups();
        QVERIFY(!groupsBefore.contains(QStringLiteral("Administrators"), Qt::CaseInsensitive));
        for (bool administrator : {true, false}) {
            {
                LocalAccount helper(existing.name, administrator);
                QVERIFY(!helper.created());
                QVERIFY(!helper.token());
                QVERIFY2(helper.error().contains(QLatin1String("already exists")), qPrintable(helper.error()));
                QVERIFY(existing.logsOnWithItsPassword());   // password not reset
                QCOMPARE(existing.groups(), groupsBefore);    // not made an administrator
            }
            QVERIFY(existing.exists());                       // not deleted by the helper's destructor
            QVERIFY(existing.logsOnWithItsPassword());
            QCOMPARE(existing.groups(), groupsBefore);
        }
        // A fresh account is the helper's own: namespaced, created, and gone once the helper is.
        QString mine;
        {
            LocalAccount fresh(false);
            QVERIFY2(fresh.token(), qPrintable(fresh.error()));
            QVERIFY(fresh.created());
            QVERIFY(fresh.name().startsWith(QLatin1String(names::kAccountPrefix)) && fresh.name() != existing.name);
            mine = fresh.name();
        }
        LPBYTE info = nullptr;
        QCOMPARE(NetUserGetInfo(nullptr, W(mine), 0, &info), NET_API_STATUS(NERR_UserNotFound));
        if (info) NetApiBufferFree(info);
    }

    // ---- real accounts (master §8.1: NetUserAdd + an in-process logon) ---------------------------
    void standard_user_cannot_open_existing_lock()
    {
        prepareLockDirectory();
        publishPair("0123456789abcdef0123456789abcdef");
        LocalAccount standard(false);
        QVERIFY2(standard.token(), qPrintable(standard.error()));
        Impersonate as(standard.token());
        QVERIFY(as.ok());
        for (const char* n : {"apply.lock", "engine.lock"}) {
            for (DWORD access : {DWORD(GENERIC_READ), DWORD(GENERIC_WRITE), DWORD(READ_CONTROL)}) {
                HANDLE h = CreateFileW(W(m_lockDir + QLatin1Char('\\') + QLatin1String(n)), access, 7, nullptr, OPEN_EXISTING, 0, nullptr);
                const DWORD e = GetLastError();
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                QVERIFY2(h == INVALID_HANDLE_VALUE && e == ERROR_ACCESS_DENIED, n);
            }
        }
    }
    void standard_user_cannot_open_engine_lock()
    {
        prepareLockDirectory();
        publishPair("0123456789abcdef0123456789abcdef");
        LocalAccount standard(false);
        QVERIFY2(standard.token(), qPrintable(standard.error()));
        {
            Impersonate as(standard.token());
            HANDLE h = CreateFileW(W(m_lockDir + QStringLiteral("\\engine.lock")), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            const DWORD e = GetLastError();
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            QCOMPARE(e, DWORD(ERROR_ACCESS_DENIED));   // cannot even open it to obstruct the engine
        }
        EngineLock engine(seams(), m_lockDir, LockRole::Service);
        LockResult why = LockResult::Busy;
        QVERIFY(engine.tryAcquire(&why));
    }

    void lock_standard_user_cannot_create_dir()
    {
        const std::optional<QString> production = programFilesLockDirectory();
        QVERIFY(production);
        LocalAccount standard(false);
        QVERIFY2(standard.token(), qPrintable(standard.error()));
        const QString planted = *production + QStringLiteral("-std-%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
        {
            Impersonate as(standard.token());
            // Where the lock lives, a standard user cannot pre-create anything ...
            const BOOL made = CreateDirectoryW(W(planted), nullptr);
            const DWORD e = GetLastError();
            if (made) RemoveDirectoryW(W(planted));
            QVERIFY(!made);
            QCOMPARE(e, DWORD(ERROR_ACCESS_DENIED));
        }
        // ... unlike %ProgramData%, where the would-be lock directory could be planted:
        wchar_t pd[MAX_PATH] = {};
        QVERIFY(ExpandEnvironmentStringsW(L"%ProgramData%", pd, MAX_PATH));
        const QString inData = QString::fromWCharArray(pd) + QStringLiteral("\\AetherSerialBridge-lk-%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
        {
            Impersonate as(standard.token());
            QVERIFY(CreateDirectoryW(W(inData), nullptr));
        }
        // and such a planted directory is refused by owner, with nothing created in it.
        FileApplyLock lock(seams(), inData);
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QVERIFY2(lock.lastError().contains(QLatin1String("owned by")), qPrintable(lock.lastError()));
        QCOMPARE(GetFileAttributesW(W(inData + QStringLiteral("\\apply.lock"))), INVALID_FILE_ATTRIBUTES);
        RemoveDirectoryW(W(inData));
        QVERIFY(production->endsWith(QLatin1String("\\AetherSerialBridge")));
        wchar_t pf[MAX_PATH] = {};
        QVERIFY(ExpandEnvironmentStringsW(L"%ProgramFiles%", pf, MAX_PATH));
        QVERIFY2(production->startsWith(QString::fromWCharArray(pf), Qt::CaseInsensitive), qPrintable(*production));
    }

    void lock_second_admin_account()
    {
        // Another administrator (a full token, impersonated in-process) contends for the same file.
        prepareLockDirectory();
        LocalAccount admin2(true);
        QVERIFY2(admin2.token(), qPrintable(admin2.error()));
        QProcess holder;
        QByteArray line;
        QVERIFY(startHolder(holder, {QStringLiteral("hold-apply"), m_lockDir}, &line));
        QCOMPARE(line, QByteArray("held"));
        {
            Impersonate as(admin2.token());
            FileApplyLock other(seams(), m_lockDir);
            QVERIFY(!other.acquire(std::chrono::seconds(1)));
        }
        holder.kill();
        QVERIFY(holder.waitForFinished(10000));
        Impersonate as(admin2.token());
        FileApplyLock other(seams(), m_lockDir);
        QVERIFY2(other.acquire(std::chrono::seconds(10)), qPrintable(other.lastError()));
    }

    // ---- INV-104: engine.lock validated on every open ----------------------------------------------
    void engine_lock_dacl_validated()
    {
        prepareLockDirectory();
        publishPair("0123456789abcdef0123456789abcdef");
        const QString path = m_lockDir + QStringLiteral("\\engine.lock");
        QCOMPARE(sddlOf(path), kLockSddl);
        QVERIFY(setSddl(path, kWide, DACL_SECURITY_INFORMATION));
        // The service refuses it: exit 4 through BridgeRunner, before any config is read.
        {
            EngineLock service(seams(), m_lockDir, LockRole::Service);
            LockResult why = LockResult::Acquired;
            QVERIFY(!service.tryAcquire(&why));
            QCOMPARE(why, LockResult::InsecureFile);   // the directory is fine: install/Repair may replace it
            FakeWorld w;
            w.checkLock = false;
            FakeFolder folder(w);
            FakeEngine engine(w.log);
            FakeSideChannel side(w.log);
            NoFirewallMonitor monitor;
            FakeWatchdog watchdog(w.log, w.clock);
            SpyFileIo io(w, w.files, QStringLiteral("folder"));
            FileStartToken token(folder, io);
            BridgeRunner runner(service, token, folder, engine, side, monitor, watchdog, QByteArrayLiteral("11111111111111111111111111111111"), w.clock);
            const RunnerStart s = runner.start();
            QCOMPARE(s, RunnerStart::EngineLockBusy);
            QCOMPARE(exitCode(s), 4);
            QVERIFY(!engine.started);
            QVERIFY(folder.reads.isEmpty());
        }
        QCOMPARE(sddlOf(path), QStringLiteral("O:BA") + kWide);   // the service changed nothing
        // The Applier repairs it through a WRITE_DAC handle and reports it.
        QStringList reports;
        EngineLock applier(seams(), m_lockDir, LockRole::Applier, systemClock(), [&](const QString& r) { reports << r; });
        LockResult why = LockResult::Busy;
        std::unique_ptr<LockHold> hold = applier.acquireUntil(systemClock().now() + std::chrono::seconds(5), &why);
        QVERIFY2(hold, qPrintable(applier.lastError()));
        QCOMPARE(sddlOf(path), kLockSddl);
        QCOMPARE(reports.size(), 1);
        hold.reset();
        EngineLock service(seams(), m_lockDir, LockRole::Service);
        QVERIFY(service.tryAcquire(&why));
    }

    void engine_lock_held_by_another_process()
    {
        prepareLockDirectory();
        publishPair("0123456789abcdef0123456789abcdef");
        QProcess engineProcess;
        QByteArray line;
        QVERIFY(startHolder(engineProcess, {QStringLiteral("hold-engine"), m_lockDir}, &line));
        QCOMPARE(line, QByteArray("held"));
        EngineLock applier(seams(), m_lockDir, LockRole::Applier);
        LockResult why = LockResult::Acquired;
        QVERIFY(!applier.acquireUntil(systemClock().now() + std::chrono::seconds(1), &why));
        QCOMPARE(why, LockResult::Busy);
        QCOMPARE(applier.peekLock().status, LockFileRead::Status::Unreadable);   // exists; content only through a hold
        QCOMPARE(applier.readRecord().status, LockFileRead::Status::Ok);         // the record never waits for the holder
        engineProcess.kill();
        QVERIFY(engineProcess.waitForFinished(10000));
        std::unique_ptr<LockHold> hold = applier.acquireUntil(systemClock().now() + std::chrono::seconds(10), &why);
        QVERIFY(hold);
        QCOMPARE(LockIdentity::classify(applier.readHeldLock(), applier.readRecord()), LockIdentity::PairState::Matching);
    }

    // ---- the WS-5 fold's ordering, natively: release, replace, re-acquire, re-check ----------------
    void engine_lock_replace_needs_release()
    {
        prepareLockDirectory();
        publishPair("0123456789abcdef0123456789abcdef");
        EngineLock applier(seams(), m_lockDir, LockRole::Applier);
        LockResult why = LockResult::Busy;
        std::unique_ptr<LockHold> hold = applier.tryAcquire(&why);
        QVERIFY(hold);
        const QByteArray fresh = "fedcba9876543210fedcba9876543210";
        WriteResult w = LockIdentity::publish(applier.io(), applier.dir(), QString::fromLatin1(LockIdentity::kLockName), fresh);
        QVERIFY(!w.replaced);   // our own share-0 handle blocks the replace (STATUS_SHARING_VIOLATION)
        QVERIFY2(w.error.contains(QLatin1String("0xc0000043")), qPrintable(w.error));
        QCOMPARE(QDir(m_lockDir).entryList({QStringLiteral("*.asbtmp")}, QDir::Files).size(), 0);   // temp disposed of
        hold.reset();
        w = LockIdentity::publish(applier.io(), applier.dir(), QString::fromLatin1(LockIdentity::kLockName), fresh);
        QVERIFY2(w.replaced && w.durable, qPrintable(w.error));
        hold = applier.tryAcquire(&why);
        QVERIFY(hold);
        QCOMPARE(LockIdentity::parse(applier.readHeldLock().bytes).value_or(QByteArray()), fresh);
        QCOMPARE(sddlOf(m_lockDir + QStringLiteral("\\engine.lock")), kLockSddl);
    }

    // ---- the production roots: the data folder only through serviceLocations(), the lock directory
    //      from FOLDERID_ProgramFiles; neither has a fallback ---------------------------------------
    void windows_paths_from_known_folders()
    {
        QString error;
        const std::optional<WindowsPaths> paths = windowsPaths(&error);
        QVERIFY2(paths, qPrintable(error));
        wchar_t pd[MAX_PATH] = {}, pf[MAX_PATH] = {};
        QVERIFY(ExpandEnvironmentStringsW(L"%ProgramData%", pd, MAX_PATH));
        QVERIFY(ExpandEnvironmentStringsW(L"%ProgramFiles%", pf, MAX_PATH));
        QCOMPARE(paths->dataFolder.toLower(), (QString::fromWCharArray(pd) + QStringLiteral("\\AetherSerialBridge")).toLower());
        QCOMPARE(paths->lockDirectory.toLower(), (QString::fromWCharArray(pf) + QStringLiteral("\\AetherSerialBridge")).toLower());
        QCOMPARE(paths->lockDirectory, programFilesLockDirectory().value_or(QString()));
    }

    // ---- INV-116 (WS-5 deferred): the lock directory's volume -------------------------------------
    void lock_directory_on_fat_refused()
    {
        const QString fat = qEnvironmentVariable("ASB_FAT_VOLUME");
        if (fat.isEmpty()) {
            if (qEnvironmentVariableIsSet("CI")) QFAIL("ASB_FAT_VOLUME is not set");
            QSKIP("no FAT32 volume");
        }
        const QString dir = nativePath(fat) + QStringLiteral("lk-%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
        FileApplyLock lock(seams(), dir);
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QCOMPARE(GetFileAttributesW(W(dir)), INVALID_FILE_ATTRIBUTES);   // nothing created
        QVERIFY(CreateDirectoryW(W(dir), nullptr));
        EngineLock engine(seams(), dir, LockRole::Applier);
        QString why;
        QVERIFY(!engine.filesystemSupported(&why));
        QVERIFY2(why.contains(QLatin1String("FAT")), qPrintable(why));
        RemoveDirectoryW(W(dir));
    }
};

QTEST_GUILESS_MAIN(tst_WinLocksNative)
#include "tst_WinLocksNative.moc"
