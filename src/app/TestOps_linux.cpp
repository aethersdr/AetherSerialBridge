// Test builds only (ASB_TEST_SEAMS): `aether-serial-bridge --test-op <operation> [arguments]`, the systemd
// suite's way to run the real Applier on the real Linux Platform until WS-9's CLI exists (WS-8). It is the
// installed program itself (install copies /proc/self/exe), so what the suite installs and starts is the
// binary under test. Never in a release build (ASB_BUILD_TESTS=OFF).
//
// Like every Applier process, it installs the process guard first (INV-110), so each operation also shows
// that QtDBus and the worker threads work under the filter.
//
// Operations (one JSON line on stdout; exit 0 for Ok / SavedNotApplied, 1 otherwise):
//   install [--force-recreate] | apply [--config <file>] [--force-recreate] | start | stop | repair |
//   deactivate [--config <file>] | uninstall [--config <file>] | discard-pending
// Probes (no Applier): try-engine-lock | hold-engine-lock <ms> | hold-apply-lock <ms> | query | detect |
//   clocks (RunningTime::now beside CLOCK_MONOTONIC and CLOCK_BOOTTIME, in nanoseconds: N9-1 on Linux)
//
// Failure points (ASB_TEST_FAIL, comma separated): firewall:sync throws (an apply that fails after the barrier);
// firewall:removeAll throws (an untick/remove whose removal fails); sys:<call>:<name> fails that PosixSys call with
// EIO (sys:renameat:config.json: the rename after a successful sync fails).
//
// Pause points (ASB_TEST_PAUSE, comma separated), where the process prints "ASB-PAUSED <point>" on stderr and
// waits forever so the suite can kill it:
//   step:<name>                 SystemdService steps: after-mask, before-allow-starts, before-start
//   firewall:<method>           the (null) firewall's sync, sweep, ...
//   sys:<before|after>:<call>:<name>   a PosixSys call (openat, renameat, fsync, ...) whose name argument
//                               (for renameat: the new name) contains <name>

#include "app/ServiceHost.h"
#include "app/TestSeams_linux.h"
#include "cli/LinuxHost.h"
#include "cli/PolkitPolicy.h"

#include "core/BridgeRunner.h"
#include "core/Interfaces.h"
#include "core/OperationHost.h"
#include "core/RunningTime.h"
#include "platform/posix/LinuxLocks.h"
#include "platform/posix/LinuxPlatform.h"
#include "platform/posix/ProcessGuard.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QStringList>

#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <thread>

namespace asb::app {
namespace {

using namespace testseams;

class Printer final : public IOperationObserver {
public:
    explicit Printer(asb::posix::LinuxPlatform& p) : m_p(p) {}
    void interim(const Interim& i) override
    {
        std::fprintf(stderr, "interim: %s: %s\n", qPrintable(i.phase), qPrintable(i.message));
    }
    void finished(const Result& r) override
    {
        std::fputs(testseams::resultJson(r, m_p.service().takeChanges(), m_p).constData(), stdout);
        std::fputs("\n", stdout);
        std::fflush(stdout);
        code = (r.outcome == Outcome::Ok || r.outcome == Outcome::SavedNotApplied) ? 0 : 1;
        QCoreApplication::exit(code);
    }
    int code = 1;

private:
    asb::posix::LinuxPlatform& m_p;
};

int usage()
{
    std::fputs("usage: --test-op <operation> [arguments] (test builds only)\n", stderr);
    return 2;
}

std::optional<SaveRequest> requestFrom(const QStringList& args)
{
    const qsizetype i = args.indexOf(QStringLiteral("--config"));
    if (i < 0 || i + 1 >= args.size()) return std::nullopt;
    QFile f(args[i + 1]);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    SaveRequest r;
    r.config = f.readAll();
    r.source.kind = Source::New;
    r.supersedePending = true;
    return r;
}

const char* lockResultName(LockResult r)
{
    switch (r) {
    case LockResult::Acquired: return "Acquired";
    case LockResult::Busy: return "Busy";
    case LockResult::Missing: return "Missing";
    case LockResult::Insecure: return "Insecure";
    case LockResult::Unsupported: return "Unsupported";
    case LockResult::InsecureFile: return "InsecureFile";
    }
    return "?";
}

}  // namespace

int runTestOperation(int argc, char** argv)
{
    asb::posix::ProcessGuard::installOrExit();   // mode entry (INV-110): before any object
    if (argc < 3) return usage();
    QCoreApplication app(argc, argv);
    QStringList args;
    for (int i = 3; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    const QString op = QString::fromLocal8Bit(argv[2]);
    const auto ms = [&](int idx) { return std::chrono::milliseconds(args.value(idx).toLongLong()); };

    if (op == QLatin1String("clocks")) {
        // Running time sampled between the two kernel clocks: it must read CLOCK_MONOTONIC (suspend
        // excluded), never CLOCK_BOOTTIME (suspend included). The suite runs this in a time namespace whose
        // boottime offset stands in for a suspend that already happened.
        const auto ns = [](clockid_t c) {
            timespec ts{};
            clock_gettime(c, &ts);
            return qlonglong(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
        };
        const qlonglong monotonic = ns(CLOCK_MONOTONIC);
        const qlonglong running = qlonglong(RunningTime::now().count());
        const qlonglong boottime = ns(CLOCK_BOOTTIME);
        std::printf("running=%lld monotonic=%lld boottime=%lld\n", running, monotonic, boottime);
        return 0;
    }

    static PausingSys pausingSys;
    asb::posix::LinuxPlatform::Options o;
    o.paths = asb::posix::LinuxPaths::from(*serviceLocations().locations);
    o.sys = &pausingSys;
    o.service.onStep = [](const QString& step) { maybePause(QStringLiteral("step:") + step); };
    o.service.sourceBinary = QStringLiteral("/proc/self/exe");
    o.service.policy = QByteArray(asb::cli::kPolkitPolicy);   // as the CLI's install (WS-9b)
    // This build, as LinuxHost passes it (v1.1 INV-120), or the build the systemd suite names (ASB_TEST_PROGRAM_SHA256:
    // the installed release program, in release mode).
    const QByteArray program = qgetenv("ASB_TEST_PROGRAM_SHA256");
    o.service.expectedSha256 = program.isEmpty() ? asb::cli::ownImageSha256() : program.toLower();
    asb::posix::LinuxPlatform lp(o);

    if (op == QLatin1String("try-engine-lock")) {
        LockResult why = LockResult::Busy;
        auto h = lp.engineLock().tryAcquire(&why);
        std::printf("%s %s\n", lockResultName(why), qPrintable(lp.engineLock().holderDescription()));
        return h ? 0 : 1;
    }
    if (op == QLatin1String("hold-engine-lock")) {
        LockResult why = LockResult::Busy;
        auto h = lp.engineLock().tryAcquire(&why);
        std::printf("%s\n", lockResultName(why));
        std::fflush(stdout);
        if (!h) return 1;
        std::this_thread::sleep_for(ms(0));
        return 0;
    }
    if (op == QLatin1String("hold-apply-lock")) {
        auto h = lp.applyLock().acquire(std::chrono::seconds(120));
        if (!h) {
            std::printf("REFUSED %s\n", qPrintable(lp.applyLock().refusalReason()));
            return 1;
        }
        std::printf("HELD %lld\n", qlonglong(QDateTime::currentMSecsSinceEpoch()));
        std::fflush(stdout);
        std::this_thread::sleep_for(ms(0));
        std::printf("RELEASED %lld\n", qlonglong(QDateTime::currentMSecsSinceEpoch()));
        return 0;
    }
    if (op == QLatin1String("query")) {
        const ServiceQuery q = lp.service().query(systemClock().now() + std::chrono::seconds(15));
        std::printf("%d %lld\n", int(q.state), qlonglong(q.pid));
        return 0;
    }
    if (op == QLatin1String("detect")) {
        const Installation i = lp.service().detect(systemClock().now() + std::chrono::seconds(15));
        std::printf("%d %s\n", int(i.state), qPrintable(i.detail));
        return 0;
    }

    PausingFirewall firewall(lp.platform().firewall);
    Platform p{lp.platform().folder, lp.platform().service, firewall, lp.platform().applyLock,
               lp.platform().engineLock, lp.platform().token, lp.platform().status, lp.platform().io};
    OperationHost host(p);
    Printer printer(lp);
    const bool force = args.contains(QStringLiteral("--force-recreate"));
    bool ok = false;
    if (op == QLatin1String("install")) ok = host.submitRun(std::nullopt, true, force, printer);
    else if (op == QLatin1String("apply")) ok = host.submitRun(requestFrom(args), false, force, printer);
    else if (op == QLatin1String("start")) ok = host.submitStart(printer);
    else if (op == QLatin1String("stop")) ok = host.submitStop(printer);
    else if (op == QLatin1String("repair")) ok = host.submitRepair(printer);
    else if (op == QLatin1String("deactivate")) ok = host.submitDeactivate(requestFrom(args), printer);
    else if (op == QLatin1String("uninstall"))   // with --config: the untick with a save (SaveAndRemove); else WS-16's uninstall
        ok = args.contains(QStringLiteral("--config"))
                 ? host.submitSaveAndRemove(requestFrom(args), printer)
                 : host.submitUninstall(UninstallOptions{args.contains(QStringLiteral("--remove-settings"))}, printer);
    else if (op == QLatin1String("discard-pending")) ok = host.submitDiscardPending(printer);
    else return usage();
    if (!ok) return usage();
    app.exec();
    return printer.code;
}

}  // namespace asb::app
