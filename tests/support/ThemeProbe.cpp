// The theme probe: one ThemeManager per process, observed from outside by tst_ThemePatches
// (INV-52), on Linux under strace. Built twice:
//   asb_theme_probe             with the bridge's ThemeManagerHost (no user theme directory)
//   asb_theme_probe_aetherhost  with AetherSDR's host expression (the positive control: the same
//                               observation must see the user directory being read)
//
// usage: <probe> scan
//          Plants a valid theme in <GenericConfigLocation>/AetherSDR/themes and one in the working
//          directory, then constructs ThemeManager between two markers.
//        <probe> activate-bytes
//          Constructs ThemeManager, builds theme bytes that exist in no file, then calls
//          activateValidatedTheme("Default Dark", bytes) between two markers.
// Prints one JSON object on stdout. The markers are access() calls on paths that cannot exist,
// so strace shows exactly which syscalls happened between them.

#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "core/ThemeManager.h"
#include "support/ThemeFixtures.h"

namespace {

void mark(const char* path) {
#ifdef Q_OS_UNIX
    (void)::access(path, F_OK);
#else
    (void)path;
#endif
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

}  // namespace

int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QJsonObject out;

    if (mode == QLatin1String("scan")) {
        const QString userDir =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
            QStringLiteral("/AetherSDR/themes");
        const QString userFile = userDir + QStringLiteral("/asb-planted-user-theme.json");
        const QString cwdFile = QDir::current().absoluteFilePath(
            QStringLiteral("asb-planted-cwd-theme.json"));
        if (!QDir().mkpath(userDir) ||
            !writeFile(userFile, asb::test::themeVariant(QStringLiteral("ASB Planted User Theme"))) ||
            !writeFile(cwdFile, asb::test::themeVariant(QStringLiteral("ASB Planted CWD Theme")))) {
            std::fprintf(stderr, "probe: could not plant the theme files\n");
            return 2;
        }
        mark("/ASB_MARK_BEGIN");
        auto& tm = AetherSDR::ThemeManager::instance();
        mark("/ASB_MARK_END");
        out.insert(QStringLiteral("userThemeDir"), QDir::cleanPath(userDir));
        out.insert(QStringLiteral("userFile"), QDir::cleanPath(userFile));
        out.insert(QStringLiteral("cwdFile"), QDir::cleanPath(cwdFile));
        out.insert(QStringLiteral("themes"), QJsonArray::fromStringList(tm.availableThemes()));
        out.insert(QStringLiteral("active"), tm.activeTheme());
    } else if (mode == QLatin1String("activate-bytes")) {
        auto& tm = AetherSDR::ThemeManager::instance();
        const QByteArray bytes = asb::test::bytesOnlyTheme(QStringLiteral("Default Dark"));
        if (bytes.isEmpty()) {
            std::fprintf(stderr, "probe: could not build the theme bytes\n");
            return 2;
        }
        mark("/ASB_MARK_BEGIN");
        const bool ok = tm.activateValidatedTheme(QStringLiteral("Default Dark"), bytes);
        mark("/ASB_MARK_END");
        out.insert(QStringLiteral("ok"), ok);
        out.insert(QStringLiteral("active"), tm.activeTheme());
        out.insert(QStringLiteral("cyan"), tm.value(QStringLiteral("color.canon.cyan")));
    } else {
        std::fprintf(stderr, "usage: %s scan|activate-bytes\n", argv[0]);
        return 2;
    }

    const QByteArray json = QJsonDocument(out).toJson(QJsonDocument::Compact);
    std::fwrite(json.constData(), 1, static_cast<size_t>(json.size()), stdout);
    std::fputc('\n', stdout);
    return 0;
}
