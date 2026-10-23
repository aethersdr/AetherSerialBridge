// The AetherSDR excerpts in src/gui/CanonBody.cpp (master §3.3, WS-2).

#include <QApplication>
#include <QFontDatabase>
#include <QStyle>
#include <QtTest>

#include "gui/CanonBody.h"

class TstCanonBody : public QObject {
    Q_OBJECT

private slots:
    // main.cpp:478-491: both bundled Inter faces register from the resources.
    void interFontsRegister() {
        QCOMPARE(asb::gui::registerInterFonts(), 2);
        QVERIFY(QFontDatabase::families().contains(QStringLiteral("Inter")));
    }

    // main.cpp:698-700.
    void fusionStyle() {
        asb::gui::applyFusionStyle(*qApp);
        QCOMPARE(qApp->style()->name().toLower(), QStringLiteral("fusion"));
    }

    // StagePage.cpp:119-130, kept verbatim as the reference for the bridge's container rule.
    void canonBodyStyleSheetReference() {
        const QString sheet = asb::gui::canonBodyStyleSheet();
        QVERIFY(sheet.startsWith(QStringLiteral(
            "QWidget#canonBody, QWidget#canonBody QWidget { background: transparent; }")));
        QVERIFY(sheet.contains(QStringLiteral("QWidget#canonBody { color: {{color.canon.inkSoft}}; }")));
        QVERIFY(sheet.endsWith(QStringLiteral(" { background: {{color.canon.raised}}; }")));
    }
};

QTEST_MAIN(TstCanonBody)
#include "tst_CanonBody.moc"
