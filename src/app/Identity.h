#pragma once

// Application identity (D20, INV-70).
//
// Qt derives every per-user directory (QStandardPaths::AppConfigLocation, AppDataLocation, the
// QSettings default scope, ...) from QCoreApplication::applicationName() and organizationName().
// In Qt 6 applicationName() is EMPTY until a Q*Application object exists, and only after that does
// it fall back to the executable's base name. So a path resolved before the identity is set lands
// in the bare config/data root (no application directory at all), and one resolved after an
// application object exists but before the identity is set lands under the executable's file
// name. Either way the data goes to the wrong place, silently and for good.
//
// So: applyIdentity() is the FIRST statement of main() in both executables, before any
// Q*Application is constructed and before any code that can resolve a path. StartupPaths is the
// one place that resolves them, once, and it refuses to run before the identity exists.

#include <QString>

namespace asb::app {

inline constexpr char kApplicationName[] = "AetherSerialBridge";

// Exit code (EX_SOFTWARE) of a process that resolved a per-user path before the identity existed.
inline constexpr int kExitIdentityViolation = 70;

// setApplicationName("AetherSerialBridge"), empty organisation name and domain. Idempotent.
void applyIdentity();

bool identityApplied();

// Per-user locations, resolved ONCE at the first call and cached for the life of the process, the
// way the vendored AppSettings singleton resolves its file (WS-2). Exits the process (code
// kExitIdentityViolation) if the identity has not been applied: a wrong path must never be used,
// and never silently.
class StartupPaths {
public:
    static const StartupPaths& get();

    // <AppConfigLocation>/gui.ini: the GUI's settings file.
    const QString& guiIniPath() const { return m_guiIni; }
    const QString& appConfigDir() const { return m_appConfig; }
    const QString& appDataDir() const { return m_appData; }

private:
    StartupPaths();
    QString m_appConfig;
    QString m_appData;
    QString m_guiIni;
};

#ifdef ASB_TEST_SEAMS
// Test builds only (never a release): `--identity-probe <file>` writes the identity and the paths
// StartupPaths captured at startup as JSON, so tst_Identity can assert on a real process. Returns
// the process exit code. `headlessDigest` is null for the headless executable.
int runIdentityProbe(const char* outFile, int& argc, char** argv, const char* kind,
                     const char* headlessDigest);

// `--probe-path-before-identity`: resolves StartupPaths first, to prove the guard aborts.
void probePathBeforeIdentity();
#endif

}  // namespace asb::app
