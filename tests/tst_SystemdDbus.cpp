// Job tracking of the systemd adapter over the D-Bus seam (WS-8, D51, INV-115): receivers installed before
// every submission, early completions kept and matched by exact job path, owner changes and every result
// but "done" fail the step, every call bounded by the remaining phase budget, outstanding jobs reconciled.

#include "platform/posix/SystemdService.h"
#include "support/FakeSystemdBus.h"
#include "support/Fakes.h"

#include <QTest>

using namespace asb;
using namespace asb::posix;
using namespace asb::test;
using namespace std::chrono_literals;

namespace {

constexpr char kUnit[] = "aether-serial-bridge.service";

struct Fixture {
    FakeClock clock;
    FakeSystemdBus bus{clock};
    SystemdService::Options opts()
    {
        SystemdService::Options o;
        o.requireRoot = false;
        return o;
    }
    SystemdService service{bus, opts(), clock};
    Fixture() { bus.unitFileExists = [](const QString&) { return true; }; }
    Deadline in(std::chrono::milliseconds d) { return clock.now() + d; }
};

}  // namespace

class tst_SystemdDbus : public QObject {
    Q_OBJECT

private slots:
    // The job finishes before systemd's reply carrying its path arrives: the receiver must already be there.
    void earlyJobRemovedBeforeReplyHandled()
    {
        Fixture f;
        f.bus.plans << FakeSystemdBus::JobPlan{FakeSystemdBus::When::BeforeReply, QStringLiteral("done"), {}, false};
        try {
            f.service.start(f.in(15s));
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
        // The StartUnit call was sent with the receivers already connected.
        bool seen = false;
        for (const FakeSystemdBus::Call& c : f.bus.calls) {
            if (c.method != QLatin1String("StartUnit")) continue;
            seen = true;
            QVERIFY(c.receiversInstalled);
        }
        QVERIFY(seen);
    }
    void lateJobRemovedAlsoHandled()
    {
        Fixture f;
        f.bus.plans << FakeSystemdBus::JobPlan{};   // after the reply
        f.service.start(f.in(15s));
    }

    // Two completions for the SAME unit: only the one whose path the reply named decides.
    void exactJobPathCorrelation_data()
    {
        QTest::addColumn<QString>("other");
        QTest::addColumn<QString>("ours");
        QTest::addColumn<bool>("ok");
        QTest::newRow("other done, ours failed") << QStringLiteral("done") << QStringLiteral("failed") << false;
        QTest::newRow("other failed, ours done") << QStringLiteral("failed") << QStringLiteral("done") << true;
        QTest::newRow("other done, ours never") << QStringLiteral("done") << QStringLiteral("never") << false;
    }
    void exactJobPathCorrelation()
    {
        QFETCH(QString, other);
        QFETCH(QString, ours);
        QFETCH(bool, ok);
        Fixture f;
        FakeSystemdBus::JobPlan p;
        p.when = ours == QLatin1String("never") ? FakeSystemdBus::When::Never : FakeSystemdBus::When::AfterReply;
        p.result = ours == QLatin1String("never") ? QStringLiteral("done") : ours;
        p.others << qMakePair(QStringLiteral("/org/freedesktop/systemd1/job/41"), other);
        f.bus.plans << p;
        bool threw = false;
        try {
            f.service.start(f.in(15s));
        } catch (const std::exception& e) {
            threw = true;
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(ours == QLatin1String("never") ? QStringLiteral("deadline") : ours), e.what());
        }
        QCOMPARE(!threw, ok);
    }

    void ownerChangeFailsStep()
    {
        Fixture f;
        FakeSystemdBus::JobPlan p;
        p.ownerChange = true;
        f.bus.plans << p;
        try {
            f.service.start(f.in(15s));
            QFAIL("a manager owner change must fail the step");
        } catch (const std::exception& e) {
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(QLatin1String("owner")), e.what());
        }
    }

    // The completion and the owner change arrive in the same pump. The step still fails,
    // whichever the adapter would have looked at first.
    void ownerChangeWithCompletionInOnePumpFailsStep()
    {
        Fixture f;
        FakeSystemdBus::JobPlan p;
        p.ownerChangeWithCompletion = true;
        f.bus.plans << p;
        try {
            f.service.start(f.in(15s));
            QFAIL("a manager owner change seen with the completion must fail the step");
        } catch (const std::exception& e) {
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(QLatin1String("owner")), e.what());
        }
    }
    // The same for reconciliation: the pump that ends the last outstanding job also brings an owner change, and
    // the next ListJobs is empty; that empty list is not a success.
    void ownerChangeThenEmptyJobListFailsReconciliation()
    {
        Fixture f;
        f.bus.runningPumps = 1;
        f.bus.ownerChangeWhenJobsFinish = true;
        f.bus.jobs << SystemdJob{8, QString::fromLatin1(kUnit), QStringLiteral("stop"), QStringLiteral("running"),
                                 QStringLiteral("/org/freedesktop/systemd1/job/8"), {}};
        try {
            f.service.reconcileOutstandingJobs(f.in(15s));
            QFAIL("a manager owner change during reconciliation must fail it");
        } catch (const std::exception& e) {
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(QLatin1String("owner")), e.what());
        }
        QVERIFY(f.bus.jobs.isEmpty());   // the job did end: only the owner change fails it
    }

    void failedResultsAreStepFailures_data()
    {
        QTest::addColumn<QString>("result");
        for (const char* r : {"canceled", "failed", "timeout", "dependency", "skipped", "invalid"}) QTest::newRow(r) << QString::fromLatin1(r);
    }
    void failedResultsAreStepFailures()
    {
        QFETCH(QString, result);
        Fixture f;
        f.bus.plans << FakeSystemdBus::JobPlan{FakeSystemdBus::When::AfterReply, result, {}, false};
        QVERIFY_THROWS_EXCEPTION(std::exception, f.service.start(f.in(15s)));
    }

    // Every call's timeout is what is left of the phase; a job that never completes fails at the deadline,
    // and nothing is sent once the deadline has passed.
    void callTimeoutsHonoured()
    {
        Fixture f;
        f.bus.onCall = [&](const QString&) { f.clock.advance(1s); };   // each call takes a second
        f.bus.plans << FakeSystemdBus::JobPlan{FakeSystemdBus::When::Never, QStringLiteral("done"), {}, false};
        const Deadline d = f.in(15s);
        const Deadline t0 = f.clock.now();
        try {
            f.service.start(d);
            QFAIL("a job that never completes must fail the step");
        } catch (const std::exception& e) {
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(QLatin1String("deadline")), e.what());
        }
        QVERIFY(f.clock.now() >= d);
        QVERIFY(f.clock.now() <= d + 1s);   // the wait never overshoots by more than one call
        // Each call's timeout was the remaining budget when it was sent.
        std::chrono::milliseconds elapsed{0};
        for (const FakeSystemdBus::Call& c : f.bus.calls) {
            QCOMPARE(c.timeout, std::chrono::duration_cast<std::chrono::milliseconds>(15s) - elapsed);
            elapsed += 1s;
        }
        for (std::chrono::milliseconds p : f.bus.pumpTimeouts) QVERIFY(p <= 200ms);
        Q_UNUSED(t0);
        // After the deadline: nothing is sent.
        const qsizetype before = f.bus.calls.size();
        QVERIFY_THROWS_EXCEPTION(PlatformError, f.service.inhibitStarts(d));
        QVERIFY_THROWS_EXCEPTION(PlatformError, f.service.requestStop(d));
        QCOMPARE(f.bus.calls.size(), before);
    }
    void subscribeOnlyOnce()
    {
        Fixture f;
        f.service.start(f.in(15s));
        f.bus.units[QString::fromLatin1(kUnit)].active = QStringLiteral("inactive");
        f.service.start(f.in(15s));
        QCOMPARE(f.bus.methods().count(QStringLiteral("Subscribe")), 1);
    }

    // Before R9 and before uninstall: a job still waiting is cancelled, a running one is awaited.
    void outstandingJobsReconciled()
    {
        Fixture f;
        f.bus.jobs << SystemdJob{7, QString::fromLatin1(kUnit), QStringLiteral("start"), QStringLiteral("waiting"),
                                 QStringLiteral("/org/freedesktop/systemd1/job/7"), {}}
                   << SystemdJob{8, QString::fromLatin1(kUnit), QStringLiteral("stop"), QStringLiteral("running"),
                                 QStringLiteral("/org/freedesktop/systemd1/job/8"), {}}
                   << SystemdJob{9, QStringLiteral("other.service"), QStringLiteral("start"), QStringLiteral("waiting"),
                                 QStringLiteral("/org/freedesktop/systemd1/job/9"), {}};
        f.service.reconcileOutstandingJobs(f.in(15s));
        QStringList cancelled;
        for (const FakeSystemdBus::Call& c : f.bus.calls)
            if (c.method == QLatin1String("CancelJob")) cancelled << c.args;
        QCOMPARE(cancelled, QStringList{QStringLiteral("7")});   // ours, waiting; never another unit's job
        QCOMPARE(f.bus.jobs.size(), 1);                           // only the other unit's job is left
    }
    void outstandingJobThatNeverEndsFailsAtTheDeadline()
    {
        Fixture f;
        f.bus.runningPumps = 1'000'000;
        f.bus.jobs << SystemdJob{8, QString::fromLatin1(kUnit), QStringLiteral("stop"), QStringLiteral("running"),
                                 QStringLiteral("/org/freedesktop/systemd1/job/8"), {}};
        const Deadline d = f.in(5s);
        QVERIFY_THROWS_EXCEPTION(PlatformError, f.service.reconcileOutstandingJobs(d));
        QVERIFY(f.clock.now() >= d);
    }
};

QTEST_GUILESS_MAIN(tst_SystemdDbus)
#include "tst_SystemdDbus.moc"
