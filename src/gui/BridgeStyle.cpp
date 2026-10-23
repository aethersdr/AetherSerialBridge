#include "gui/BridgeStyle.h"

#include "core/ThemeManager.h"
#include "gui/ModemChrome.h"
#include "gui/Theme.h"

#include <QApplication>
#include <QIcon>
#include <QObject>

namespace asb::gui {

QString bridgeContainerRules()
{
    // Containers only, by object name or class; never a control (B6). The ink colour and the popup rules are
    // canonBodyStyleSheet's (AetherSDR StagePage.cpp:119-130): popups are separate windows, so they get their
    // panel back.
    return QStringLiteral(
        "QWidget#canonBody, QWidget#asbHeader, QWidget#asbServiceRow, QWidget#asbPortsPanel, QFrame#asbCardHost,"
        " QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"
        "QWidget#canonBody { color: {{color.canon.inkSoft}}; }"
        "QWidget#canonBody QMenu, QWidget#canonBody QComboBoxPrivateContainer,"
        " QWidget#canonBody QComboBox QAbstractItemView"
        " { background: {{color.canon.raised}}; }");
}

QString bridgeTableRules()
{
    // ModemChrome.cpp's QTableWidget / ::item / QHeaderView::section declarations with the Canon palette's values
    // (Dialog scale), on QTableView: the bridge's table is a QTableView over PortTableModel.
    return QStringLiteral(
        "QTableView {"
        " color: {{color.canon.inkSoft}};"
        " background: {{color.canon.control}};"
        " alternate-background-color: {{color.canon.raised}};"
        " border: none;"
        " gridline-color: {{color.canon.line}};"
        " font-family: \"SF Mono\", \"Menlo\", \"Consolas\", monospace;"
        " font-size: 13px;"
        " selection-background-color: {{color.canon.nested}};"
        " selection-color: {{color.canon.ink}}; }"
        "QTableView::item { padding: 2px 10px; }"
        "QTableView QHeaderView::section {"
        " color: {{color.canon.muted}};"
        " background: {{color.canon.raised}};"
        " border: none;"
        " border-bottom: 1px solid {{color.canon.line}};"
        " padding: 5px 8px;"
        " font-size: 11px;"
        " font-weight: 700; }"
        "QTableView QTableCornerButton::section { background: {{color.canon.raised}}; border: none; }");
}

QString bridgeLabelRules()
{
    // The card is a nested surface with a hairline (style guide "card of label / value rows"); notes carry their
    // meaning in words, the colour only repeats it (state never colour alone).
    return QStringLiteral(
        "QFrame#asbCard { background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}};"
        " border-radius: 12px; }"
        "QFrame#asbCard QLabel { background: transparent; }"
        "QLabel#asbSubtitle { color: {{color.canon.muted}}; }"
        "QLabel#asbCardTitle { color: {{color.canon.ink}}; font-weight: 700; }"
        "QLabel#asbFieldLabel { color: {{color.canon.muted}}; }"
        "QLabel#asbHint { color: {{color.canon.muted}}; }"
        "QLabel#asbServiceText { color: {{color.canon.ink}}; }"
        "QLabel[tone=\"info\"] { color: {{color.canon.inkSoft}}; }"
        "QLabel[tone=\"good\"] { color: {{color.accent.success}}; }"
        "QLabel[tone=\"warn\"] { color: {{color.accent.warning}}; }"
        "QLabel[tone=\"bad\"] { color: {{color.accent.danger}}; }"
        "QLabel#StatusDot[tone=\"good\"] { background: {{color.accent.success}}; }"
        "QLabel#StatusDot[tone=\"warn\"] { background: {{color.accent.warning}}; }"
        "QLabel#StatusDot[tone=\"bad\"] { background: {{color.accent.danger}}; }"
        "QLabel#StatusDot[tone=\"info\"] { background: {{color.canon.muted}}; }"
        "QPlainTextEdit#asbLog { color: {{color.canon.inkSoft}}; background: {{color.canon.control}};"
        " border: 1px solid {{color.canon.line}}; border-radius: 7px;"
        " font-family: \"SF Mono\", \"Menlo\", \"Consolas\", monospace; font-size: 12px; }");
}

QString composedBodySheet()
{
    return AetherSDR::ModemChrome::styleSheet(AetherSDR::ModemChrome::Scale::Dialog,
                                              AetherSDR::ModemChrome::Look::Canon) +
           bridgeContainerRules() + bridgeTableRules() + bridgeLabelRules();
}

void applyApplicationSheet(QApplication& app)
{
    auto& tm = AetherSDR::ThemeManager::instance();
    // Resolved at root scope (resolve(), not resolveFor()): the application sheet is everyone's base.
    app.setStyleSheet(tm.resolve(AetherSDR::appStylesheetTemplate()));
    QObject::connect(&tm, &AetherSDR::ThemeManager::themeChanged, &app, [&app] {
        app.setStyleSheet(AetherSDR::ThemeManager::instance().resolve(AetherSDR::appStylesheetTemplate()));
    });
}

void applyApplicationIcon(QApplication& app)
{
    QIcon icon;
    for (const int size : {16, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/appicon/asb-icon-%1.png").arg(size), QSize(size, size));
    app.setWindowIcon(icon);
    QApplication::setDesktopFileName(QStringLiteral("aether-serial-bridge"));
}

}  // namespace asb::gui
