// `--service` on Windows (WS-7). See ServiceMain.h for the order and the rules; ServiceLifecycle.h for the
// stop/exit decisions (portable, tested on every OS).

#include "platform/win/ServiceMain.h"

#include "core/AtomicJson.h"
#include "core/BridgeRunner.h"
#include "core/BuildInfo.h"
#include "core/DeadlineWatchdog.h"
#include "core/Exit.h"
#include "core/LogSinks.h"
#include "core/SideChannel.h"
#include "core/StartToken.h"
#include "core/StatusStore.h"
#include "engine/BridgeHost.h"
#include "engine/QtSerialDevice.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/EngineLock.h"
#include "platform/win/InstallBinary.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/ProcessPolicy.h"
#include "platform/win/ServiceExit.h"
#include "platform/win/ServiceLifecycle.h"
#include "platform/win/ServiceStatus.h"
#include "platform/win/WbemSession.h"
#include "platform/win/WinFileIo.h"
#include "platform/win/WinLogFile.h"
#include "platform/win/WindowsDataFolder.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QTimer>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <thread>

#ifdef ASB_TEST_SEAMS
#include <QFile>
#include <QHash>
#include <deque>
#include <stdexcept>
#endif

namespace asb::win {

namespace {

// ---------------------------------------------------------------------------------------------------
// SCM connection
// ---------------------------------------------------------------------------------------------------
std::atomic<SERVICE_STATUS_HANDLE> g_handle{nullptr};
HANDLE g_connected = nullptr;
std::atomic<ServiceLifecycle*> g_life{nullptr};
std::atomic<ISideChannel*> g_side{nullptr};

class NativeReporter final : public IServiceStatusReporter {
public:
    bool report(const NativeServiceStatus& s) override
    {
        SERVICE_STATUS st{};
        st.dwServiceType = s.serviceType;
        st.dwCurrentState = s.currentState;
        st.dwControlsAccepted = s.controlsAccepted;
        st.dwWin32ExitCode = s.win32ExitCode;
        st.dwServiceSpecificExitCode = s.serviceSpecificExitCode;
        st.dwCheckPoint = s.checkPoint;
        st.dwWaitHint = s.waitHint;
        return SetServiceStatus(g_handle.load(), &st) != FALSE;
    }
};

DWORD WINAPI controlHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    ServiceLifecycle* life = g_life.load();
    if (!life) return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
    return life->control(control);
}

void WINAPI serviceMainEntry(DWORD, LPWSTR*)
{
    g_handle = RegisterServiceCtrlHandlerExW(L"AetherSerialBridge", controlHandler, nullptr);
    SetEvent(g_connected);
    // The process ends through Exit::now / faultDie; this thread only keeps the dispatcher's ServiceMain alive.
    for (;;) Sleep(INFINITE);
}

// ---------------------------------------------------------------------------------------------------
// Faults (S4, INV-82): faultDie(1), nothing else.
// ---------------------------------------------------------------------------------------------------
LONG WINAPI unhandledException(EXCEPTION_POINTERS*) { faultDie(1); }
void onTerminate() { faultDie(1); }
void qtMessage(QtMsgType type, const QMessageLogContext&, const QString& text)
{
    if (type == QtFatalMsg) faultDie(1);
    if (ISideChannel* side = g_side.load()) side->log(type, text);
}

// An exception that escapes any event handler or slot ends the process (the `guarded()` rule for every slot).
class ServiceApp final : public QCoreApplication {
public:
    using QCoreApplication::QCoreApplication;
    bool notify(QObject* receiver, QEvent* event) override
    {
        try {
            return QCoreApplication::notify(receiver, event);
        } catch (...) {
            faultDie(1);
        }
    }
};

// ---------------------------------------------------------------------------------------------------
// The log file and the status writer, opened lazily (INV-22: nothing in the folder before trust passed).
// ---------------------------------------------------------------------------------------------------
class LazyWinLogOps final : public LogFileOps {
public:
    LazyWinLogOps(NtSeams& s, const IDataFolder& folder) : m_s(s), m_folder(folder) {}
    qint64 size() override { return file().size(); }
    bool rollover(QString* err) override { return file().rollover(err); }
    bool append(const QByteArray& bytes, QString* err) override { return file().append(bytes, err); }

private:
    WinLogFile& file()
    {
        if (!m_file) m_file = std::make_unique<WinLogFile>(m_s, m_folder.handle());
        return *m_file;
    }
    NtSeams& m_s;
    const IDataFolder& m_folder;
    std::unique_ptr<WinLogFile> m_file;
};

// The orphaned status temps a killed engine left (WS-5 round 1: P5 leaves them to the service). Runtime-owned
// files only, removed by handle; a failure only leaves a temp behind.
void sweepStatusTemps(NtSeams& s, FileIo& io, const DirHandle& dir)
{
    QStringList names;
    if (!s.list(WinFileIo::nativeOf(dir), &names).ok()) return;
    const QString prefix = QString::fromLatin1(StatusStore::kFileName) + QLatin1Char('.');
    for (const QString& n : names) {
        if (!n.startsWith(prefix) || !isTempName(n)) continue;
        QString err;
        io.remove(dir, n, &err);
    }
}

// v1.1 station fix: the one line of an exit that the side channel may not carry out in time — exit 7
// (BridgeRunner::LastWords: an error, event 7) and a start that is not activated (ServiceLifecycle's `say`: a warning,
// event 6). bridge.log first, through a handle of its own on the trusted data folder (never the side channel: it can
// be blocked, D56; the native rows block it), else the Windows Application event log (source AetherSerialBridge).
// Best effort; the caller's deadline (exit 7: the check's start + 5 s; not activated: the kill deadline, now + 5 s)
// ends the process whatever this does, so a write that blocks cannot delay the exit past it.
void sayExitReason(NtSeams& nt, const IDataFolder& folder, const QString& text, QtMsgType level, WORD eventType,
                   DWORD eventId)
{
    QString err;
    bool written = false;
    try {
        WinLogFile log(nt, folder.handle());
        written = log.append(FileLogSink::formatLine(level, QDateTime::currentDateTime(), text), &err);
    } catch (...) {
        err = QStringLiteral("an exception");
    }
    if (written) return;
    HANDLE source = RegisterEventSourceW(nullptr, L"AetherSerialBridge");
    if (!source) return;
    const QString full = err.isEmpty() ? text : QStringLiteral("%1 (bridge.log could not be written: %2)").arg(text, err);
    const std::wstring w = full.toStdWString();
    LPCWSTR strings[] = {w.c_str()};
    ReportEventW(source, eventType, 0, eventId, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}

// ServiceLifecycle's `say` (the not-activated line): set once the trusted folder's objects exist (step 6). The
// lifecycle only says it after the runner's trust check passed (lock -> trust -> token), on the main thread.
std::function<void(const QString&)> g_sayNotActivated;

QString joinPath(const QString& dir, const char* name)
{
    QString d = dir;
    if (!d.endsWith(QLatin1Char('\\'))) d += QLatin1Char('\\');
    return d + QString::fromLatin1(name);
}

#ifdef ASB_TEST_SEAMS
// ---------------------------------------------------------------------------------------------------
// Test seams (test builds only; the release audit asserts they are absent). Configured through the service's own
// environment (the registry value HKLM\SYSTEM\CurrentControlSet\Services\AetherSerialBridge\Environment), which
// only an administrator can set. A "block" waits forever on a named event nobody signals unless the test does:
// the kill deadline, a watchdog or the test ends the process.
// ---------------------------------------------------------------------------------------------------
QString env(const char* name) { return qEnvironmentVariable(name); }
int envInt(const char* name) { return qEnvironmentVariableIntValue(name); }

void blockOn(const QString& suffix)
{
    const QString name = env("ASB_TEST_EVENT_PREFIX") + QLatin1Char('-') + suffix;
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
    WaitForSingleObject(e ? e : GetCurrentProcess(), INFINITE);
}

void appendLine(const QString& path, const QByteArray& line)
{
    if (path.isEmpty()) return;
    HANDLE f = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, line.constData(), DWORD(line.size()), &n, nullptr);
    CloseHandle(f);
}

// ServiceStatusReporter seam: records every report (before making it) and blocks the chosen ones forever.
class TestReporter final : public IServiceStatusReporter {
public:
    explicit TestReporter(IServiceStatusReporter& real) : m_real(real), m_mode(env("ASB_TEST_BLOCK_REPORT")), m_log(env("ASB_TEST_REPORT_LOG")) {}
    bool report(const NativeServiceStatus& s) override
    {
        appendLine(m_log, QByteArray::number(s.currentState) + ' ' + QByteArray::number(s.checkPoint) + ' ' +
                              QByteArray::number(s.win32ExitCode) + ' ' + QByteArray::number(s.serviceSpecificExitCode) + ' ' +
                              QByteArray::number(qint64(GetTickCount64())) + '\n');
        const bool afterRunning = m_runningSeen.load();
        if (s.currentState == svc::kRunning) m_runningSeen = true;
        if ((m_mode == QLatin1String("after-running") && afterRunning) ||
            (m_mode == QLatin1String("stop-pending") && s.currentState == svc::kStopPending) ||
            (m_mode == QLatin1String("final") && s.currentState == svc::kStopped))
            blockOn(QStringLiteral("report"));
        return m_real.report(s);
    }

private:
    IServiceStatusReporter& m_real;
    QString m_mode, m_log;
    std::atomic<bool> m_runningSeen{false};
};

// A log sink that blocks forever on its first line written 2 s or more after Running (the side channel then
// stays blocked: INV-6/68/93/119 rows); earlier lines are dropped, so the first status (the ACK) gets out.
std::atomic<qint64> g_runningAtMs{0};
class BlockedSink final : public LogSink {
public:
    void write(QtMsgType, const QDateTime&, const QString&) override
    {
        const qint64 at = g_runningAtMs.load();
        if (at != 0 && qint64(GetTickCount64()) - at >= 2000) blockOn(QStringLiteral("sink"));
    }
};

// A loopback "serial device": what the client sends comes back. `blockClose` makes close() hang (a driver close
// that never returns: INV-81 stop_with_blocked_device_close).
class EchoDevice final : public engine::ISerialDevice {
public:
    explicit EchoDevice(bool blockClose) : m_blockClose(blockClose) {}
    bool open(const PortMapping&, QString*) override
    {
        m_open = true;
        return true;
    }
    void close() override
    {
        if (m_open && m_blockClose) blockOn(QStringLiteral("device-close"));
        m_open = false;
        m_buffer.clear();
    }
    qint64 read(char* d, qint64 max) override
    {
        if (!m_open) return -1;
        const qint64 n = std::min<qint64>(max, m_buffer.size());
        memcpy(d, m_buffer.constData(), size_t(n));
        m_buffer.remove(0, n);
        return n;
    }
    qint64 write(const char* d, qint64 n) override
    {
        if (!m_open) return -1;
        m_buffer.append(d, n);
        QTimer::singleShot(0, this, [this, n] {
            emit bytesWritten(n);
            emit readyRead();
        });
        return n;
    }
    qint64 bytesAvailable() const override { return m_buffer.size(); }
    qint64 bytesToWrite() const override { return 0; }
    std::optional<qint64> nativeOutputPending() const override { return 0; }
    void setReadBufferSize(qint64) override {}

private:
    bool m_blockClose;
    bool m_open = false;
    QByteArray m_buffer;
};

// The BlockingWbem seams at the session level: a decorator around the real WbemSession that blocks or delays a
// chosen call on an event the test controls (ConnectServer; the group query; every association query), then
// forwards. Used by the INV-119 schedules.
class BlockingCim final : public ICimSession {
public:
    explicit BlockingCim(ICimSession& real) : m_real(real), m_mode(env("ASB_TEST_CIM_BLOCK")) {}
    void connect() override
    {
        if (m_mode == QLatin1String("connect")) blockOn(QStringLiteral("cim"));
        m_real.connect();
    }
    QList<cim::Instance> query(const QString& wql, cim::Store s) override
    {
        const int checks = ++m_queries;
        if (m_mode == QLatin1String("group-query") && wql.startsWith(QLatin1String("SELECT")) && checks > 1)
            blockOn(QStringLiteral("cim"));   // the first check (startup) passes; a later one hangs
        if (m_mode.startsWith(QLatin1String("assoc-delay:")) && wql.startsWith(QLatin1String("ASSOCIATORS")))
            Sleep(DWORD(m_mode.mid(12).toInt()));
        return m_real.query(wql, s);
    }
    void put(const QString& c, const QString& p, const cim::Props& v, cim::PutFlag f, cim::Store s,
             const std::function<void()>& beforeWrite = {}) override
    {
        m_real.put(c, p, v, f, s, beforeWrite);
    }
    void remove(const QString& p, cim::Store s) override { m_real.remove(p, s); }

private:
    ICimSession& m_real;
    QString m_mode;
    std::atomic<int> m_queries{0};
};

void armTestFault(ServiceStatus& status)
{
    const QString fault = env("ASB_TEST_FAULT");
    if (fault.isEmpty()) return;
    QTimer::singleShot(envInt("ASB_TEST_FAULT_AFTER_MS"), qApp, [fault, &status] {
        if (fault == QLatin1String("slot-exception")) throw std::runtime_error("test seam: exception from a slot");
        if (fault == QLatin1String("qfatal")) qFatal("test seam: qFatal");
        if (fault == QLatin1String("terminate")) std::terminate();
        if (fault == QLatin1String("slot-exception-status-mutex")) {
            // A thread holds the status mutex forever; the fault must still end the process (INV-82).
            std::thread([&status] {
                status.mutexForTest().lock();
                for (;;) Sleep(INFINITE);
            }).detach();
            Sleep(200);
            throw std::runtime_error("test seam: exception while the status mutex is held");
        }
    });
}
#endif  // ASB_TEST_SEAMS

}  // namespace

[[noreturn]] void runService(int argc, char** argv)
{
    // 1. Both deadline watchdogs exist before anything else (D59): never destroyed.
    static DeadlineWatchdog* const firewallWatchdog = new DeadlineWatchdog();
    static DeadlineWatchdog* const killDeadline = new DeadlineWatchdog();

    // 2. Faults terminate (S4).
    std::set_terminate(onTerminate);
    SetUnhandledExceptionFilter(unhandledException);
    qInstallMessageHandler(qtMessage);

    // 3. Connect to SCM on a dispatcher thread of its own.
    g_connected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread([] {
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(L"AetherSerialBridge"), serviceMainEntry}, {nullptr, nullptr}};
        if (!StartServiceCtrlDispatcherW(table)) SetEvent(g_connected);
    }).detach();
    WaitForSingleObject(g_connected, 30000);
    if (!g_handle.load()) {
        std::fputs("aether-serial-bridge: --service runs only when the Windows service manager starts it\n", stderr);
        Exit::now(*killDeadline, 2, nullptr);   // not started by SCM: nothing to report to
    }
    static NativeReporter nativeReporter;
    IServiceStatusReporter* reporter = &nativeReporter;
#ifdef ASB_TEST_SEAMS
    static TestReporter testReporter(nativeReporter);
    reporter = &testReporter;
#endif
    ServiceExit::setFinalReporter(reporter);
    static ServiceStatus status(*reporter);
    status.startPending();

    auto* app = new ServiceApp(argc, argv);
    applyQtLibraryPolicy(PluginPolicy::NoPlugins);

    static BridgeRunner* runner = nullptr;
    static SideChannel* side = nullptr;
    ServiceLifecycle::Hooks hooks;
    hooks.postToMain = [app](std::function<void()> f) { QMetaObject::invokeMethod(app, std::move(f), Qt::QueuedConnection); };
    hooks.runnerStop = [](std::function<void()> done) {
        if (runner) runner->stop(std::move(done));
        else done();
    };
    hooks.shutdownSideChannel = [](std::chrono::milliseconds budget) { return side ? side->shutdown(budget) : true; };
#ifdef ASB_TEST_SEAMS
    if (const int ms = envInt("ASB_TEST_PAUSE_BEFORE_EXIT_MS"); ms > 0) hooks.beforeExit = [ms] { Sleep(DWORD(ms)); };
#endif
    hooks.say = [](const QString& text) {
        if (g_sayNotActivated) g_sayNotActivated(text);
    };
    static ServiceLifecycle life(status, *killDeadline, systemClock(), std::move(hooks), &Exit::now);
    g_life = &life;

    // 4. Where everything is (no fallback folder: WS-3 hand-off).
    ServiceLocations locations = serviceLocations();
    QString why;
    std::optional<WindowsPaths> paths;
#ifdef ASB_TEST_SEAMS
    // The two known-folder lookups failing (WS-6 deferred row, run here): no guessed folder, ever.
    const QString failFolder = env("ASB_TEST_FAIL_KNOWN_FOLDER");
    if (failFolder == QLatin1String("ProgramData")) locations = serviceLocations(true, [] { return QString(); });
    if (failFolder == QLatin1String("ProgramFiles")) {
        if (!locations.locations) life.refuse(exitCode(locations.failure));
        paths = windowsPathsFrom(locations, programFilesLockDirectoryFrom(QString()), &why);
        if (!paths) life.refuse(4);
    }
#endif
    if (!locations.locations) life.refuse(exitCode(locations.failure));
    if (!paths) paths = windowsPaths(&why);
    if (!paths) life.refuse(4);   // the lock directory (Program Files) cannot be determined

    // 5. No child process from here on (S17, INV-112); CIM keeps working under the guard (cim_works_under_job).
    if (!installProcessGuard(&why)) life.refuse(5);
    initialiseComSecurity(nullptr);   // a failure fails the firewall checks closed
    status.startPending();

    // 6. The objects; none of them touches the data folder before the runner's trust check.
    NtSeams& nt = nativeSeams();
    static WindowsDataFolder folder(nt, paths->dataFolder);
    static auto io = std::make_shared<WinFileIo>(nt, CreateSecurity::dataFolder());
    static EngineLock engineLock(nt, paths->lockDirectory, LockRole::Service);
    static FileStartToken token(folder, *io);
    std::unique_ptr<LogSink> sink = std::make_unique<FileLogSink>(std::make_unique<LazyWinLogOps>(nt, folder));
#ifdef ASB_TEST_SEAMS
    if (envInt("ASB_TEST_BLOCK_SINK") == 1) sink = std::make_unique<BlockedSink>();
#endif
    auto swept = std::make_shared<std::atomic<bool>>(false);
    NtSeams* ntp = &nt;
    StatusWriter writer = [ntp, swept](const StatusReport& r, QString* err) {
        const DirHandle& dir = folder.handle();
        if (!swept->exchange(true)) sweepStatusTemps(*ntp, *io, dir);
        const WriteResult w = StatusStore::write(*io, dir, r);
        if (!w.replaced && err) *err = w.error;
        return w.replaced;
    };
    side = new SideChannel(std::move(sink), std::move(writer));
    g_side = side;

    engine::SerialDeviceFactory devices = [] { return engine::makeQtSerialDevice(); };
#ifdef ASB_TEST_SEAMS
    if (const QString d = env("ASB_TEST_DEVICE"); !d.isEmpty()) {
        const bool blockClose = d == QLatin1String("block-close");
        devices = [blockClose]() -> std::unique_ptr<engine::ISerialDevice> { return std::make_unique<EchoDevice>(blockClose); };
    }
#endif
    static engine::BridgeHost engineHost(devices, *side);
    static WbemSession monitorSession;
    ICimSession* cimForMonitor = &monitorSession;
#ifdef ASB_TEST_SEAMS
    static BlockingCim blockingCim(monitorSession);
    cimForMonitor = &blockingCim;
#endif
    static CimFirewallMonitor monitor(*cimForMonitor, joinPath(paths->lockDirectory, InstallBinary::kInstalledName));
    const QByteArray instanceId = QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0') +
                                  QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
    runner = new BridgeRunner(engineLock, token, folder, engineHost, *side, monitor, *firewallWatchdog, instanceId);
    runner->setLastWords([ntp](const QString& text) { sayExitReason(*ntp, folder, text, QtCriticalMsg, EVENTLOG_ERROR_TYPE, 7); });
    g_sayNotActivated = [ntp](const QString& text) {
        sayExitReason(*ntp, folder, text, QtWarningMsg, EVENTLOG_WARNING_TYPE, DWORD(svc::kNotActivatedCode));
    };

#ifdef ASB_TEST_SEAMS
    if (const int ms = envInt("ASB_TEST_HOLD_START_MS"); ms > 0) {
        for (int waited = 0; waited < ms; waited += 500) {
            Sleep(500);
            status.startPending();
        }
    }
#endif
    // 7. The managed engine: lock, trust, token, config, firewall check, listeners.
    const RunnerStart started = runner->start();
    if (life.started(started, runner->tokenRefusal())) {
        side->log(QtInfoMsg, QStringLiteral("Aether Serial Bridge %1 started (pid %2).")
                                 .arg(QString::fromLatin1(core::version().data(), qsizetype(core::version().size())))
                                 .arg(GetCurrentProcessId()));
#ifdef ASB_TEST_SEAMS
        g_runningAtMs = qint64(GetTickCount64());
        armTestFault(status);
        // A blocked sink must see a line after Running: one every second keeps the side channel busy.
        if (envInt("ASB_TEST_BLOCK_SINK") == 1) {
            auto* tick = new QTimer(app);
            QObject::connect(tick, &QTimer::timeout, [] {
                if (ISideChannel* s = g_side.load()) s->log(QtInfoMsg, QStringLiteral("test seam: tick"));
            });
            tick->start(1000);
        }
#endif
    }
    app->exec();
    // The event loop never returns in an orderly run (the stop ends the process); if it does, end the same way.
    Exit::now(*killDeadline, 0, &ServiceExit::reportStopped);
}

}  // namespace asb::win
