// The exit primitives (WS-4 implementation of the WS-3 contract; D28, D39, D59, S12): faultDie and
// Exit::now, run in child processes because they end the process.

#include "core/DeadlineWatchdog.h"
#include "core/Exit.h"
#include "support/EngineTestSupport.h"

#include <QTest>

#include <cstdio>
#include <thread>

using namespace asb;
using namespace asb::test;
using namespace std::chrono_literals;

namespace {

// Records the order of calls in a fixed buffer (no allocation in the probe's exit path).
char g_order[64];
int g_orderLen = 0;
void note(char c)
{
    if (g_orderLen < 63) g_order[g_orderLen++] = c;
}

// A watchdog that only records: 't' for tighten. Never fires.
class OrderWatchdog final : public IDeadlineWatchdog {
public:
    void arm(std::chrono::steady_clock::time_point, int) override { note('a'); }
    void extend(std::chrono::steady_clock::time_point) override { note('e'); }
    void tighten(std::chrono::steady_clock::time_point d, int c) override
    {
        note('t');
        deadline = d;
        code = c;
    }
    void expireNow() override { note('x'); }
    std::chrono::steady_clock::time_point deadline;
    int code = -1;
};
OrderWatchdog* g_order_wd = nullptr;

void reportPrints(int code)
{
    note('r');
    std::printf("report %d order %.*s tightened-to %d within %lld ms\n", code, g_orderLen, g_order, g_order_wd ? g_order_wd->code : -1,
                g_order_wd ? (long long)std::chrono::duration_cast<std::chrono::milliseconds>(g_order_wd->deadline - std::chrono::steady_clock::now()).count()
                           : -1LL);
    std::fflush(stdout);
}

void reportBlocks(int code)
{
    std::printf("report %d blocking\n", code);
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(1s);
}

int runScenario(const QString& s)
{
    if (s == QLatin1String("faultDie")) faultDie(5);
    if (s == QLatin1String("order")) {
        OrderWatchdog w;
        g_order_wd = &w;
        Exit::now(w, 3, &reportPrints);
    }
    if (s == QLatin1String("noReport")) {
        DeadlineWatchdog w;
        Exit::now(w, 2, nullptr);
    }
    if (s == QLatin1String("blockedReport")) {
        DeadlineWatchdog w;   // the kill deadline, created at "process start", never armed before
        Exit::now(w, 4, &reportBlocks);
    }
    return 100;
}

}  // namespace

class tst_Exit : public QObject {
    Q_OBJECT

private slots:
    void faultDieExitsWithTheCode()
    {
        const ProbeResult r = runProbe(QStringLiteral("faultDie"));
        QVERIFY(r.finished);
        QCOMPARE(r.exitCode, 5);
        QVERIFY(r.out.isEmpty());
    }

    // D59: the kill deadline is tightened to now + 2 s BEFORE the report runs, then the process ends
    // with the code right after the report.
    void killDeadlineTightenedBeforeTheReport()
    {
        const ProbeResult r = runProbe(QStringLiteral("order"));
        QVERIFY(r.finished);
        QCOMPARE(r.exitCode, 3);
        QVERIFY2(r.out.startsWith("report 3 order tr tightened-to 3 within "), r.out.constData());
        const qint64 within = r.out.trimmed().split(' ').at(7).toLongLong();
        QVERIFY2(within > 1000 && within <= 2000, r.out.constData());
    }

    void exitWithoutAReport()
    {
        const ProbeResult r = runProbe(QStringLiteral("noReport"));
        QVERIFY(r.finished);
        QCOMPARE(r.exitCode, 2);
    }

    // A report that blocks (SetServiceStatus stuck in RPC) cannot keep the process alive: the kill
    // deadline ends it with the same code about 2 s later.
    void blockedReportEndsByTheKillDeadline()
    {
        const ProbeResult r = runProbe(QStringLiteral("blockedReport"));
        QVERIFY(r.finished);
        QCOMPARE(r.exitCode, 4);
        QVERIFY(r.out.startsWith("report 4 blocking"));
        QVERIFY2(r.ms >= 1800 && r.ms < 6000, qPrintable(QString::number(r.ms)));
    }
};

int main(int argc, char** argv)
{
    const QString probe = qEnvironmentVariable("ASB_PROBE");
    if (!probe.isEmpty()) return runScenario(probe);
    QCoreApplication app(argc, argv);
    tst_Exit t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_Exit.moc"
