// The running-time base of the deadline watchdogs (N9-1; the README builder nit for WS-3/WS-4: how a
// steady_clock::time_point maps to QueryUnbiasedInterruptTime / CLOCK_MONOTONIC) and the reference
// model of the IDeadlineWatchdog state rules (Exit.h).

#include "core/Exit.h"
#include "core/RunningTime.h"

#include <QTest>

#include <thread>

using namespace asb;
using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;
using RunningTime::Duration;

class tst_RunningTime : public QObject {
    Q_OBJECT

private slots:
    void nowIsMonotonicAndAdvances()
    {
        const Duration a = RunningTime::now();
        std::this_thread::sleep_for(60ms);   // QueryUnbiasedInterruptTime moves in ~15.6 ms timer ticks
        const Duration b = RunningTime::now();
        QVERIFY(b >= a + 30ms);
        QVERIFY(b < a + 10s);
    }

    // The mapping keeps the remaining budget at the call.
    void fromSteadyKeepsTheRemainingBudget()
    {
        const Steady::time_point s0 = Steady::time_point(1000s);
        const Duration r0 = 5000s;
        QCOMPARE(RunningTime::fromSteady(s0 + 35s, s0, r0), r0 + 35s);
        QCOMPARE(RunningTime::fromSteady(s0, s0, r0), r0);
        QCOMPARE(RunningTime::fromSteady(s0 - 3s, s0, r0), r0 - 3s);   // already past: already expired
    }

    void neverStaysNever()
    {
        QCOMPARE(RunningTime::fromSteady(Steady::time_point::max(), Steady::time_point(1s), 1s), RunningTime::kNever);
        QCOMPARE(RunningTime::fromSteady(Steady::time_point::max()), RunningTime::kNever);
    }

    void saturatesInsteadOfOverflowing()
    {
        const auto huge = Steady::time_point(Steady::duration(Steady::duration::max().count() - 1));
        const Duration r = RunningTime::fromSteady(huge, Steady::time_point(0s), 10s);
        QVERIFY(r < RunningTime::kNever);   // finite deadlines never collide with "never"
        QVERIFY(r > 1000000h);
        const auto tiny = Steady::time_point(Steady::duration(Steady::duration::min().count() + 1));
        QVERIFY(RunningTime::fromSteady(tiny, Steady::time_point(1000s), 10s) < 0s);
    }

    void liveMappingIsCloseToTheBudget()
    {
        const Duration before = RunningTime::now();
        const Steady::time_point deadline = Steady::now() + 2s;
        const Duration d = RunningTime::fromSteady(deadline);
        const Duration after = RunningTime::now();
        QVERIFY(d >= before + 2s - 50ms);   // clock granularity (QueryUnbiasedInterruptTime ticks ~15.6 ms)
        // Never later than requested: the running sample precedes the steady one, so the skew between
        // them only subtracts. `after` is sampled after the call, so there is no tolerance on this side.
        QVERIFY(d <= after + 2s);
    }

    // A pause between the two samples must never extend the converted deadline. The fake
    // clocks are one timeline (running time = steady time) on which the thread is descheduled for 3 s
    // right after the FIRST sample, whichever clock that is. With running time sampled first the
    // result is 1000 + (1002 - 1003) = 999 s. Sampling steady first reads 1000, the pause follows, and
    // running time then reads 1003: 1003 + (1002 - 1000) = 1005 s, three seconds past the requested 1002 s.
    void pauseBetweenSamplesNeverExtendsTheDeadline()
    {
        const Steady::time_point deadline(1002s);
        for (const auto pause : {0s, 3s, 3600s}) {
            Duration t = 1000s;
            bool paused = false;
            auto sample = [&] {
                const Duration at = t;
                if (!paused) { paused = true; t += pause; }   // the deschedule, once, after the first sample
                return at;
            };
            RunningTime::Clocks clocks;
            clocks.running = sample;
            clocks.steady = [&] { return Steady::time_point(sample()); };
            const Duration d = RunningTime::fromSteady(deadline, clocks);
            const Duration requested(deadline.time_since_epoch());   // the identity timeline
            QVERIFY2(d <= requested, qPrintable(QStringLiteral("pause %1 s: converted %2 s, requested %3 s")
                                                    .arg(pause.count()).arg(d.count() / 1000000000).arg(requested.count() / 1000000000)));
            QCOMPARE(d, requested - pause);   // the whole pause comes off the budget, none is added
        }
    }

    // ---- IDeadlineWatchdog state rules (DeadlineState) --------------------------------------------
    void neverArmedNeverFires()
    {
        DeadlineState s;
        QVERIFY(!s.armed());
        QCOMPARE(s.deadline, RunningTime::kNever);
        QVERIFY(!s.expired(RunningTime::kNever - 1ns));
        QCOMPARE(s.exitCode(), kNeverArmedCode);
    }

    void extendIgnoredWhenNeverArmed()
    {
        DeadlineState s;
        s.extend(10s);
        QVERIFY(!s.armed());
        QCOMPARE(s.deadline, RunningTime::kNever);
    }

    void tightenActivatesANeverArmedWatchdog()
    {
        DeadlineState s;
        s.tighten(100s, 0);   // Exit::now on a kill deadline nobody armed yet
        QVERIFY(s.armed());
        QCOMPARE(s.deadline, Duration(100s));
        QCOMPARE(s.exitCode(), 0);
        QVERIFY(s.expired(100s));
        QVERIFY(!s.expired(99s));
    }

    void tightenOnlyEarlierAndTakesTheCode()
    {
        DeadlineState s;
        s.arm(100s, 0);
        s.tighten(150s, 6);   // later: ignored, deadline AND code unchanged
        QCOMPARE(s.deadline, Duration(100s));
        QCOMPARE(s.exitCode(), 0);
        s.tighten(50s, 4);    // earlier: takes both
        QCOMPARE(s.deadline, Duration(50s));
        QCOMPARE(s.exitCode(), 4);
    }

    void extendOnlyLaterAndKeepsTheCode()
    {
        DeadlineState s;
        s.arm(100s, 7);
        s.extend(90s);
        QCOMPARE(s.deadline, Duration(100s));
        s.extend(135s);
        QCOMPARE(s.deadline, Duration(135s));
        QCOMPARE(s.exitCode(), 7);
    }

    // A late arm (e.g. STOP arriving after Exit::now already tightened the kill deadline) can never
    // move an armed deadline later.
    void armOnAnArmedWatchdogOnlyTightens()
    {
        DeadlineState s;
        s.tighten(2s, 3);
        s.arm(20s, 0);
        QCOMPARE(s.deadline, Duration(2s));
        QCOMPARE(s.exitCode(), 3);
        s.arm(1s, 0);
        QCOMPARE(s.deadline, Duration(1s));
        QCOMPARE(s.exitCode(), 0);
    }

    void reportBudgetIsTwoSeconds() { QCOMPARE(Exit::kReportBudget, 2s); }
};

QTEST_GUILESS_MAIN(tst_RunningTime)
#include "tst_RunningTime.moc"
