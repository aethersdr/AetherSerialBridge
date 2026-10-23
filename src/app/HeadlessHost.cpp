#include "app/HeadlessHost.h"

#include "app/ScratchEngine.h"
#include "app/ServiceHost.h"

#ifdef _WIN32
#include <QSerialPortInfo>
#endif

#ifdef ASB_TEST_SEAMS
#include "app/GuardProbe.h"
#ifdef __linux__
#include "app/TestSeams_linux.h"

#include <cstdio>
#endif
#endif

namespace asb::app {

HeadlessHost::HeadlessHost(int argc, char** argv) : cli::SystemHost(argc, argv) {}

HeadlessHost::~HeadlessHost() = default;

int HeadlessHost::runScratch(const QString& configFile, const BridgeConfig& loopbackConfig, cli::Console& c)
{
    return asb::app::runScratch(*this, configFile, loopbackConfig, c);
}

#ifdef __linux__
int HeadlessHost::runService()
{
    return runLinuxService(m_argc, m_argv);
}
#endif

#ifdef _WIN32
int HeadlessHost::listPorts(cli::Console& c)
{
    // The ports Windows knows (SetupAPI / the registry); none is opened (INV-32; HW 10 under Process Monitor).
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    if (ports.isEmpty()) c.out(QStringLiteral("No serial ports found."));
    for (const QSerialPortInfo& p : ports) {
        const QString what = p.description().isEmpty() ? QString() : QStringLiteral("  %1").arg(p.description());
        c.out(QStringLiteral("  %1%2").arg(p.portName(), what));
    }
    return 0;
}
#endif

#ifdef ASB_TEST_SEAMS
void HeadlessHost::guardProbe()
{
    asb::app::guardProbe();
}

#ifdef __linux__
void HeadlessHost::resultHook(const Result& r, const QStringList& reports)
{
    if (qEnvironmentVariableIsEmpty("ASB_TEST_RESULT_JSON") || !linuxPlatform()) return;
    const QByteArray line = testseams::resultJson(r, reports, *linuxPlatform()) + '\n';
    std::fputs(line.constData(), stdout);
    std::fflush(stdout);
}

void HeadlessHost::customise(posix::LinuxPlatform::Options& o)
{
    static testseams::PausingSys sys;
    o.sys = &sys;
    o.service.onStep = [](const QString& step) { testseams::maybePause(QStringLiteral("step:") + step); };
    // ASB_TEST_PROGRAM_SHA256: the build this seam build counts as its own (v1.1 INV-120). The systemd suite's release
    // mode runs failure points and paused operations from the seam build against the installed RELEASE program, and
    // sets it to that program's digest so those operations see the installation as the release would.
    const QByteArray program = qgetenv("ASB_TEST_PROGRAM_SHA256");
    if (!program.isEmpty()) o.service.expectedSha256 = program.toLower();
}

IFirewall* HeadlessHost::wrapFirewall(IFirewall& real)
{
    m_firewall = std::make_unique<testseams::PausingFirewall>(real);
    return m_firewall.get();
}
#endif
#endif

}  // namespace asb::app
