// Produces a theme file with AetherSDR's own Save As (WS-11, INV-36): the vendored ThemeManager
// forks Default Light, edits one colour, and saveCurrentThemeAs() writes the document AetherSDR
// writes (schema 2, primitives, scopes, and the recorded "baseTheme").
//
// usage: asb_theme_saveas_gen <out-file>
// Runs in Qt test mode; the caller gives it a scratch HOME (POSIX), so ThemeManager's
// <GenericConfigLocation>/AetherSDR/themes is a scratch directory. The written file is copied to
// <out-file>. tst_ThemeFollower::saveAsFixtureIsAetherSdrOutput compares it with the committed
// fixture tests/data/themes/pass_saveas_v2_basetheme.json.

#include <cstdio>

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QStandardPaths>

#include "core/ThemeManager.h"

int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: asb_theme_saveas_gen <out-file>\n");
        return 2;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);
    auto& tm = AetherSDR::ThemeManager::instance();
    if (!tm.setActiveTheme(QStringLiteral("Default Light"))) {
        std::fprintf(stderr, "cannot activate Default Light\n");
        return 1;
    }
    tm.setColor(QStringLiteral("color.accent"), QColor(QStringLiteral("#0a7f6e")));
    const QString name = QStringLiteral("ASB Saved Light");
    if (!tm.saveCurrentThemeAs(name)) {
        std::fprintf(stderr, "saveCurrentThemeAs failed\n");
        return 1;
    }
    const QString written = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                            QStringLiteral("/AetherSDR/themes/") + name + QStringLiteral(".json");
    QFile src(written);
    if (!src.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(written));
        return 1;
    }
    const QByteArray bytes = src.readAll();
    src.close();
    QFile::remove(written);
    QFile dst(out);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate) || dst.write(bytes) != bytes.size()) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(out));
        return 1;
    }
    std::printf("%s\n", qPrintable(written));
    return 0;
}
