#include "cli/WindowsHost.h"

#include "cli/Elevation.h"
#include "platform/win/EngineLock.h"
#include "platform/win/InstallBinary.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/WbemSession.h"
#include "platform/win/WinFileIo.h"
#include "platform/win/WindowsDataFolder.h"
#include "platform/win/WindowsPlatform.h"
#include "platform/win/WindowsService.h"
#include "platform/win/SelfElevate.h"

#include <QDir>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>

#include <cstdio>
#include <string>

namespace asb::cli {
namespace {

QString moduleFileName()
{
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return QString::fromStdWString(buf);
}

}  // namespace

WindowsHost::WindowsHost(int argc, char** argv, Image image) : m_argc(argc), m_argv(argv), m_image(std::move(image))
{
    QString error;
    // windowsPaths(): serviceLocations() and %ProgramFiles%\AetherSerialBridge; empty strings when either cannot be
    // determined (the commands then refuse with exit 3, never a guessed folder).
    if (const std::optional<win::WindowsPaths> p = win::windowsPaths(&error)) {
        m_dataFolder = p->dataFolder;
        m_configFile = QDir(p->dataFolder).filePath(QStringLiteral("config.json"));
        m_lockDirectory = p->lockDirectory;
    }
    const QString self = moduleFileName();
    if (m_image.path.isEmpty()) {
        m_image.path = self;
    } else if (!m_image.path.contains(QLatin1Char('\\')) && !m_image.path.contains(QLatin1Char('/'))) {
        m_image.path = self.left(self.lastIndexOf(QLatin1Char('\\')) + 1) + m_image.path;
    }
}

WindowsHost::~WindowsHost()
{
    // The Platform is deliberately not destroyed: its CIM connection was made on the OperationHost's worker thread,
    // and releasing those COM proxies here, after that thread ended, crashed the process at exit (CI run
    // 38718967072: the operation succeeded, the exit code was a crash). The process ends right after this, which
    // releases every handle and lock the Platform holds.
    (void)m_wp.release();
}

void WindowsHost::enterGuardedMode()
{
    QString error;
    if (!win::installProcessGuard(&error)) {
        const QByteArray line = QByteArrayLiteral("aether-serial-bridge: the process guard could not be installed: ") +
                                error.toLocal8Bit() + '\n';
        std::fputs(line.constData(), stderr);
        std::fflush(stderr);
        ExitProcess(5);
    }
}

void WindowsHost::enterStandardUserJob()
{
    unsigned limit = 2;   // D65: the window and one child of its own; a limit of 1 is believed to block `runas` (HT W-2)
#ifdef ASB_TEST_SEAMS
    // HT W-2 step 5 only: a test build of the same commit with limit 1, relaunched through the real runas and UAC.
    if (qEnvironmentVariableIntValue("ASB_TEST_STANDARD_JOB_LIMIT") == 1) limit = 1;
#endif
    QString error;
    if (!win::installStandardUserJob(limit, &error)) {
        const QByteArray line = QByteArrayLiteral("aether-serial-bridge: the window's job could not be installed: ") +
                                error.toLocal8Bit() + '\n';
        std::fputs(line.constData(), stderr);
        std::fflush(stderr);
        ExitProcess(5);
    }
}

bool WindowsHost::elevated()
{
    return Elevation::isElevated();
}

Platform* WindowsHost::platform(QString* error, int* code)
{
    if (m_wp) return &m_wp->platform();
    // This thread joins the multithreaded apartment and sets the process's COM security (as the service host does)
    // before any operation runs, so the apartment the operation worker's CIM connection lives in outlasts that
    // worker. A failure is not fatal here: the firewall steps then fail closed with the COM error.
    win::initialiseComSecurity(nullptr);
    QByteArray expected = m_image.expectedSha256;
    if (expected.isEmpty()) {
        QString err;
        const std::optional<QByteArray> bytes = win::readWholeFile(m_image.path, &err);
        if (!bytes) {
            if (error) *error = QStringLiteral("cannot read %1: %2").arg(QDir::toNativeSeparators(m_image.path), err);
            if (code) *code = 1;
            return nullptr;
        }
        expected = win::InstallBinary::sha256Hex(*bytes);
    }
    m_expectedSha256 = expected;
    win::WindowsPlatform::Options o;
    o.expectedHeadlessSha256 = expected;
    const QString path = m_image.path;
    o.image = [path](QString* e) { return win::readWholeFile(path, e); };
    o.lockReport = [this](const QString& line) {
        std::lock_guard<std::mutex> g(m_reportsMutex);
        m_reports << line;
    };
    QString err;
    m_wp = win::WindowsPlatform::create(std::move(o), &err);
    if (!m_wp) {
        if (error) *error = err;
        if (code) *code = 3;   // the Windows folders cannot be determined (kWindowsDataFolderError)
        return nullptr;
    }
#ifdef ASB_TEST_SEAMS
    // tst_GuiNative's handle-probe control only: the v1.0 lock-directory handle, kept for the object's life (INV-128).
    if (qEnvironmentVariableIsSet("ASB_TEST_KEEP_LOCK_DIRECTORY"))
        if (auto* lock = dynamic_cast<win::EngineLock*>(&m_wp->platform().engineLock)) lock->setKeepDirectory(true);
#endif
    return &m_wp->platform();
}

QString WindowsHost::thisBuild()
{
    const QString version = Host::thisBuild();
    if (m_expectedSha256.isEmpty()) return version;
    return QStringLiteral("%1, program sha256 %2").arg(version, QString::fromLatin1(m_expectedSha256.left(12)));
}

QStringList WindowsHost::takeReports()
{
    QStringList r;
    {
        std::lock_guard<std::mutex> g(m_reportsMutex);
        r.swap(m_reports);
    }
    if (m_wp) r << m_wp->service().takeChanges();
    return r;
}

ScratchFolder WindowsHost::scratchFolder(const QString& directory)
{
    ScratchFolder s;
    s.folder = std::make_unique<win::WindowsDataFolder>(win::nativeSeams(), directory);
    // Default (inherited) security: the invoking user owns what it writes beside its scratch config.
    s.io = std::make_unique<win::WinFileIo>(win::nativeSeams(), win::CreateSecurity{});
    return s;
}

QString WindowsHost::ownExecutable()
{
    return moduleFileName();
}

QString WindowsHost::installedExecutable()
{
    if (m_lockDirectory.isEmpty()) return {};
    return QDir(m_lockDirectory).filePath(QString::fromLatin1(win::InstallBinary::kInstalledName));
}

int WindowsHost::runScratch(const QString&, const BridgeConfig&, Console& c)
{
    c.err(QStringLiteral("error: --run runs the bridge, which only the console executable has: aether-serial-bridge.exe "
                         "--run --config <file>."));
    return 2;
}

int WindowsHost::runService()
{
    std::fputs("error: --service runs only in the console executable, started by the service manager.\n", stderr);
    return 2;
}

int WindowsHost::listPorts(Console& c)
{
    c.err(QStringLiteral("error: --list-ports runs with the console executable: aether-serial-bridge.exe --list-ports."));
    return 2;
}

std::optional<bool> WindowsHost::confirm(const QString& question)
{
    if (!_isatty(_fileno(stdin))) return std::nullopt;
    std::fputs(question.toLocal8Bit().constData(), stderr);
    std::fflush(stderr);
    char line[64] = {};
    if (!std::fgets(line, static_cast<int>(sizeof line), stdin)) return false;
    const QString a = QString::fromLocal8Bit(line).trimmed().toLower();
    return a == QLatin1String("y") || a == QLatin1String("yes");
}

bool WindowsHost::messageBox(const QString& text, Box kind)
{
#ifdef ASB_TEST_SEAMS
    // Test builds: a native test answers the question and reads the other boxes on stderr; nothing waits for a click.
    const QString scripted = qEnvironmentVariable("ASB_TEST_MESSAGEBOX");
    if (!scripted.isEmpty()) {
        const QByteArray line = QByteArrayLiteral("[message box] ") + text.toUtf8() + '\n';
        std::fputs(line.constData(), stderr);
        std::fflush(stderr);
        return kind == Box::Question && scripted == QLatin1String("yes");
    }
#endif
    UINT flags = MB_SETFOREGROUND | MB_TOPMOST;
    switch (kind) {
    case Box::Question: flags |= MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2; break;
    case Box::Information: flags |= MB_OK | MB_ICONINFORMATION; break;
    case Box::Error: flags |= MB_OK | MB_ICONERROR; break;
    }
    const std::wstring body = text.toStdWString();
    return MessageBoxW(nullptr, body.c_str(), L"Aether Serial Bridge", flags) == IDYES;
}

void WindowsHost::detachConsole()
{
#ifdef ASB_TEST_SEAMS
    if (!qEnvironmentVariable("ASB_TEST_MESSAGEBOX").isEmpty()) return;   // the test reads stderr
#endif
    FreeConsole();
}

Host::Relaunch WindowsHost::relaunchElevated(const QStringList& arguments)
{
    const win::SelfElevate::Outcome o = win::SelfElevate::relaunch(arguments);
    Relaunch r;
    r.started = o.started;
    r.cancelled = o.cancelled;
    r.exitCode = o.exitCode;
    r.error = o.error;
    return r;
}

}  // namespace asb::cli
