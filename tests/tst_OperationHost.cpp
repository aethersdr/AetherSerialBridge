// OperationHost (D57, INV-118): interim vs final, exclusion held while a native call is outstanding,
// cleanup on the worker before the final, busy until the final is delivered, one busy gate for every
// operation. The native call is a fake firewall call blocked on a gate while the fake clock is moved
// past its phase deadline.

#include "core/OperationHost.h"
#include "support/Fakes.h"

#include <QCoreApplication>
#include <QTest>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace asb;
using namespace asb::test;

namespace {

SaveRequest request(const QByteArray& bytes)
{
    SaveRequest r;
    r.config = bytes;
    r.source.kind = Source::Committed;
    r.source.sha256 = digest(configA());
    return r;
}

struct Observer : IOperationObserver {
    QList<Interim> interims;
    QList<Result> finals;
    QStringList order;
    QThread* interimThread = nullptr;
    QThread* finishedThread = nullptr;
    std::function<void(const Result&)> onFinished;
    void interim(const Interim& i) override
    {
        interims << i;
        order << QStringLiteral("interim");
        interimThread = QThread::currentThread();
    }
    void finished(const Result& r) override
    {
        finals << r;
        order << QStringLiteral("final");
        finishedThread = QThread::currentThread();
        if (onFinished) onFinished(r);
    }
};

// A native call that does not return until the test opens the gate.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false, open = false;
    void enter()
    {
        std::unique_lock lk(m);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return open; });
    }
    void waitEntered()
    {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return entered; });
    }
    void release()
    {
        std::lock_guard g(m);
        open = true;
        cv.notify_all();
    }
};

// Opens the gate when the test function returns (declared after the host, so it runs first): a failed
// check never leaves the worker blocked while the host waits for it.
struct OpenOnExit {
    Gate& gate;
    ~OpenOnExit() { gate.release(); }
};

// The firewall sync "takes" 61 s of fake time (past its 60 s phase deadline), then blocks on the gate.
void blockSyncPastDeadline(Rig& r, Gate& gate)
{
    r.firewall.onCall = [&r, &gate](const QString& m) {
        if (m != QLatin1String("sync")) return;
        r.world.clock.advance(std::chrono::seconds(61));
        gate.enter();
    };
}

// INV-118: no operation path may end the process. A process exit while a test function runs (from
// any thread) is turned into a named failure instead of an exit code ctest would read as success.
std::atomic<bool> g_inTest{false};
void exitDuringTest()
{
    if (!g_inTest.load()) return;
    const char* fn = QTest::currentTestFunction();
    std::fprintf(stdout, "FAIL!  : tst_OperationHost::%s() the process exited during the test\n", fn ? fn : "?");
    std::fflush(stdout);
    std::_Exit(1);
}

void spinUntil(const std::function<bool()>& done)
{
    for (int i = 0; i < 5000 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace

class tst_OperationHost : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { std::atexit(exitDuringTest); }
    void init() { g_inTest = true; }
    void cleanup() { g_inTest = false; }
    void interimBeforeFinal();
    void noFinalWhileNativeCallOutstanding();
    void locksHeldUntilReturn();
    void cleanupOnWorkerThenFinal();
    void deadlineCoincidesWithReturn();
    void delayedInterimSuppressedById();
    void busyUntilFinalDelivered();
    void repairAndDiscardUseBusyGate();
    void finishedMaySubmitTheNextOperation();
    void interimWordsOnlyTheEvidence_data();
    void interimWordsOnlyTheEvidence();
    void nothingHeldWhenTheFinalIsDelivered();
};

void tst_OperationHost::interimBeforeFinal()
{
    Rig r;
    r.installedRunning(configA());
    Gate gate;
    blockSyncPastDeadline(r, gate);
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    OpenOnExit open{gate};
    Observer o;
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    gate.waitEntered();
    QTRY_COMPARE(o.interims.size(), 1);
    QCOMPARE(o.interims[0].phase, phaseName(Phase::Firewall));
    QVERIFY(o.interims[0].message.contains(QStringLiteral("firewall change")));
    QCOMPARE(o.interimThread, QThread::currentThread());
    QTest::qWait(50);
    QCOMPARE(o.interims.size(), 1);   // at most once per phase
    gate.release();
    QTRY_COMPARE(o.finals.size(), 1);
    QCOMPARE(o.order, (QStringList{QStringLiteral("interim"), QStringLiteral("final")}));
    QCOMPARE(o.finals[0].outcome, Outcome::StateUnknown);
    QCOMPARE(o.finals[0].step, phaseName(Phase::Firewall));
}

// INV-118. Kills: posting the final result at the deadline while the native call is still outstanding.
void tst_OperationHost::noFinalWhileNativeCallOutstanding()
{
    Rig r;
    r.installedRunning(configA());
    Gate gate;
    blockSyncPastDeadline(r, gate);
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    OpenOnExit open{gate};
    Observer o;
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    gate.waitEntered();
    QTRY_COMPARE(o.interims.size(), 1);
    QTest::qWait(100);
    QCOMPARE(o.finals.size(), 0);
    QVERIFY(host.busy());
    gate.release();
    QTRY_COMPARE(o.finals.size(), 1);
    QVERIFY(!host.busy());
}

// INV-118. Kills: releasing apply.lock (or engine.lock) at the deadline while the worker is blocked.
void tst_OperationHost::locksHeldUntilReturn()
{
    Rig r;
    r.installedRunning(configA());
    Gate gate;
    blockSyncPastDeadline(r, gate);
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    OpenOnExit open{gate};
    Observer o;
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    gate.waitEntered();
    QTRY_COMPARE(o.interims.size(), 1);
    // A second Applier (another process, in reality) cannot get in meanwhile.
    const bool applyHeld = r.world.applyLock.held();
    const bool engineHeld = r.engineLock.appliersHold();
    std::thread other([&] { Applier(*r.platform, r.world.clock).stop(); });
    spinUntil([&] { return r.world.applyLock.waiting.load() == 1; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int waiting = r.world.applyLock.waiting.load();
    gate.release();
    other.join();
    QVERIFY(applyHeld);
    QVERIFY(engineHeld);
    QCOMPARE(waiting, 1);
    QTRY_COMPARE(o.finals.size(), 1);
    QVERIFY(!r.world.applyLock.held());
}

// INV-118. Kills: exiting (or posting the final) before failClosed ran on the worker.
void tst_OperationHost::cleanupOnWorkerThenFinal()
{
    Rig r;
    r.installedRunning(configA());
    Gate gate;
    blockSyncPastDeadline(r, gate);
    QList<QThread*> inhibitThreads;
    int inhibitsBeforeFinal = -1;
    r.service.onCall = [&](const QString& m) {
        if (m == QLatin1String("inhibitStarts")) inhibitThreads << QThread::currentThread();
    };
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    OpenOnExit open{gate};
    Observer o;
    o.onFinished = [&](const Result&) { inhibitsBeforeFinal = int(inhibitThreads.size()); };
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    gate.waitEntered();
    gate.release();
    QTRY_COMPARE(o.finals.size(), 1);
    QCOMPARE(inhibitsBeforeFinal, 2);   // the barrier's, then failClosed's — both before the final
    for (QThread* t : inhibitThreads) QVERIFY(t != QThread::currentThread());
    QCOMPARE(o.finishedThread, QThread::currentThread());
    QVERIFY(o.finals[0].postcondition.contains(QStringLiteral("verified")));
    QVERIFY(!r.world.applyLock.held());
}

// INV-118. Kills: an interim delivered after the final when the deadline coincides with the return.
void tst_OperationHost::deadlineCoincidesWithReturn()
{
    for (int round = 0; round < 20; ++round) {
        Rig r;
        r.installedRunning(configA());
        r.firewall.onCall = [&r](const QString& m) {
            if (m == QLatin1String("sync")) r.world.clock.advance(std::chrono::seconds(61));   // returns at once, late
        };
        OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(1));
        Observer o;
        QVERIFY(host.submitRun(request(configB()), false, false, o));
        QTRY_COMPARE(o.finals.size(), 1);
        QTest::qWait(10);
        const int fin = int(o.order.indexOf(QStringLiteral("final")));
        QVERIFY2(o.order.lastIndexOf(QStringLiteral("interim")) < fin, qPrintable(o.order.join(',')));
        QCOMPARE(o.finals.size(), 1);
    }
}

// INV-118. Kills: delivering an interim whose operation is no longer inside the phase — one posted
// while the worker was blocked but delivered after it returned.
void tst_OperationHost::delayedInterimSuppressedById()
{
    Rig r;
    r.installedRunning(configA());
    Gate gate;
    blockSyncPastDeadline(r, gate);
    OperationHost host(*r.platform, r.world.clock, std::chrono::hours(1));   // only the explicit poll runs
    OpenOnExit open{gate};
    Observer o;
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    gate.waitEntered();
    host.pollForInterim();   // queued for operation 1, not yet delivered
    gate.release();
    spinUntil([&] { return host.workerReturned(); });
    QVERIFY(host.workerReturned());
    QTRY_COMPARE(o.finals.size(), 1);
    QVERIFY2(o.interims.isEmpty(), qPrintable(o.order.join(',')));
}

// INV-118. Kills: clearing busy when the final is posted rather than delivered.
void tst_OperationHost::busyUntilFinalDelivered()
{
    Rig r;
    r.installedRunning(configA());
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    Observer o;
    QVERIFY(host.submitStop(o));
    spinUntil([&] { return host.workerReturned(); });   // no event processing here
    QVERIFY(host.workerReturned());
    QVERIFY(host.busy());
    QCOMPARE(o.finals.size(), 0);
    QTRY_COMPARE(o.finals.size(), 1);
    QVERIFY(!host.busy());
}

// INV-118. Kills: Repair or Discard pending (or any operation) bypassing the busy gate.
void tst_OperationHost::repairAndDiscardUseBusyGate()
{
    Rig r;
    r.installedRunning(configA());
    Gate gate;
    blockSyncPastDeadline(r, gate);
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    OpenOnExit open{gate};
    Observer o, other;
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    gate.waitEntered();
    QVERIFY(!host.submitRepair(other));
    QVERIFY(!host.submitDiscardPending(other));
    QVERIFY(!host.submitStop(other));
    QVERIFY(!host.submitStart(other));
    QVERIFY(!host.submitEdit([](BridgeConfig&) {}, other));
    QVERIFY(!host.submitSaveAndRemove(std::nullopt, other));
    QVERIFY(!host.submitDeactivate(std::nullopt, other));
    QVERIFY(!host.submitRun(std::nullopt, false, false, other));
    QVERIFY(!host.submit([](Applier& a) { return a.repair(); }, other));
    QCOMPARE(r.folder.renames, 0);
    gate.release();
    QTRY_COMPARE(o.finals.size(), 1);
    QTest::qWait(20);
    QVERIFY(other.finals.isEmpty() && other.interims.isEmpty());
    QCOMPARE(r.world.log.count(QStringLiteral("applyLock:acquired")), 1);
}

void tst_OperationHost::finishedMaySubmitTheNextOperation()
{
    Rig r;
    r.installedRunning(configA());
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    Observer first, second;
    bool accepted = false;
    first.onFinished = [&](const Result&) { accepted = host.submitStart(second); };
    QVERIFY(host.submitStop(first));
    QTRY_COMPARE(second.finals.size(), 1);
    QVERIFY(accepted);
    QCOMPARE(first.finals[0].outcome, Outcome::Ok);
    QCOMPARE(second.finals[0].outcome, Outcome::Ok);
    QVERIFY(host.currentOperationId() == 2);
}

// WS-5 round 6 (class audit): an interim says "the bridge stays stopped" only while the operation holds the
// evidence for it (a validated engine.lock hold, the engine-free proof, or no service): during R6 it does.
// Before detection, during the barrier and during the cleanup it does not, so the interim claims nothing
// about the bridge there. The evidence is engineClaim's, as for every result.
void tst_OperationHost::interimWordsOnlyTheEvidence_data()
{
    QTest::addColumn<QString>("call");
    QTest::addColumn<int>("phase");
    QTest::addColumn<int>("late");
    QTest::addColumn<bool>("proven");
    QTest::newRow("prologue: the installation query") << QStringLiteral("detect") << int(Phase::Prologue) << 16 << false;
    QTest::newRow("barrier: blocking starts") << QStringLiteral("inhibitStarts") << int(Phase::Barrier) << 31 << false;
    QTest::newRow("firewall: the sync, engine.lock held and matching") << QStringLiteral("sync") << int(Phase::Firewall) << 61
                                                                        << true;
}

void tst_OperationHost::interimWordsOnlyTheEvidence()
{
    QFETCH(QString, call);
    QFETCH(int, phase);
    QFETCH(int, late);
    QFETCH(bool, proven);
    Rig r;
    r.installedRunning(configA());
    Gate gate;
    auto once = std::make_shared<bool>(false);
    const auto block = [&r, &gate, call, late, once](const QString& m) {
        if (*once || m != call) return;
        *once = true;
        r.world.clock.advance(std::chrono::seconds(late));
        gate.enter();
    };
    if (call == QLatin1String("sync")) r.firewall.onCall = block;
    else r.service.onCall = block;
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    OpenOnExit open{gate};
    Observer o;
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    gate.waitEntered();
    QTRY_COMPARE(o.interims.size(), 1);
    QCOMPARE(o.interims[0].phase, phaseName(Phase(phase)));
    const QString msg = o.interims[0].message;
    if (proven) {
        QVERIFY2(msg.contains(QStringLiteral("the bridge stays stopped until it finishes")), qPrintable(msg));
    } else {
        QVERIFY2(!claimsStop(msg), qPrintable(msg));
        QVERIFY2(msg.contains(QStringLiteral("the result says what state the bridge is in")), qPrintable(msg));
    }
    gate.release();
    QTRY_COMPARE(o.finals.size(), 1);
}

// v1.1 INV-128: when the final is delivered, the host's Platform holds nothing between operations: no lock, no lock
// directory's handle and no data folder's handle (the Applier inspected the folder; the host closes it on the worker).
// Kills: the worker not closing the data folder's handle after the operation (the window's reload would hide it).
void tst_OperationHost::nothingHeldWhenTheFinalIsDelivered()
{
    Rig r;
    r.installedRunning(configA());
    OperationHost host(*r.platform, r.world.clock, std::chrono::milliseconds(5));
    Observer o;
    bool folderOpen = true, lockDirectoryOpen = true;
    o.onFinished = [&](const Result&) {
        folderOpen = r.folder.directoryOpen;
        lockDirectoryOpen = r.engineLock.directoryOpen;
    };
    QVERIFY(host.submitRun(request(configB()), false, false, o));
    QTRY_COMPARE(o.finals.size(), 1);
    QCOMPARE(o.finals[0].outcome, Outcome::Ok);
    QVERIFY2(r.folder.inspections > 0, "the operation never inspected the data folder");
    QVERIFY2(!folderOpen, "the data folder's handle is still open when the final is delivered");
    QVERIFY2(!lockDirectoryOpen, "the lock directory's handle is still open when the final is delivered");
    QVERIFY(!r.world.applyLock.held());
}

QTEST_GUILESS_MAIN(tst_OperationHost)
#include "tst_OperationHost.moc"
