// asb_tui_testhost: the TUI (tui::run, the same entry the executables call) on the fake service host, for the pty
// process tests (tst_TuiProcess; master §8.4). Test code only; never installed.
//
// Environment:
//   ASB_TEST_SERVICE_HOST   the scenario (support/TuiScenario.h); "installed" when unset
//   ASB_TEST_STATUS_FILE    status.json the status poll reads (the test rewrites it); otherwise the fake's own
//   ASB_TEST_RELEASE        a file whose existence releases a blocked call (block=...)
//   ASB_TEST_EVENT_LOG      where the fake world's call log is written when the TUI ends, one call per line, then
//                           "file <name> <sha256>" for every file left in the data folder
//   ASB_TEST_SERIAL_BYID, ASB_TEST_SERIAL_DEV   the directories the port list reads (Linux; INV-32)

#include "support/TuiScenario.h"

#include "core/ConfigJson.h"
#include "core/StatusStore.h"
#include "tui/Tui.h"

#ifdef __linux__
#include "platform/posix/SerialPortList.h"

#include <pthread.h>
#include <signal.h>
#endif

#include <QFile>
#include <QTextStream>

#include <cstdio>

namespace {

using namespace asb;
using namespace asb::test;

class FileStatus final : public IStatusReader {
public:
    explicit FileStatus(QString path) : m_path(std::move(path)) {}
    std::optional<StatusReport> read() override
    {
        QFile f(m_path);
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        return StatusStore::fromJson(f.readAll());
    }

private:
    QString m_path;
};

class TestHost final : public FakeCliHost {
public:
    explicit TestHost(Rig& rig) : FakeCliHost(rig)
    {
        const QString statusFile = qEnvironmentVariable("ASB_TEST_STATUS_FILE");
        IStatusReader* status = &rig.status;
        if (!statusFile.isEmpty()) {
            m_fileStatus = std::make_unique<FileStatus>(statusFile);
            status = m_fileStatus.get();
        }
        m_platform = std::make_unique<Platform>(Platform{rig.folder, rig.service, rig.firewall, rig.world.applyLock,
                                                         rig.engineLock, *rig.token, *status, rig.io});
    }
    Platform* platform(QString*, int*) override
    {
        calls << QStringLiteral("platform");
        return m_platform.get();
    }
    void blockStopSignals() override
    {
        calls << QStringLiteral("signals");
#ifdef __linux__
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGTERM);
        sigaddset(&s, SIGINT);
        sigaddset(&s, SIGHUP);
        pthread_sigmask(SIG_BLOCK, &s, nullptr);
#endif
    }
    int listPorts(cli::Console& c) override
    {
        calls << QStringLiteral("listPorts");
#ifdef __linux__
        QString text;
        {
            QTextStream out(&text);
            posix::SerialPortList::print(out, qEnvironmentVariable("ASB_TEST_SERIAL_BYID"), qEnvironmentVariable("ASB_TEST_SERIAL_DEV"));
        }
        for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) c.out(line);
        return 0;
#else
        return FakeCliHost::listPorts(c);
#endif
    }

private:
    std::unique_ptr<FileStatus> m_fileStatus;
    std::unique_ptr<Platform> m_platform;
};

}  // namespace

int main()
{
    Rig rig;
    TestHost host(rig);
    TuiScenario scenario(rig, host);
    scenario.releaseFile = qEnvironmentVariable("ASB_TEST_RELEASE");
    const QString words = qEnvironmentVariable("ASB_TEST_SERVICE_HOST");
    scenario.apply(words.isEmpty() ? QStringLiteral("installed") : words);

    tui::Options opts;
    opts.notATerminal = [] {
        std::fputs("asb_tui_testhost: not a terminal\n", stderr);
        return 2;
    };
    const int code = tui::run(host, opts);

    const QString logFile = qEnvironmentVariable("ASB_TEST_EVENT_LOG");
    if (!logFile.isEmpty()) {
        QFile f(logFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream out(&f);
            for (const QString& e : rig.world.log.all()) out << e << '\n';
            for (const QString& name : {QStringLiteral("config.json"), QStringLiteral("config.pending.json"), QStringLiteral("start.token")})
                if (rig.has(name)) out << "file " << name << ' ' << sha256Hex(rig.file(name)) << '\n';
        }
    }
    return code;
}
