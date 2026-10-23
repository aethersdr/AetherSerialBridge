// DeadlineWatchdog (WS-4; master §4.7 N9-1, D56, D59): the IDeadlineWatchdog rules on a real thread,
// in-process with the expiry hooked, and the real faultDie in a child process.

#include "core/DeadlineWatchdog.h"
#include "support/EngineTestSupport.h"

#include <QElapsedTimer>
#include <QTest>

#include <atomic>
#include <thread>

using namespace asb;
using namespace asb::test;
using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;

namespace {

// The expiry hooked: records the code and when it fired, never ends the process.
struct Fired {
    std::atomic<int> code{-1};
    std::atomic<int> count{0};
    QElapsedTimer clock;
    std::atomic<qint64> atMs{-1};
    Fired() { clock.start(); }
    DeadlineWatchdog::Hooks hooks()
    {
        DeadlineWatchdog::Hooks h;
        h.expire = [this](int c) {
            code = c;
            atMs = clock.elapsed();
            ++count;
        };
        return h;
    }
};

// Child-process scenarios: the real faultDie through the watchdog thread.
int runScenario(const QString& s)
{
    DeadlineWatchdog w;   // created at "process start"
    if (s == QLatin1String("arm")) {
        w.arm(Steady::now() + 200ms, 9);
    } else if (s == QLatin1String("tighten")) {
        w.arm(Steady::now() + 1h, 2);
        w.tighten(Steady::now() + 50ms, 8);
    } else if (s == QLatin1String("expireNow")) {
        w.arm(Steady::now() + 1h, 11);
        w.expireNow();
    } else {
        return 100;
    }
    std::this_thread::sleep_for(30s);
    return 101;   // the watchdog did not end the process
}

}  // namespace

class tst_DeadlineWatchdog : public QObject {
    Q_OBJECT

private slots:
    void deadlineChangeWakes()
    {
        Fired f;
        DeadlineWatchdog w(f.hooks());
        w.arm(Steady::now() + 1h, 3);
        std::this_thread::sleep_for(50ms);   // the thread is inside a full 1 s slice now
        const qint64 t0 = f.clock.elapsed();
        w.tighten(Steady::now() + 30ms, 4);
        QTRY_COMPARE_WITH_TIMEOUT(f.count.load(), 1, 3000);
        QCOMPARE(f.code.load(), 4);
        const qint64 took = f.atMs - t0;
        QVERIFY2(took >= 10 && took < 500, qPrintable(QString::number(took)));
    }

    void tightenNeverLater()
    {
        Fired f;
        DeadlineWatchdog w(f.hooks());
        w.arm(Steady::now() + 150ms, 3);
        w.tighten(Steady::now() + 10s, 4);   // later: ignored, code unchanged
        QTRY_COMPARE_WITH_TIMEOUT(f.count.load(), 1, 3000);
        QCOMPARE(f.code.load(), 3);
        QVERIFY(f.atMs < 1000);
    }

    void extendNeverEarlier()
    {
        Fired f;
        DeadlineWatchdog w(f.hooks());
        w.arm(Steady::now() + 400ms, 3);
        w.extend(Steady::now() + 20ms);   // earlier: ignored
        std::this_thread::sleep_for(250ms);
        QCOMPARE(f.count.load(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(f.count.load(), 1, 3000);
        QVERIFY2(f.atMs >= 370, qPrintable(QString::number(f.atMs)));
    }

    void extendMovesLater()
    {
        Fired f;
        DeadlineWatchdog w(f.hooks());
        w.arm(Steady::now() + 150ms, 5);
        w.extend(Steady::now() + 700ms);
        std::this_thread::sleep_for(400ms);
        QCOMPARE(f.count.load(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(f.count.load(), 1, 3000);
        QCOMPARE(f.code.load(), 5);
    }

    void tightenActivatesANeverArmedWatchdog()
    {
        Fired f;
        DeadlineWatchdog w(f.hooks());
        w.extend(Steady::now() + 10ms);   // never armed: ignored
        std::this_thread::sleep_for(100ms);
        QCOMPARE(f.count.load(), 0);
        w.tighten(Steady::now() + 50ms, 6);
        QTRY_COMPARE_WITH_TIMEOUT(f.count.load(), 1, 3000);
        QCOMPARE(f.code.load(), 6);
    }

    void neverArmedNeverFiresAndExpireNowUsesTheGenericCode()
    {
        Fired f;
        DeadlineWatchdog w(f.hooks());
        std::this_thread::sleep_for(1200ms);
        QCOMPARE(f.count.load(), 0);
        QVERIFY(w.wakeups() >= 1);   // slices of at most 1 s
        w.expireNow();
        QCOMPARE(f.count.load(), 1);   // at once, on the calling thread
        QCOMPARE(f.code.load(), kNeverArmedCode);
        w.expireNow();
        QCOMPARE(f.count.load(), 1);
    }

    void priorityFailureIgnored()
    {
        Fired f;
        auto h = f.hooks();
        h.raisePriority = [] { return false; };
        DeadlineWatchdog w(h);
        w.arm(Steady::now() + 50ms, 7);
        QTRY_COMPARE_WITH_TIMEOUT(f.count.load(), 1, 3000);
        QCOMPARE(f.code.load(), 7);
        QVERIFY(!w.priorityRaised());
    }

    // Running time stands still while the machine sleeps: a deadline does not pass during a
    // "suspend" however much wall/steady time goes by, and passes once running time moves again.
    void runningTimeAcrossSuspend()
    {
        Fired f;
        std::atomic<qint64> fakeNs{1'000'000'000'000};
        auto h = f.hooks();
        h.clock = [&] { return RunningTime::Duration(fakeNs.load()); };
        DeadlineWatchdog w(h);
        w.arm(Steady::now() + 300ms, 9);   // running deadline = fake now + 300 ms
        std::this_thread::sleep_for(1500ms);   // "suspended": running time frozen
        QCOMPARE(f.count.load(), 0);
        fakeNs += 200'000'000;   // resume; 200 ms of running time
        std::this_thread::sleep_for(1200ms);
        QCOMPARE(f.count.load(), 0);
        fakeNs += 150'000'000;   // now past the deadline
        QTRY_COMPARE_WITH_TIMEOUT(f.count.load(), 1, 3000);
        QCOMPARE(f.code.load(), 9);
    }

    // The real primitive: faultDie on the watchdog thread ends the process with the code.
    void realWatchdogEndsTheProcess()
    {
        const ProbeResult r = runProbe(QStringLiteral("arm"));
        QVERIFY(r.finished);
        QCOMPARE(r.exitCode, 9);
        QVERIFY2(r.ms >= 150 && r.ms < 5000, qPrintable(QString::number(r.ms)));
    }

    void realTightenWakesTheThread()
    {
        const ProbeResult r = runProbe(QStringLiteral("tighten"));
        QVERIFY(r.finished);
        QCOMPARE(r.exitCode, 8);
        QVERIFY2(r.ms < 3000, qPrintable(QString::number(r.ms)));
    }

    void realExpireNow()
    {
        const ProbeResult r = runProbe(QStringLiteral("expireNow"));
        QVERIFY(r.finished);
        QCOMPARE(r.exitCode, 11);
    }
};

int main(int argc, char** argv)
{
    const QString probe = qEnvironmentVariable("ASB_PROBE");
    if (!probe.isEmpty()) return runScenario(probe);
    QCoreApplication app(argc, argv);
    tst_DeadlineWatchdog t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_DeadlineWatchdog.moc"
