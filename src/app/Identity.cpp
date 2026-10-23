#include "app/Identity.h"

#include <cstdio>
#include <cstdlib>

#include <QCoreApplication>
#include <QStandardPaths>

#ifdef ASB_TEST_SEAMS
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#endif

namespace asb::app {

namespace {
bool g_identityApplied = false;
}

void applyIdentity() {
    QCoreApplication::setApplicationName(QString::fromLatin1(kApplicationName));
    QCoreApplication::setOrganizationName(QString());
    QCoreApplication::setOrganizationDomain(QString());
    g_identityApplied = true;
}

bool identityApplied() { return g_identityApplied; }

StartupPaths::StartupPaths() {
    if (!g_identityApplied) {
        // No Qt message handler, no dialog, no core dump, no I/O beyond one line: exit at once.
        std::fputs("INV-70: a per-user path was resolved before the application identity was set\n",
                   stderr);
        std::_Exit(kExitIdentityViolation);
    }
    m_appConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    m_appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_guiIni = m_appConfig + QStringLiteral("/gui.ini");
}

const StartupPaths& StartupPaths::get() {
    static const StartupPaths instance;
    return instance;
}

#ifdef ASB_TEST_SEAMS
int runIdentityProbe(const char* outFile, int& argc, char** argv, const char* kind,
                     const char* headlessDigest) {
    // Everything below the first three fields is read from state captured at startup, not
    // recomputed here, so a late setApplicationName cannot make a wrong startup look right.
    const StartupPaths& paths = StartupPaths::get();
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QString::fromLatin1(kind));
    o.insert(QStringLiteral("identityApplied"), identityApplied());
    o.insert(QStringLiteral("applicationName"), QCoreApplication::applicationName());
    o.insert(QStringLiteral("organizationName"), QCoreApplication::organizationName());
    o.insert(QStringLiteral("organizationDomain"), QCoreApplication::organizationDomain());
    o.insert(QStringLiteral("guiIniPathAtStartup"), paths.guiIniPath());
    o.insert(QStringLiteral("appConfigAtStartup"), paths.appConfigDir());
    o.insert(QStringLiteral("appDataAtStartup"), paths.appDataDir());
    {
        // The identity must survive the construction of the application object.
        QCoreApplication app(argc, argv);
        o.insert(QStringLiteral("applicationNameAfterConstruction"),
                 QCoreApplication::applicationName());
    }
    if (headlessDigest) {
        o.insert(QStringLiteral("headlessDigest"), QString::fromLatin1(headlessDigest));
    }
    QFile f(QString::fromLocal8Bit(outFile));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return 1;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.error() == QFileDevice::NoError ? 0 : 1;
}

void probePathBeforeIdentity() {
    // Called before applyIdentity(): must not return.
    (void)StartupPaths::get();
}
#endif

}  // namespace asb::app
