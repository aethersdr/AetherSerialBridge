// tst_Tui (WS-12): the TUI's controller and screens in process, over the WS-5 fakes (support/TuiScenario.h).
// Screens are rendered to text at a fixed size, so they are compared exactly (goldens in tests/data/tui; set
// ASB_UPDATE_GOLDENS=1 to rewrite them after a deliberate change, then review the diff).
//
// Owned rows here: INV-13 quitWithUnsavedAsks (WS-10's), INV-33 untrustedFolderBanner (WS-10's TUI row), the INV-107
// dirty rule (digestMismatchKeepsDirty, barrierDeadlineKeepsDirty in process), INV-118 refusals while busy, INV-72's
// render half (noColourRenderHasNoColour; the process half is tst_TuiProcess::noColorHasNoSgrColour).

#include "support/DemoFixture.h"
#include "support/SanitizerFailLine.h"
#include "support/TuiScenario.h"

#include "core/AllowedClients.h"
#include "core/ConfigJson.h"
#include "core/FirewallKeywords.h"
#include "core/HelpText.h"
#include "tui/Logo.h"
#include "tui/TuiApp.h"
#include "tui/TuiHelp.h"
#include "tui/TuiText.h"
#include "tui/TuiView.h"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <QDir>
#include <QScopeGuard>
#include <QJsonArray>
#include <QRegularExpression>
#include <QFile>
#include <QTest>

#include <algorithm>
#include <functional>
#include <memory>

using namespace asb;
using namespace asb::test;
using namespace asb::tui;
using ftxui::Event;

namespace {

struct Fixture {
    explicit Fixture(const QString& scenario, ColorMode color = ColorMode::None, bool windows = false,
                     const std::function<void(Fixture&)>& before = {},
                     const std::function<void(TuiApp::Env&)>& setEnv = {})
    {
        sc = std::make_unique<TuiScenario>(rig, host);
        sc->apply(scenario);
        if (before) before(*this);   // the docs images' demo fixture (WS-19): set up before the TUI first reads
        QString err;
        int code = 0;
        TuiApp::Env env;
        env.elevated = host.isElevated;
        env.windows = windows;
        env.utf8 = true;
        env.color = color;
        env.statusPollMs = 0;   // the tests refresh by hand
        env.servicePollMs = 0;
        env.program = windows ? QStringLiteral("C:/Users/op/Downloads/aether-serial-bridge.exe")
                              : QStringLiteral("/home/op/My Downloads/aether-serial-bridge-x86_64.AppImage");
        if (setEnv) setEnv(env);
        app = std::make_unique<TuiApp>(host, host.platform(&err, &code), QString(), env);
    }
    ~Fixture()
    {
        if (sc) sc->release();
        // The worker must have returned before the fakes go (OperationHost's destructor waits).
        app.reset();
    }

    void key(const Event& e) { app->onEvent(e); }
    void key(const char* chars)
    {
        for (const char* c = chars; *c; ++c) app->onEvent(Event::Character(*c));
    }
    QString screen(int w = 80, int h = 24) const
    {
        QStringList l;
        for (const std::string& s : renderText(app->state(), w, h)) l << QString::fromStdString(s);
        return l.join(QLatin1Char('\n'));
    }
    // The screen as one line of words (wrapped sentences joined), at a width without the logo.
    QString flat(int w = 59, int h = 24) const
    {
        QStringList l;
        for (const std::string& s : renderText(app->state(), w, h)) l << QString::fromStdString(s).trimmed();
        return l.join(QLatin1Char(' ')).simplified();
    }
    // Mutations the Applier side recorded: anything but reads of the folder and queries of the service.
    QStringList changes() const
    {
        QStringList out;
        for (const QString& e : rig.world.log.all()) {
            if (e.startsWith(QLatin1String("folder:inspect")) || e.startsWith(QLatin1String("folder:read")) ||
                e == QLatin1String("service:detect") || e == QLatin1String("service:query"))
                continue;
            out << e;
        }
        return out;
    }

    Rig rig;
    FakeCliHost host{rig};
    std::unique_ptr<TuiScenario> sc;
    std::unique_ptr<TuiApp> app;
};

StatusReport amplifierConnected()
{
    StatusReport r;
    r.pid = 4000;
    PortStatus p;
    p.name = QStringLiteral("Amp");
    p.comPort = QStringLiteral("COM3");
    p.bindAddress = QStringLiteral("0.0.0.0");
    p.tcpPort = 4001;
    p.state = BridgeState::Connected;
    p.client = QStringLiteral("10.0.0.5:51234");
    r.ports << p;
    return r;
}

// A complete installation of another build asks on open whether to update it (user report 2026-10-20); the tests
// about Save & apply's own build question dismiss it first (Esc: nothing changes).
void dismissUpdate(Fixture& f)
{
    QCOMPARE(f.app->state().screen, Screen::Confirm);
    QVERIFY(f.app->state().question.contains(QStringLiteral("Update it to this build?")));
    f.key(Event::Escape);
    QCOMPARE(f.app->state().screen, Screen::Ports);
}

QString goldenPath(const QString& name)
{
    return QStringLiteral(ASB_TEST_DATA_DIR "/tui/%1.txt").arg(name);
}

// The build question's two routes over the fakes (v1.1 WS-14): Windows, and Linux, where detect() now finds another
// build by the installed program's SHA-256 (INV-120) and LinuxHost names this build's digest; the installed program
// here is a v1.0 build (no stamp).
void routes()
{
    QTest::addColumn<bool>("windows");
    QTest::newRow("windows") << true;
    QTest::newRow("linux") << false;
}
void linuxRoute(Fixture& f, bool windows)
{
    if (windows) return;
    f.rig.service.installation.flavor = Installation::Flavor::Linux;
    f.rig.service.installation.installedVersion = QString();   // read, no stamp: a build before 1.1
    f.host.buildName = QStringLiteral("1.1.0, program sha256 0123456789ab");
    f.app->refreshService();
}

// n exposed ports, every one on every interface with no allowed clients, named with `width` characters.
QByteArray exposedPorts(int n, int width)
{
    QStringList ports;
    for (int i = 1; i <= n; ++i) {
        QString name = QStringLiteral("Port-%1-").arg(i, 2, 10, QLatin1Char('0'));
        while (name.size() < width) name += QChar(QLatin1Char('A' + (name.size() % 26)));
        ports << QStringLiteral(R"({"enabled": true, "name": "%1", "comPort": "COM%2", "baudRate": 38400, "tcpPort": %3, )"
                                R"("bindAddress": "0.0.0.0", "allowedClients": ""})")
                     .arg(name)
                     .arg(i)
                     .arg(4000 + i);
    }
    return QStringLiteral(R"({"version": 1, "ports": [%1]})").arg(ports.join(QStringLiteral(", "))).toUtf8();
}

// ---- the help's guard (owner request 2026-10-19) -------------------------------------------------------------------
// Every key a terminal can send that the TUI could answer, by the name the help's key table uses (TuiHelp.h).
struct Probe {
    QString name;
    Event event;
};
std::vector<Probe> probes()
{
    std::vector<Probe> out{
        {QStringLiteral("Up"), Event::ArrowUp},       {QStringLiteral("Down"), Event::ArrowDown},
        {QStringLiteral("Left"), Event::ArrowLeft},   {QStringLiteral("Right"), Event::ArrowRight},
        {QStringLiteral("Enter"), Event::Return},     {QStringLiteral("Esc"), Event::Escape},
        {QStringLiteral("Tab"), Event::Tab},          {QStringLiteral("Shift+Tab"), Event::TabReverse},
        {QStringLiteral("Backspace"), Event::Backspace}, {QStringLiteral("Delete"), Event::Delete},
        {QStringLiteral("Home"), Event::Home},        {QStringLiteral("End"), Event::End},
        {QStringLiteral("PgUp"), Event::PageUp},      {QStringLiteral("PgDn"), Event::PageDown},
        {QStringLiteral("Insert"), Event::Insert},    {QStringLiteral("F1"), Event::F1},
        {QStringLiteral("Ctrl+C"), Event::CtrlC},     {QStringLiteral("Ctrl+D"), Event::CtrlD},
        {QStringLiteral("Ctrl+Z"), Event::CtrlZ},
    };
    for (char c = 0x20; c < 0x7f; ++c)
        out.push_back({c == ' ' ? QStringLiteral("Space") : QString(QLatin1Char(c)), Event::Character(c)});
    return out;
}

}  // namespace

class tst_Tui : public QObject {
    Q_OBJECT

private slots:
    // ---- render goldens (80 x 24 and 40 x 20, section 6.3) --------------------------------------------------------
    void renderGoldens_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<int>("color");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<QString>("keys");
        QTest::newRow("ports-80x24") << "installed" << int(ColorMode::None) << 80 << 24 << "";
        QTest::newRow("ports-40x20") << "installed" << int(ColorMode::None) << 40 << 20 << "";
        QTest::newRow("ports-80x24-truecolor") << "installed" << int(ColorMode::TrueColor) << 80 << 24 << "";
        QTest::newRow("ports-80x24-dirty-pending") << "installed,pending" << int(ColorMode::None) << 80 << 24 << " ";
        QTest::newRow("readonly-80x24") << "installed,readonly" << int(ColorMode::None) << 80 << 24 << "";
        QTest::newRow("readonly-40x20") << "installed,readonly" << int(ColorMode::None) << 40 << 20 << "";
        QTest::newRow("untrusted-80x24") << "installed,untrusted" << int(ColorMode::None) << 80 << 24 << "";
        QTest::newRow("fresh-40x20") << "fresh" << int(ColorMode::None) << 40 << 20 << "";
        QTest::newRow("edit-80x24") << "installed" << int(ColorMode::None) << 80 << 24 << "\r";
        QTest::newRow("edit-40x20") << "installed" << int(ColorMode::None) << 40 << 20 << "\r";
        QTest::newRow("confirm-80x24") << "installed" << int(ColorMode::None) << 80 << 24 << "  s";
        // Round 2: typing a long by-id path at 40 columns keeps its end, and the cursor, in view.
        QTest::newRow("edit-40x20-long-path") << "installed" << int(ColorMode::None) << 40 << 20
                                              << "\r\t\t\b\b\b\b/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A10KQ7XB-if00-port0";
        QTest::newRow("confirm-40x8") << "installed" << int(ColorMode::None) << 40 << 8 << "  s";
        QTest::newRow("confirm-30x5") << "installed" << int(ColorMode::None) << 30 << 5 << "  s";
        // The field error sits with the footer, and stays when the screen is short.
        QTest::newRow("edit-40x12-error") << "installed" << int(ColorMode::None) << 40 << 12 << "\r\t\t\t\b\b\b\b\bfast\r";
        // Owner request 2026-10-19: the help page, and the hint under the form for the focused field (here Allowed).
        QTest::newRow("help-80x24") << "installed" << int(ColorMode::None) << 80 << 24 << "?";
        QTest::newRow("help-40x20") << "installed" << int(ColorMode::None) << 40 << 20 << "?";
        QTest::newRow("edit-80x24-allowed") << "installed" << int(ColorMode::None) << 80 << 24 << "\r\t\t\t\t\t\t\t";
        QTest::newRow("edit-40x20-allowed") << "installed" << int(ColorMode::None) << 40 << 20 << "\r\t\t\t\t\t\t\t";
        // On Windows the Allowed hint is the window's example line, then its sentence. A tag ending in
        // "-windows" renders the Windows route.
        QTest::newRow("edit-80x24-allowed-windows") << "installed" << int(ColorMode::None) << 80 << 24 << "\r\t\t\t\t\t\t\t";
        QTest::newRow("edit-40x20-allowed-windows") << "installed" << int(ColorMode::None) << 40 << 20 << "\r\t\t\t\t\t\t\t";
    }
    void renderGoldens()
    {
        QFETCH(QString, scenario);
        QFETCH(int, color);
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(QString, keys);
        Fixture f(scenario, ColorMode(color), QString::fromLatin1(QTest::currentDataTag()).endsWith(QLatin1String("-windows")));
        f.rig.service.status = amplifierConnected();
        f.app->refreshStatus();
        for (const QChar c : keys) {
            if (c == QLatin1Char('\r')) f.key(Event::Return);
            else if (c == QLatin1Char('\t')) f.key(Event::Tab);
            else if (c == QLatin1Char('\b')) f.key(Event::Backspace);
            else f.key(Event::Character(char(c.toLatin1())));
        }
        const QString got = f.screen(width, height) + QLatin1Char('\n');
        const QString path = goldenPath(QString::fromLatin1(QTest::currentDataTag()));
        if (qEnvironmentVariableIsSet("ASB_UPDATE_GOLDENS")) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile out(path);
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(got.toUtf8());
        }
        QFile in(path);
        QVERIFY2(in.open(QIODevice::ReadOnly), qPrintable(QStringLiteral("no golden %1 (ASB_UPDATE_GOLDENS=1 writes it)").arg(path)));
        const QString want = QString::fromUtf8(in.readAll());
        if (got != want) qWarning().noquote() << "rendered:\n" << got;
        QCOMPARE(got, want);
        // Every line is exactly `width` cells.
        for (const std::string& line : renderText(f.app->state(), width, height))
            QCOMPARE(ftxui::string_width(line), width);
    }

    // ---- INV-33 (WS-10's TUI row) ----------------------------------------------------------------------------------
    void untrustedFolderBanner()
    {
        Fixture f(QStringLiteral("installed,untrusted"));
        const QString s = f.screen();
        QVERIFY2(s.contains(QString::fromLatin1(text::kUntrusted)), qPrintable(s));
        // Nothing from the folder is shown, and the empty table is not "no ports yet".
        QVERIFY2(!s.contains(QString::fromLatin1(text::kNoPorts)), qPrintable(s));
        QVERIFY2(!s.contains(QStringLiteral("Amp")), qPrintable(s));
        QCOMPARE(f.app->state().load, Load::Untrusted);
        for (const QString& e : f.rig.world.log.all()) QVERIFY2(!e.startsWith(QLatin1String("folder:read config")), qPrintable(e));
        // Elevated, it says how to fix it; editing is refused with the same words.
        QVERIFY(s.contains(QStringLiteral("Press R to repair it.")));
        f.key("a");
        QCOMPARE(f.app->state().message, QString::fromLatin1(text::kUntrustedEdit));
        QCOMPARE(f.app->state().screen, Screen::Ports);
    }

    // ---- INV-13 (WS-10's TUI row) ----------------------------------------------------------------------------------
    void quitWithUnsavedAsks()
    {
        Fixture f(QStringLiteral("installed"));
        f.key("q");
        QVERIFY(f.app->quitRequested());   // nothing unsaved: quits

        Fixture g(QStringLiteral("installed"));
        g.key(" ");   // the port off: an edit
        QVERIFY(g.app->state().dirty);
        g.key("q");
        QVERIFY(!g.app->quitRequested());
        QCOMPARE(g.app->state().screen, Screen::Confirm);
        QCOMPARE(g.app->state().question, QString::fromLatin1(text::kQuitDirty));
        g.key("n");
        QVERIFY(!g.app->quitRequested());
        QVERIFY(g.app->state().dirty);
        g.key(Event::CtrlC);   // Ctrl+C is q
        QCOMPARE(g.app->state().screen, Screen::Confirm);
        g.key(Event::Escape);  // Esc is no
        QVERIFY(!g.app->quitRequested());
        g.key("q");
        g.key("y");
        QVERIFY(g.app->quitRequested());
        QVERIFY(g.changes().isEmpty());
    }

    // ---- read only (section 6.1) -------------------------------------------------------------------------------------
    void readOnlyRefusesChanges()
    {
        Fixture f(QStringLiteral("installed,readonly"));
        QVERIFY2(f.flat().contains(QString::fromLatin1(text::kReadOnly)), qPrintable(f.flat()));
        for (const char* k : {"a", "d", " ", "s", "t", "x", "R", "D"}) {
            f.key(k);
            QCOMPARE(f.app->state().message, QString::fromLatin1(text::kReadOnly));
            QCOMPARE(f.app->state().screen, Screen::Ports);
            QVERIFY(!f.app->state().dirty);
        }
        // Enter shows the port; changing a field refuses with the same sentence.
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Edit);
        f.key("9");
        QCOMPARE(f.app->state().form.error, QString::fromLatin1(text::kReadOnly));
        QCOMPARE(f.app->state().form.text[int(Field::Name)], QStringLiteral("Amp"));
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(!f.app->state().dirty);
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join(QLatin1Char('\n'))));
        // Viewing is allowed.
        f.key("?");
        QCOMPARE(f.app->state().screen, Screen::Page);
    }

    // ---- INV-72, render half ---------------------------------------------------------------------------------------
    void noColourRenderHasNoColour()
    {
        auto coloured = [](ColorMode mode, const QString& keys) {
            Fixture f(QStringLiteral("installed,pending"), mode);
            f.rig.service.status = amplifierConnected();
            f.app->refreshStatus();
            for (const QChar c : keys) f.key(Event::Character(char(c.toLatin1())));
            ftxui::Screen screen(80, 24);
            ftxui::Render(screen, render(f.app->state(), 80, 24));
            int n = 0;
            for (int y = 0; y < 24; ++y)
                for (int x = 0; x < 80; ++x) {
                    const ftxui::Cell& c = screen.CellAt(x, y);
                    if (c.foreground_color != ftxui::Color::Default || c.background_color != ftxui::Color::Default) ++n;
                }
            return n;
        };
        for (const QString& keys : {QString(), QStringLiteral(" "), QStringLiteral("?")}) {
            QCOMPARE(coloured(ColorMode::None, keys), 0);
            // The control: the same screens in colour do colour cells.
            QVERIFY(coloured(ColorMode::TrueColor, keys) > 0);
            QVERIFY(coloured(ColorMode::Palette256, keys) > 0);
            QVERIFY(coloured(ColorMode::Basic16, keys) > 0);
        }
    }

    void noColorWins()
    {
        qputenv("NO_COLOR", "1");
        QVERIFY(noColorRequested());   // the TUI's own check, whatever FTXUI does with the variable
        QCOMPARE(currentColorMode(), ColorMode::None);
        qputenv("NO_COLOR", "");
        QVERIFY(!noColorRequested());   // an empty NO_COLOR is not set (no-color.org)
        qunsetenv("NO_COLOR");
    }

    // ---- layout tiers (section 6.3) ---------------------------------------------------------------------------------
    void layoutTiers()
    {
        Fixture f(QStringLiteral("installed"), ColorMode::TrueColor);
        const QString wide = f.screen(80, 24);
        QVERIFY(wide.contains(QStringLiteral("▀")));   // the half-block logo
        QVERIFY(wide.contains(QStringLiteral("Listen")));
        QVERIFY(wide.contains(QStringLiteral("0.0.0.0:4001")));
        const QString w59 = f.screen(59, 24);
        QVERIFY(!w59.contains(QStringLiteral("▀")));    // no logo
        QVERIFY(!w59.contains(QStringLiteral("Listen")));    // no bind column
        QVERIFY(!w59.contains(QStringLiteral("0.0.0.0")));
        QVERIFY(w59.contains(QStringLiteral("Device")));
        QVERIFY(w59.startsWith(QStringLiteral("Aether Serial Bridge ·")));   // one-line header
        const QString w39 = f.screen(39, 20);
        QVERIFY(!w39.contains(QStringLiteral("Device")));    // name + state
        QVERIFY(!w39.contains(QStringLiteral("COM3")));
        QVERIFY(w39.contains(QStringLiteral("Amp")));
        const QString h11 = f.screen(80, 11);
        QVERIFY(!h11.contains(QString::fromLatin1(text::kTitle)));   // no header
        QVERIFY(h11.contains(QStringLiteral("Amp")));
        // Without UTF-8 the logo is the ASCII ramp, and the dots are words' companions *, o, !.
        Fixture a(QStringLiteral("installed"), ColorMode::TrueColor);
        auto st = a.app->state();
        st.utf8 = false;
        const std::vector<std::string> ascii = renderText(st, 80, 24);
        QCOMPARE(QString::fromStdString(ascii[0]).left(24), QString::fromStdString(logoAsciiRows()[0]));
    }

    void logoArt()
    {
        QCOMPARE(logoSize(ColorMode::TrueColor, true).width, 32);
        QCOMPARE(logoSize(ColorMode::TrueColor, true).height, 16);
        QCOMPARE(logoSize(ColorMode::Palette256, true).height, 16);
        QCOMPARE(logoSize(ColorMode::None, true).width, 24);
        QCOMPARE(logoSize(ColorMode::Basic16, true).height, 12);
        QCOMPARE(logoSize(ColorMode::TrueColor, false).width, 24);
        const std::vector<std::string> rows = logoAsciiRows();
        QCOMPARE(int(rows.size()), 12);
        for (const std::string& r : rows) {
            QCOMPARE(int(r.size()), 24);
            for (const char c : r) QVERIFY(c >= 0x20 && c < 0x7f);
        }
        QCOMPARE(nearest256(palette::kBg), 232);   // the quantiser the generator shares
    }

    // ---- operations ------------------------------------------------------------------------------------------------
    void saveAppliesAndClearsDirty()
    {
        Fixture f(QStringLiteral("installed"));
        f.key(" ");   // Amp off: nothing listens on every interface, so no exposure question
        QVERIFY(f.app->state().dirty);
        f.key("s");
        QVERIFY(f.app->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Result);
        QVERIFY2(f.app->state().resultTitle == QStringLiteral("Done"), qPrintable(f.app->state().resultLines.join('\n')));
        QVERIFY(!f.app->state().dirty);
        const std::optional<BridgeConfig> committed = fromJson(f.rig.file(QStringLiteral("config.json")), nullptr);
        QVERIFY(committed && committed->ports.size() == 1 && !committed->ports[0].enabled);
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(!f.screen().contains(QString::fromLatin1(text::kUnsaved)));
    }

    // INV-107 in process (the process twin is tst_TuiProcess::barrierDeadlineKeepsDirty).
    void barrierDeadlineKeepsDirty()
    {
        Fixture f(QStringLiteral("installed,barrier-deadline"));
        f.key(" ");
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Result);
        QCOMPARE(f.app->state().resultTitle, QStringLiteral("The bridge's state is not known"));
        QVERIFY(f.app->state().dirty);
        QVERIFY(!f.rig.has(QStringLiteral("config.pending.json")));
        f.key(Event::Return);
        QVERIFY(f.screen().contains(QString::fromLatin1(text::kUnsaved)));
        QVERIFY(!f.app->state().work.ports[0].enabled);   // the edit is still in the editor
        f.key("r");
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kReloadDirty));
        f.key("y");
        QVERIFY(f.app->state().work.ports[0].enabled);    // the reload shows the old committed config
        QVERIFY(!f.app->state().dirty);
    }

    void digestMismatchKeepsDirty()
    {
        const QByteArray n = toJson(BridgeConfig{});
        Result r;
        r.persisted = Persisted::Staged;
        r.persistedSha256 = sha256Hex(n).toLatin1();
        QVERIFY(TuiApp::clearsDirty(r, n));
        r.persisted = Persisted::Committed;
        QVERIFY(TuiApp::clearsDirty(r, n));
        r.persistedSha256 = sha256Hex(n + ' ').toLatin1();   // another revision's bytes
        QVERIFY(!TuiApp::clearsDirty(r, n));
        r.persistedSha256.clear();
        QVERIFY(!TuiApp::clearsDirty(r, n));
        r.persisted = Persisted::None;
        r.persistedSha256 = sha256Hex(n).toLatin1();
        QVERIFY(!TuiApp::clearsDirty(r, n));
    }

    // INV-118 in process: an outstanding operation refuses q, Ctrl+C and every other operation; the interim is a
    // status line; the final result arrives after the call returns.
    void busyRefusesQuitAndOperations()
    {
        Fixture f(QStringLiteral("installed,block=firewall:sync"));
        f.key(" ");
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(f.sc->blocked(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->state().interim.isEmpty(), 20000);
        QVERIFY(f.app->busy());
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(f.screen().contains(QStringLiteral("Applying")));
        for (const Event& e : {Event::Character('q'), Event::CtrlC, Event::CtrlC}) {
            f.key(e);
            QVERIFY(!f.app->quitRequested());
            QCOMPARE(f.app->state().message, QString::fromLatin1(text::kBusyQuit));
            QCOMPARE(f.app->state().screen, Screen::Ports);
        }
        for (const char* k : {"s", "t", "x", "r"}) {
            f.key(k);
            QCOMPARE(f.app->state().message, QString::fromLatin1(text::kBusyAction));
        }
        // Editing stays allowed, and that edit is not cleared by the result.
        f.key(Event::ArrowDown);
        f.key(" ");   // auto-start off
        f.sc->release();
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Result);
        QVERIFY(f.app->state().interim.isEmpty());
        QVERIFY(f.app->state().dirty);
        f.key(Event::Return);
        f.key("q");
        QCOMPARE(f.app->state().screen, Screen::Confirm);   // dirty: asks
    }

    void hangupWhileBusyQuitsAfterFinished()
    {
        Fixture f(QStringLiteral("installed,block=firewall:sync"));
        f.key(" ");
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(f.sc->blocked(), 20000);
        f.app->hangup();
        QVERIFY(!f.app->drawing());
        QVERIFY(!f.app->quitRequested());
        QTest::qWait(100);
        QVERIFY(!f.app->quitRequested());
        f.sc->release();
        QTRY_VERIFY_WITH_TIMEOUT(f.app->quitRequested(), 20000);
        QVERIFY(!f.app->busy());
    }

    // INV-35 in process (the process row is tst_TuiProcess::cancelChangesNothing).
    void confirmNoWritesNothing()
    {
        Fixture f(QStringLiteral("installed"));
        f.key("  ");   // off and on: dirty, Amp listens on 0.0.0.0 (Linux does not enforce its allowed clients)
        f.key("s");
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        QVERIFY(f.app->state().question.contains(QStringLiteral("every network interface")));
        f.key("n");
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QCOMPARE(f.app->state().message, QString::fromLatin1(text::kNothingChanged));
        QVERIFY(!f.app->busy());
        // The untick question, after the exposure one.
        f.key(Event::ArrowDown);
        f.key(" ");
        f.key("s");
        f.key("y");
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        QVERIFY(f.app->state().question.contains(QStringLiteral("auto-start")));
        f.key(Event::Escape);
        QVERIFY(!f.app->busy());
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join(QLatin1Char('\n'))));
    }

    void pendingIsTheCleanWorkingCopy()
    {
        Fixture f(QStringLiteral("installed,pending"));
        QVERIFY(f.app->state().pendingPresent);
        QVERIFY(!f.app->state().dirty);
        QCOMPARE(f.app->state().work.ports[0].comPort, QStringLiteral("COM4"));
        QVERIFY2(f.flat().contains(QString::fromLatin1(text::kPendingNote)), qPrintable(f.flat()));
        // Discard asks first; No changes nothing.
        f.key("D");
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kDiscardPending));
        f.key("n");
        QVERIFY(f.rig.has(QStringLiteral("config.pending.json")));
        f.key("D");
        f.key("y");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QVERIFY(!f.rig.has(QStringLiteral("config.pending.json")));
        QCOMPARE(f.app->state().work.ports[0].comPort, QStringLiteral("COM3"));
        QVERIFY(!f.app->state().pendingPresent);
    }

    void editFormValidates()
    {
        Fixture f(QStringLiteral("installed"));
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Edit);
        f.key(Event::Tab);
        f.key(Event::Tab);
        f.key(Event::Tab);   // Baud
        QCOMPARE(Field(f.app->state().form.focus), Field::Baud);
        for (int i = 0; i < 5; ++i) f.key(Event::Backspace);
        f.key("fast");
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Edit);
        QCOMPARE(f.app->state().form.error, QStringLiteral("Baud must be a whole number."));
        for (int i = 0; i < 4; ++i) f.key(Event::Backspace);
        f.key("9600");
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QCOMPARE(f.app->state().work.ports[0].baudRate, 9600);
        QVERIFY(f.app->state().dirty);
        // Esc with changes asks; No keeps the form.
        f.key(Event::Return);
        f.key("X");
        f.key(Event::Escape);
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kDiscardPort));
        f.key("n");
        QCOMPARE(f.app->state().screen, Screen::Edit);
        f.key(Event::Escape);
        f.key("y");
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QCOMPARE(f.app->state().work.ports[0].name, QStringLiteral("Amp"));
        // A new port.
        f.key("a");
        QCOMPARE(f.app->state().form.text[int(Field::TcpPort)], QStringLiteral("4002"));
        QCOMPARE(f.app->state().form.text[int(Field::Listen)], QStringLiteral("127.0.0.1"));
        f.key("Tuner");
        f.key(Event::Tab);
        f.key(Event::Tab);
        f.key("COM5");
        f.key(Event::Return);
        QCOMPARE(int(f.app->state().work.ports.size()), 2);
        QCOMPARE(f.app->state().selected, 1);
        f.key("d");
        QCOMPARE(int(f.app->state().work.ports.size()), 1);
        QVERIFY(f.changes().isEmpty());
    }

    void pagesListAndHelp()
    {
        Fixture f(QStringLiteral("installed"));
        f.key("p");
        QCOMPARE(f.app->state().screen, Screen::Page);
        QVERIFY(f.host.calls.contains(QStringLiteral("listPorts")));
        QVERIFY(f.screen().contains(QStringLiteral("(ports)")));
        f.key(Event::Escape);
        f.key("l");
        QVERIFY(f.screen().contains(QStringLiteral("journalctl -u aether-serial-bridge")));
        f.key("q");
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(!f.app->quitRequested());
        f.key("?");
        QVERIFY(f.screen().contains(QStringLiteral("Save & apply")));
        f.key(Event::PageDown);
        f.key(Event::Escape);
        QCOMPARE(f.app->state().screen, Screen::Ports);
    }

    // ---- round 1: input on an open form or question survives a screen transition --------------------------------
    // The result of an operation that finishes while a port form is open waits for the ports screen; the
    // form and what was typed into it stay.
    void resultWaitsForTheOpenForm()
    {
        Fixture f(QStringLiteral("installed,block=firewall:sync"));
        f.key(" ");
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(f.sc->blocked(), 20000);
        f.key("a");
        QCOMPARE(f.app->state().screen, Screen::Edit);
        f.key("Tuner");
        f.key(Event::CtrlC);   // refused while busy, said on the form itself
        QCOMPARE(f.app->state().screen, Screen::Edit);
        QVERIFY2(f.flat(80).contains(QString::fromLatin1(text::kBusyQuit)), qPrintable(f.flat(80)));
        f.sc->release();
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Edit);
        QCOMPARE(f.app->state().form.text[int(Field::Name)], QStringLiteral("Tuner"));
        QVERIFY(!f.app->state().resultPending.isEmpty());
        QVERIFY2(f.flat(80).contains(QStringLiteral("The result is shown when you are back on the ports screen")), qPrintable(f.flat(80)));
        f.key(Event::Tab);
        f.key(Event::Tab);
        f.key("COM5");
        f.key(Event::Return);   // accepted: back on the ports screen, where the result now shows
        QCOMPARE(f.app->state().screen, Screen::Result);
        QVERIFY(f.app->state().resultPending.isEmpty());
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QCOMPARE(int(f.app->state().work.ports.size()), 2);
        QCOMPARE(f.app->state().work.ports[1].name, QStringLiteral("Tuner"));
        QVERIFY(f.app->state().dirty);
    }

    // ... and an open question keeps its answer, and the form behind it.
    void resultWaitsForTheOpenQuestion()
    {
        Fixture f(QStringLiteral("installed,block=firewall:sync"));
        f.key(" ");
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(f.sc->blocked(), 20000);
        f.key(Event::Return);   // the port form
        f.key("X");
        f.key(Event::Escape);   // "Discard the changes to this port?"
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kDiscardPort));
        f.sc->release();
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kDiscardPort));
        QVERIFY(f.flat(80).contains(QStringLiteral("Finished:")));
        f.key("n");             // keep the form
        QCOMPARE(f.app->state().screen, Screen::Edit);
        QCOMPARE(f.app->state().form.text[int(Field::Name)], QStringLiteral("AmpX"));
        f.key(Event::Escape);
        f.key("y");             // discard it: back on the ports screen, the result shows
        QCOMPARE(f.app->state().screen, Screen::Result);
    }

    // Ctrl+C with unaccepted form changes asks, from a clean configuration, also behind the form's own
    // question; No restores the form intact.
    void ctrlCAsksForUnfinishedFormChanges()
    {
        Fixture f(QStringLiteral("installed"));
        QVERIFY(!f.app->state().dirty);
        f.key(Event::Return);
        f.key("X");
        f.key(Event::CtrlC);
        QVERIFY(!f.app->quitRequested());
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kQuitDirty));
        f.key("n");
        QCOMPARE(f.app->state().screen, Screen::Edit);
        QCOMPARE(f.app->state().form.text[int(Field::Name)], QStringLiteral("AmpX"));
        f.key(Event::Escape);   // the form's discard question, then Ctrl+C on it
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kDiscardPort));
        f.key(Event::CtrlC);
        QVERIFY(!f.app->quitRequested());
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kQuitDirty));
        f.key(Event::Escape);
        QCOMPARE(f.app->state().screen, Screen::Edit);
        QCOMPARE(f.app->state().form.text[int(Field::Name)], QStringLiteral("AmpX"));
        // A new port's form too.
        f.key(Event::Escape);
        f.key("y");
        f.key("a");
        f.key("New");
        f.key(Event::CtrlC);
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kQuitDirty));
        f.key("y");
        QVERIFY(f.app->quitRequested());
        // An untouched form quits at once.
        Fixture g(QStringLiteral("installed"));
        g.key(Event::Return);
        g.key(Event::CtrlC);
        QVERIFY(g.app->quitRequested());
    }

    // The exposure question follows the bind address's value, in every accepted spelling.
    void exposureByValue_data()
    {
        QTest::addColumn<QString>("bind");
        QTest::addColumn<bool>("asks");
        QTest::newRow("expanded v6") << QStringLiteral("0:0:0:0:0:0:0:0") << true;
        QTest::newRow("::") << QStringLiteral("::") << true;
        QTest::newRow("zero-padded v4") << QStringLiteral("0.00.000.0") << true;
        QTest::newRow("v4-mapped any") << QStringLiteral("::ffff:0.0.0.0") << true;
        QTest::newRow("loopback") << QStringLiteral("127.0.0.1") << false;
        QTest::newRow("loopback v6") << QStringLiteral("::1") << false;
    }
    void exposureByValue()
    {
        QFETCH(QString, bind);
        QFETCH(bool, asks);
        Fixture f(QStringLiteral("installed"));
        f.key(Event::Return);
        for (int i = 0; i < int(Field::Listen); ++i) f.key(Event::Tab);
        QCOMPARE(Field(f.app->state().form.focus), Field::Listen);
        for (int i = 0; i < 7; ++i) f.key(Event::Backspace);   // "0.0.0.0"
        f.key(bind.toLatin1().constData());
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QCOMPARE(f.app->state().work.ports[0].bindAddress, bind);
        f.key("s");
        if (asks) {
            QCOMPARE(f.app->state().screen, Screen::Confirm);
            QVERIFY(f.app->state().question.contains(QStringLiteral("every network interface")));
            f.key("n");
            QVERIFY(f.changes().isEmpty());
        } else {
            QVERIFY(f.app->state().screen != Screen::Confirm);
            QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        }
    }

    // The table scrolls so the selected port is drawn, however many banners the header stacks.
    void selectionStaysVisible()
    {
        TuiState s;
        s.elevated = false;
        s.readOnlyReason = QString::fromLatin1(text::kReadOnly);
        s.load = Load::Ok;
        s.pendingPresent = true;
        s.disabledNote = true;
        s.busy = true;
        s.busyStep = QStringLiteral("updating the firewall");
        s.interim = QStringLiteral("Windows is still processing a firewall change; the result says what state the bridge is in.");
        for (int i = 0; i < 9; ++i) {
            PortMapping p;
            p.name = QStringLiteral("P%1").arg(i);
            p.comPort = QStringLiteral("COM%1").arg(i + 1);
            p.bindAddress = QStringLiteral("127.0.0.1");
            p.tcpPort = 4001 + i;
            s.work.ports << p;
        }
        for (const auto& [w, h] : {std::pair{59, 16}, std::pair{59, 20}, std::pair{80, 24}, std::pair{40, 14}}) {
            for (int sel = 0; sel < 9; ++sel) {
                s.selected = sel;
                QStringList lines;
                for (const std::string& l : renderText(s, w, h)) lines << QString::fromStdString(l);
                const QRegularExpression row(QStringLiteral("^\\[x\\] P%1 ").arg(sel));
                QVERIFY2(!lines.filter(row).isEmpty(), qPrintable(QStringLiteral("%1x%2, P%3 not drawn:\n%4").arg(w).arg(h).arg(sel).arg(lines.join('\n'))));
            }
        }
    }

    // ---- the window's finding, applied to the TUI: an unknown auto-start never unticks ------------------------
    // detect() fails: auto-start is not known, Save & apply refuses (nothing is submitted, so nothing is removed or
    // masked); choosing it explicitly on its row lets the save go ahead as a Run.
    void unknownServiceNeverUnticks()
    {
        Fixture f(QStringLiteral("installed,detect-fails"));
        QVERIFY(!f.app->state().autoStartKnown);
        QVERIFY(f.screen().contains(QStringLiteral("[?] Start automatically")));
        f.key(" ");   // Amp off: an edit
        f.key("s");
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(!f.app->busy());
        QVERIFY2(f.app->state().message.startsWith(QStringLiteral("Auto-start is not known")), qPrintable(f.app->state().message));
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join(QLatin1Char('\n'))));
        // The explicit choice. Off first: the installation is not known, so the untick still asks.
        f.key(Event::ArrowDown);
        f.key(" ");
        f.key(" ");
        QVERIFY(f.app->state().autoStartKnown);
        QVERIFY(!f.app->state().autoStart);
        f.key("s");
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        QVERIFY(f.app->state().question.contains(QStringLiteral("Turning off auto-start")));
        f.key("n");
        QVERIFY(f.changes().isEmpty());
        // Then on.
        f.key(" ");
        QVERIFY(f.app->state().autoStartKnown);
        QVERIFY(f.app->state().autoStart);
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        for (const QString& e : f.rig.world.log.all()) {
            QVERIFY2(!e.startsWith(QLatin1String("service:uninstall")), qPrintable(e));
            QVERIFY2(!e.startsWith(QLatin1String("firewall:removeAll")), qPrintable(e));
        }
    }

    // The coordinator's case: detect() says Current, query() throws. The installation, and so auto-start, stay as
    // detected; Save & apply is a Run, never the untick.
    void queryFailsAfterDetectCurrentKeepsTheService()
    {
        Fixture f(QStringLiteral("installed,query-fails"));
        QVERIFY(f.app->state().installKnown);
        QVERIFY(f.app->state().installed);
        QVERIFY(f.app->state().autoStartKnown);
        QVERIFY(f.app->state().autoStart);
        QVERIFY(f.app->state().serviceLine.contains(QStringLiteral("state unknown")));
        f.key(" ");
        f.key("s");
        QCOMPARE(f.app->state().screen, Screen::Ports);   // no untick question
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        for (const QString& e : f.rig.world.log.all()) {
            QVERIFY2(!e.startsWith(QLatin1String("service:uninstall")), qPrintable(e));
            QVERIFY2(!e.startsWith(QLatin1String("firewall:removeAll")), qPrintable(e));
        }
    }

    // A start token that cannot be read or parsed is not "off".
    void unreadableActivationIsNotOff()
    {
        Fixture f(QStringLiteral("installed,token-corrupt"));
        QVERIFY(!f.app->state().autoStartKnown);
        QVERIFY(!f.app->state().disabledNote);
        f.key(" ");
        f.key("s");
        QVERIFY(f.app->state().message.startsWith(QStringLiteral("Auto-start is not known")));
        QVERIFY(f.changes().isEmpty());
        // Without the service (detect answers NotInstalled), "off" is known and saving does not ask.
        Fixture g(QStringLiteral("fresh"));
        QVERIFY(g.app->state().autoStartKnown);
        QVERIFY(!g.app->state().autoStart);
    }

    // ---- round 2 ----------------------------------------------------------------------------------------------------
    // After a save that persisted, the editor's evidence of the disk follows it while the edits made
    // during the run stay. The next Save & apply does not ask to "replace" the user's own pending file.
    void sourceFollowsAPersistedSave()
    {
        Fixture f(QStringLiteral("installed,block=firewall:sync"));
        f.key(" ");   // Amp off
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(f.sc->blocked(), 20000);
        f.key(Event::Return);   // an edit during the run: the port's baud
        for (int i = 0; i < 3; ++i) f.key(Event::Tab);
        f.key(Event::Backspace);
        f.key("1");
        f.key(Event::Return);
        QCOMPARE(f.app->state().screen, Screen::Ports);
        f.sc->release();
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QVERIFY(f.rig.has(QStringLiteral("config.pending.json")));   // staged before the firewall phase (R4)
        QVERIFY(f.app->state().dirty);                             // the edit made during the run
        QVERIFY(f.app->state().pendingPresent);                    // the evidence of the disk was refreshed
        f.key(Event::Return);   // leave the result
        f.key("s");
        QVERIFY2(f.app->state().question != QString::fromLatin1(text::kSupersede), "asked to replace its own pending file");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Result);
    }

    // The field being typed in scrolls so its end and the cursor stay visible.
    void longFieldKeepsItsEndVisible()
    {
        Fixture f(QStringLiteral("installed"));
        f.key(Event::Return);
        f.key(Event::Tab);
        f.key(Event::Tab);   // Device
        for (int i = 0; i < 4; ++i) f.key(Event::Backspace);
        const QString path = QStringLiteral("/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A10KQ7XB-if00-port0");
        f.key(path.toLatin1().constData());
        for (const int w : {40, 59, 80}) {
            const QString screen = f.screen(w, 20);
            QVERIFY2(screen.contains(QStringLiteral("if00-port0_")), qPrintable(screen));
        }
        f.key(Event::Backspace);
        QVERIFY(f.screen(40, 20).contains(QStringLiteral("if00-port_")));
        // Another field showing the long path shows its start.
        f.key(Event::Tab);
        QVERIFY(f.screen(40, 20).contains(QStringLiteral("/dev/serial/")));
    }

    // A question, a form or a result never loses the line that answers it, however small the screen.
    void answerLineSurvivesSmallScreens()
    {
        Fixture f(QStringLiteral("installed"));
        f.key("  ");
        f.key("s");   // the exposure question, about 3 lines at 40 columns
        for (const auto& [w, h] : {std::pair{40, 6}, std::pair{40, 8}, std::pair{30, 5}, std::pair{80, 5}}) {
            const QStringList lines = f.screen(w, h).split(QLatin1Char('\n'));
            QCOMPARE(lines.size(), h);
            QVERIFY2(lines.last().startsWith(QStringLiteral("y Yes")), qPrintable(lines.join('\n')));
            // What y would authorise is on the screen (the whole question, or its decision sentence).
            const QString flat = lines.join(' ').simplified();
            QVERIFY2(flat.contains(QStringLiteral("every network interface")) && flat.contains(QStringLiteral("anyway?")),
                     qPrintable(lines.join('\n')));
        }
        f.key("n");
        f.key(Event::Return);   // the port form
        for (const auto& [w, h] : {std::pair{40, 8}, std::pair{40, 12}}) {
            const QStringList lines = f.screen(w, h).split(QLatin1Char('\n'));
            QVERIFY2(lines.last().startsWith(QStringLiteral("Tab next")), qPrintable(lines.join('\n')));
        }
        f.key(Event::Escape);
        f.key(Event::Escape);
        f.key(" ");   // Amp off: no question
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Result);
        const QStringList lines = f.screen(30, 4).split(QLatin1Char('\n'));
        QVERIFY2(lines.last().startsWith(QStringLiteral("Enter continue")), qPrintable(lines.join('\n')));
    }

    // A wrapped page scrolls to its last line, and back up at once.
    void wrappedPageScrollsToTheEnd()
    {
        Fixture f(QStringLiteral("installed"));
        f.key("?");
        (void)f.app->render(40, 10);   // the size the page is drawn at
        const int max = pageMaxTop(f.app->state(), 40, 10);
        QVERIFY(max > int(f.app->state().pageLines.size()) - 8);   // the wrapped body is longer than the lines
        for (int i = 0; i < 20; ++i) f.key(Event::PageDown);
        QCOMPARE(f.app->state().pageTop, max);
        QVERIFY2(f.flat(40, 10).contains(QStringLiteral("Data folder:")), qPrintable(f.screen(40, 10)));
        f.key(Event::ArrowUp);
        QCOMPARE(f.app->state().pageTop, max - 1);
        for (int i = 0; i < 20; ++i) f.key(Event::PageUp);
        QCOMPARE(f.app->state().pageTop, 0);
    }

    // ---- follow-up --------------------------------------------------------------------------
    // The review's sequence: a clean editor with auto-start on, an external pending change, the supersede question open,
    // a poll whose detect() fails, Yes, and detect() answering again by the time the Applier would run. The decision
    // taken at Save (a Run) is checked against the state at Yes: auto-start is not known, so nothing is submitted;
    // nothing is unticked, removed or masked.
    void pollDuringSupersedeNeverUnticks()
    {
        Fixture f(QStringLiteral("installed"));
        QVERIFY(f.app->state().autoStartKnown && f.app->state().autoStart && !f.app->state().dirty);
        f.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
        f.key("s");
        QVERIFY(f.app->state().question.contains(QStringLiteral("every network interface")));
        f.key("y");   // auto-start is on: no untick question; the supersede question
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kSupersede));
        f.rig.service.fail[QStringLiteral("detect")] = QStringLiteral("the service manager did not answer");
        f.app->refreshService();   // the poll, while the question is open
        QVERIFY(!f.app->state().autoStartKnown);
        f.rig.service.fail.remove(QStringLiteral("detect"));
        f.key("y");
        QVERIFY(!f.app->busy());
        QVERIFY2(f.app->state().message.startsWith(QStringLiteral("Auto-start is not known")), qPrintable(f.app->state().message));
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join(QLatin1Char('\n'))));
        // The next poll reads it again; Save & apply then asks again and runs, never unticking.
        f.app->refreshService();
        QVERIFY(f.app->state().autoStartKnown && f.app->state().autoStart);
        f.key(Event::Escape);
        f.key("s");
        f.key("y");
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kSupersede));
        f.key("y");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        bool started = false;
        for (const QString& e : f.rig.world.log.all()) {
            QVERIFY2(!e.startsWith(QLatin1String("service:uninstall")), qPrintable(e));
            QVERIFY2(!e.startsWith(QLatin1String("firewall:removeAll")), qPrintable(e));
            started = started || e == QLatin1String("service:start");
        }
        QVERIFY(started);   // a Run
    }

    // A poll that changes the decision (known, but now different) starts the questions again, and the changed
    // decision is itself asked before anything is submitted.
    void changedDecisionAsksAgain()
    {
        Fixture f(QStringLiteral("installed"));
        f.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
        f.key("s");
        f.key("y");
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kSupersede));
        f.rig.service.installation.state = InstallState::NotInstalled;   // removed behind the TUI's back
        f.app->refreshService();
        QVERIFY(f.app->state().autoStartKnown);
        QVERIFY(!f.app->state().autoStart);
        f.key("y");
        QVERIFY(!f.app->busy());
        QVERIFY(f.changes().isEmpty());
        QCOMPARE(f.app->state().screen, Screen::Confirm);   // the questions again, from the top
        QVERIFY(f.app->state().message.contains(QStringLiteral("changed while you were answering")));
        QVERIFY(f.app->state().question.contains(QStringLiteral("every network interface")));
        f.key("y");
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kSupersede));
        f.key("y");
        QVERIFY(f.app->state().question.contains(QStringLiteral("will now save these settings and keep the bridge stopped")));
        f.key("n");
        QVERIFY(!f.app->busy());
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join(QLatin1Char('\n'))));
    }

    // Discard asks about a pending file; the core judges it at the answer: gone meanwhile, nothing to discard.
    // v1.1 INV-128: the TUI holds nothing in the data folder after any of its reads, as the window's view
    // (tst_BridgeWindow::nothingHeldWhenFinishedFires): its load and Reload (the installation not read, so the load's own
    // close is the one that counts), the activation read of a service refresh, the pending check of the Discard
    // question, bridge.log (Windows) and an operation with its result.
    void nothingHeldAfterARead_data()
    {
        QTest::addColumn<QString>("read");
        QTest::newRow("load and reload") << QStringLiteral("load");
        QTest::newRow("activation read") << QStringLiteral("activation");
        QTest::newRow("discard question") << QStringLiteral("discard");
        QTest::newRow("bridge.log") << QStringLiteral("log");
        QTest::newRow("save and its result") << QStringLiteral("save");
    }
    void nothingHeldAfterARead()
    {
        QFETCH(QString, read);
        const QString scenario = read == QLatin1String("load")      ? QStringLiteral("installed,detect-fails")
                                 : read == QLatin1String("discard") ? QStringLiteral("installed,pending")
                                                                    : QStringLiteral("installed");
        Fixture f(scenario, ColorMode::None, /*windows=*/read == QLatin1String("log"));
        QVERIFY2(!f.rig.folder.directoryOpen, "held after the load");
        if (read == QLatin1String("load")) {
            f.key("r");
        } else if (read == QLatin1String("activation")) {
            f.rig.world.log.clear();
            f.app->refreshService();
            QVERIFY(!f.rig.world.log.all().filter(QStringLiteral("folder:inspect")).isEmpty());   // the read happened
        } else if (read == QLatin1String("discard")) {
            f.key("D");
            QCOMPARE(f.app->state().question, QString::fromLatin1(text::kDiscardPending));
        } else if (read == QLatin1String("log")) {
            f.key("l");
            QCOMPARE(f.app->state().screen, Screen::Page);
        } else {
            f.key(" ");
            f.key("s");
            QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
            f.key(Event::Return);
            QCOMPARE(f.app->state().screen, Screen::Ports);
        }
        QVERIFY2(!f.rig.folder.directoryOpen, qPrintable(QStringLiteral("held after %1").arg(read)));
    }

    void discardRevalidatesThePendingFile()
    {
        Fixture f(QStringLiteral("installed,pending"));
        f.key("D");
        QCOMPARE(f.app->state().question, QString::fromLatin1(text::kDiscardPending));
        QString err;
        QVERIFY(f.rig.world.files.remove(f.rig.folder.handle(), QString::fromLatin1(ConfigFiles::kPending), &err));   // gone meanwhile
        f.key("y");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QVERIFY2(f.app->state().resultLines.join(QLatin1Char('\n')).contains(QStringLiteral("no saved changes to discard")),
                 qPrintable(f.app->state().resultLines.join(QLatin1Char('\n'))));
    }

    // Short terminals: the focused field is always drawn, and so is the line of keys.
    void focusedFieldAlwaysDrawn()
    {
        static const char* labels[kFieldCount] = {"Name", "Enabled", "Device", "Baud", "Format",
                                                  "Listen on", "TCP port", "Allowed", "Assert DTR", "Assert RTS"};
        for (const auto& [w, h] : {std::pair{40, 12}, std::pair{40, 8}, std::pair{40, 5}, std::pair{80, 24}, std::pair{30, 3}}) {
            Fixture f(QStringLiteral("installed"));
            f.key(Event::Return);
            for (int i = 0; i < kFieldCount; ++i) {
                QCOMPARE(f.app->state().form.focus, i);
                const QStringList lines = f.screen(w, h).split(QLatin1Char('\n'));
                QCOMPARE(lines.size(), h);
                QVERIFY2(!lines.filter(QRegularExpression(QStringLiteral("^%1 ").arg(QString::fromLatin1(labels[i])))).isEmpty(),
                         qPrintable(QStringLiteral("%1x%2, %3 not drawn:\n%4").arg(w).arg(h).arg(QString::fromLatin1(labels[i]), lines.join('\n'))));
                QVERIFY2(lines.last().startsWith(QStringLiteral("Tab next")), qPrintable(lines.join('\n')));
                f.key(Event::Tab);
            }
        }
    }

    // ---- review round 1 --------------------------------------------------------------------------------------------
    // Whenever y is live, the decision it would authorise is on the screen, at the smallest sizes too, for
    // every question that leads to an operation.
    // Whenever y is live, every decisive sentence of the question (the action and its consequences) is on the screen,
    // in the whole question or in its compact form; where neither fits, y is not offered and does nothing
    //. Linux and Windows wordings.
    void questionsShowTheirDecisionAtSmallSizes()
    {
        using Phrases = QStringList;
        auto check = [](Fixture& f, const Phrases& full, const Phrases& compact, const char* what) {
            QCOMPARE(f.app->state().screen, Screen::Confirm);
            for (const auto& [w, h] : {std::pair{30, 2}, std::pair{30, 5}, std::pair{40, 5}, std::pair{40, 8}, std::pair{80, 24}}) {
                const QStringList lines = f.screen(w, h).split(QLatin1Char('\n'));
                QStringList trimmed;
                for (const QString& l : lines) trimmed << l.trimmed();
                const QString flat = trimmed.join(' ').simplified();
                const bool live = !lines.filter(QRegularExpression(QStringLiteral("^y Yes"))).isEmpty();
                if (h >= 8) QVERIFY2(live, qPrintable(QStringLiteral("%1 at %2x%3: y not offered:\n%4").arg(QString::fromLatin1(what)).arg(w).arg(h).arg(lines.join('\n'))));
                if (live) {
                    auto all = [&flat](const Phrases& ps) {
                        for (const QString& p : ps)
                            if (!flat.contains(p)) return false;
                        return true;
                    };
                    QVERIFY2(all(full) || all(compact),
                             qPrintable(QStringLiteral("%1 at %2x%3: y is live but a decisive sentence is not shown:\n%4")
                                            .arg(QString::fromLatin1(what)).arg(w).arg(h).arg(lines.join('\n'))));
                } else {
                    QVERIFY2(lines.last().startsWith(QStringLiteral("Too small to answer")), qPrintable(lines.join('\n')));
                    // y does nothing at this size.
                    (void)f.app->render(w, h);
                    const QString question = f.app->state().question;
                    const QStringList before = f.changes();
                    f.key("y");
                    QCOMPARE(f.app->state().screen, Screen::Confirm);
                    QCOMPARE(f.app->state().question, question);
                    QCOMPARE(f.changes(), before);
                    (void)f.app->render(80, 24);
                }
            }
        };
        for (const bool windows : {false, true}) {
            // exposure, untick, supersede
            Fixture f(QStringLiteral("installed"), ColorMode::None, windows);
            f.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
            if (windows) {   // Windows asks only for a port with no allowed clients
                f.key(Event::Return);
                for (int i = 0; i < int(Field::Allowed); ++i) f.key(Event::Tab);
                for (int i = 0; i < 8; ++i) f.key(Event::Backspace);
                f.key(Event::Return);
            }
            f.key(Event::ArrowDown);
            f.key(" ");   // auto-start off
            f.key("s");
            if (windows)
                check(f,
                      {"Amp will listen on every network interface with no client restriction",
                       "Anyone who can reach this PC could operate that device", "Save anyway?"},
                      {"Amp: every network interface, no client restriction", "anyone who can reach this PC can use the device",
                       "Save anyway?"},
                      "exposure (Windows)");
            else
                check(f,
                      {"Amp will listen on every network interface", "Linux does not enforce allowed clients",
                       "Anyone who can reach this machine could operate that device", "Save anyway?"},
                      {"Amp: every network interface", "Linux ignores allowed clients", "anyone who can reach it can use the device",
                       "Save anyway?"},
                      "exposure (Linux)");
            f.key("y");
            if (windows)
                check(f, {"Turning off auto-start removes the service", "until you turn it back on", "Continue?"},
                      {"Turn off auto-start?", "The service is removed", "unbridged until you turn it back on"}, "untick (Windows)");
            else
                check(f, {"Turning off auto-start stops the bridge", "until you turn it back on", "Continue?"},
                      {"Turn off auto-start?", "The bridge stops", "unbridged until you turn it back on"}, "untick (Linux)");
            f.key("y");
            check(f, {"changed since they were loaded", "Replace them with yours?"},
                  {"Replace the saved, not applied settings that changed since they were loaded?"}, "supersede");
            f.key("n");
        }
        {   // the changed decision, after a restart
            Fixture f(QStringLiteral("installed"));
            f.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
            f.key("s");
            f.key("y");
            f.rig.service.installation.state = InstallState::NotInstalled;
            f.app->refreshService();
            f.key("y");   // restart: the exposure question again, with the notice below it if there is room
            check(f,
                  {"Amp will listen on every network interface", "Linux does not enforce allowed clients",
                   "Anyone who can reach this machine could operate that device", "Save anyway?"},
                  {"Amp: every network interface", "Linux ignores allowed clients", "anyone who can reach it can use the device",
                   "Save anyway?"},
                  "exposure after a restart");
            f.key("y");
            f.key("y");
            check(f, {"The service changed while you were answering", "will now save these settings and keep the bridge stopped", "Continue?"},
                  {"will now save these settings and keep the bridge stopped", "Continue?"}, "changed");
            QVERIFY(f.app->state().message.isEmpty());   // the question states the change; the notice is not repeated
            f.key("n");
        }
        {   // Repair
            Fixture f(QStringLiteral("installed"));
            f.key("R");
            check(f, {"stops the bridge", "moved aside", "nothing is deleted", "Continue?"},
                  {"Repair?", "The bridge stops", "moved aside", "nothing is deleted"}, "repair");
            f.key("n");
        }
        {   // Discard pending
            Fixture f(QStringLiteral("installed,pending"));
            f.key("D");
            check(f, {"Discard the saved, not applied settings", "The bridge keeps running the applied ones"},
                  {"Discard the saved, not applied settings", "The bridge keeps running the applied ones"}, "discard");
            f.key("n");
        }
    }

    // A Yes on "Save & apply will now ..." submits the new route (here Deactivate: nothing started).
    void changedDecisionYesSubmitsTheNewRoute()
    {
        Fixture f(QStringLiteral("installed"));
        f.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
        f.key("s");
        f.key("y");
        f.rig.service.installation.state = InstallState::NotInstalled;
        f.app->refreshService();
        f.key("y");   // restart
        f.key("y");   // exposure
        f.key("y");   // supersede
        QVERIFY(f.app->state().question.contains(QStringLiteral("will now save these settings and keep the bridge stopped")));
        f.key("y");
        QVERIFY(f.app->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QVERIFY(!f.changes().isEmpty());   // it ran
        for (const QString& e : f.rig.world.log.all()) {
            QVERIFY2(e != QLatin1String("service:start"), qPrintable(e));
            QVERIFY2(!e.startsWith(QLatin1String("service:uninstall")), qPrintable(e));
        }
        QVERIFY(f.rig.has(QStringLiteral("config.pending.json")));   // Deactivate stages the edits
    }

    // On a short form the field error outlasts the notices.
    void errorWinsOnShortForms()
    {
        Fixture f(QStringLiteral("installed,block=firewall:sync"));
        f.key(" ");
        f.key("s");
        QTRY_VERIFY_WITH_TIMEOUT(f.sc->blocked(), 20000);
        f.key(Event::Return);
        for (int i = 0; i < 3; ++i) f.key(Event::Tab);
        for (int i = 0; i < 5; ++i) f.key(Event::Backspace);
        f.key("fast");
        f.key(Event::Return);
        f.key(Event::CtrlC);   // a busy refusal: a notice on the form
        QVERIFY(!f.app->state().message.isEmpty());
        for (const auto& [w, h] : {std::pair{40, 4}, std::pair{40, 6}, std::pair{30, 5}}) {
            const QString screen = f.screen(w, h);
            QVERIFY2(screen.contains(QStringLiteral("Baud must be")), qPrintable(screen));
            QVERIFY2(screen.split('\n').last().startsWith(QStringLiteral("Tab next")), qPrintable(screen));
        }
        f.sc->release();
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
    }

    // INV-127: Discard asks about one pending file and passes its digest; the core keeps a different one at the
    // answer. An untrusted folder refuses. Kills inv127-discard-ignores-digest (with the Applier row) and a TUI that
    // submits the digest of the file as it is at the answer.
    void discardChecksDigest()
    {
        Fixture f(QStringLiteral("installed,pending"));
        f.key("D");
        f.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configC(), QStringLiteral("root"));
        f.key("y");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        const QString shown = f.app->state().resultLines.join(QLatin1Char('\n'));
        QVERIFY2(shown.contains(QStringLiteral("were replaced since")), qPrintable(shown));
        QCOMPARE(f.rig.file(QStringLiteral("config.pending.json")), configC());

        Fixture g(QStringLiteral("installed,pending"));
        g.key("D");
        g.rig.folder.owner = QStringLiteral("attacker");
        g.key("y");
        QVERIFY(!g.app->busy());
        QVERIFY2(g.app->state().message.startsWith(QString::fromLatin1(text::kUntrusted)), qPrintable(g.app->state().message));
    }

    // v1.1 debt (WS-14): the compact exposure question names at most two ports and counts the rest, so a dozen exposed
    // ports with long names still leave the decision on a 40 x 8 screen, and y is offered. The whole question keeps
    // every name.
    void compactExposureAnswerableAt40x8With12Ports_data() { routes(); }
    void compactExposureAnswerableAt40x8With12Ports()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed"), ColorMode::None, windows);
        f.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), exposedPorts(12, 24), QStringLiteral("root"));
        f.key("P");
        f.key("s");
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        const QString q = f.app->state().question;
        QVERIFY2(q.contains(QStringLiteral("every network interface")), qPrintable(q));
        for (int i = 1; i <= 12; ++i) QVERIFY2(q.contains(QStringLiteral("Port-%1-").arg(i, 2, 10, QLatin1Char('0'))), qPrintable(q));
        const QString compact = f.app->state().questionShort;
        QVERIFY2(compact.startsWith(QStringLiteral("12 ports (Port-01-")), qPrintable(compact));
        QVERIFY2(compact.contains(QStringLiteral(", +10 more)")), qPrintable(compact));
        QVERIFY2(!compact.contains(QStringLiteral("Port-03-")), qPrintable(compact));
        const QStringList lines = f.screen(40, 8).split(QLatin1Char('\n'));
        QStringList trimmed;
        for (const QString& l : lines) trimmed << l.trimmed();
        const QString flat = trimmed.join(' ').simplified();
        QVERIFY2(!lines.filter(QRegularExpression(QStringLiteral("^y Yes"))).isEmpty(), qPrintable(lines.join('\n')));
        const QStringList decisive = windows ? QStringList{"12 ports (", "+10 more)", "every network interface, no client restriction",
                                                           "anyone who can reach this PC can use the devices", "Save anyway?"}
                                             : QStringList{"12 ports (", "+10 more)", "every network interface",
                                                           "Linux ignores allowed clients", "anyone who can reach it can use the devices",
                                                           "Save anyway?"};
        for (const QString& p : decisive) QVERIFY2(flat.contains(p), qPrintable(p + QStringLiteral(" not shown:\n") + lines.join('\n')));
        (void)f.app->render(40, 8);
        f.key("y");   // answerable at this size: the save goes on
        QVERIFY(f.app->state().screen != Screen::Confirm || f.app->state().question != q);
    }

    // ---- no silent program replacement (the station outage of 2026-10-16) ----------------------------------------
    // A complete installation of another build: Save & apply asks before it replaces the program, and shows the
    // whole decision whenever y is live.
    void otherBuildAsksBeforeReplacing_data() { routes(); }
    void otherBuildAsksBeforeReplacing()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed,other-build"), ColorMode::None, windows);
        linuxRoute(f, windows);
        dismissUpdate(f);
        f.key(" ");   // Amp off: no exposure question
        f.key("s");
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        const QString q = f.app->state().question;
        QVERIFY2(q.contains(QStringLiteral("The installed bridge is a different build (program sha256 abcdef012345")), qPrintable(q));
        QVERIFY(q.contains(QStringLiteral("Update it to this build (")));
        if (!windows) {
            // Linux names both builds (D61): the installed program's stamp (none: before 1.1) and this digest.
            QVERIFY2(q.contains(QStringLiteral("(program sha256 abcdef012345; an earlier build, before 1.1)")), qPrintable(q));
            QVERIFY2(q.contains(QStringLiteral("Update it to this build (1.1.0, program sha256 0123456789ab)?")), qPrintable(q));
        }
        QVERIFY(f.changes().isEmpty());
        for (const auto& [w, h] : {std::pair{30, 2}, std::pair{30, 5}, std::pair{40, 5}, std::pair{40, 8}, std::pair{80, 24}}) {
            const QStringList lines = f.screen(w, h).split(QLatin1Char('\n'));
            QStringList trimmed;
            for (const QString& l : lines) trimmed << l.trimmed();
            const QString flat = trimmed.join(' ').simplified();
            const bool live = !lines.filter(QRegularExpression(QStringLiteral("^y Yes"))).isEmpty();
            if (h >= 8) QVERIFY2(live, qPrintable(lines.join('\n')));
            if (live) {
                const bool full = flat.contains(QStringLiteral("different build")) && flat.contains(QStringLiteral("Update it to this build")) &&
                                  flat.contains(QStringLiteral("The ports restart")) && flat.contains(QStringLiteral("No keeps the installed program"));
                const bool compact = flat.contains(QStringLiteral("Update the installed bridge (a different build)")) &&
                                     flat.contains(QStringLiteral("The ports restart")) && flat.contains(QStringLiteral("No keeps it"));
                QVERIFY2(full || compact, qPrintable(lines.join('\n')));
            } else {
                (void)f.app->render(w, h);
                f.key("y");
                QCOMPARE(f.app->state().question, q);   // y does nothing while the decision is not shown
                QVERIFY(f.changes().isEmpty());
                (void)f.app->render(80, 24);
            }
        }
    }

    // No (n) keeps the installed program and applies the settings to it.
    void otherBuildNoKeepsTheProgram_data() { routes(); }
    void otherBuildNoKeepsTheProgram()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed,other-build"), ColorMode::None, windows);
        linuxRoute(f, windows);
        dismissUpdate(f);
        f.key(" ");
        f.key("s");
        f.key("n");   // No: keep the installed build and apply
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        QCOMPARE(f.app->state().screen, Screen::Result);
        bool started = false;
        for (const QString& e : f.rig.world.log.all()) {
            QVERIFY2(!e.startsWith(QLatin1String("service:installOrConfigure")), qPrintable(e));
            started = started || e == QLatin1String("service:start");
        }
        QVERIFY2(started, qPrintable(f.app->state().resultLines.join('\n')));
        QVERIFY(f.rig.service.installation.otherBuild);   // still the installed build
    }

    // Esc always cancels the whole save; n, which acts here (keep and apply), is accepted only while
    // the question's decisive sentences are on the screen, exactly like y.
    void buildQuestionEscCancels_data() { routes(); }
    void buildQuestionEscCancels()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed,other-build"), ColorMode::None, windows);
        linuxRoute(f, windows);
        dismissUpdate(f);
        f.key(" ");
        f.key("s");
        QVERIFY(f.app->state().question.contains(QStringLiteral("different build")));
        QVERIFY(f.screen(80, 24).contains(QStringLiteral("Esc cancel")));
        f.key(Event::Escape);
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(!f.app->busy());
        QCOMPARE(f.app->state().message, QString::fromLatin1(text::kNothingChanged));
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join('\n')));
    }
    void buildQuestionHiddenOnlyEscWorks()
    {
        Fixture f(QStringLiteral("installed,other-build"));
        dismissUpdate(f);
        f.key(" ");
        f.key("s");
        const QString q = f.app->state().question;
        for (const auto& [w, h] : {std::pair{30, 2}, std::pair{30, 5}}) {
            (void)f.app->render(w, h);
            const QStringList lines = f.screen(w, h).split(QLatin1Char('\n'));
            QVERIFY2(lines.last().startsWith(QStringLiteral("Too small: Esc cancels")), qPrintable(lines.join('\n')));
            for (const char* k : {"n", "N", "y"}) {
                f.key(k);
                QCOMPARE(f.app->state().screen, Screen::Confirm);
                QCOMPARE(f.app->state().question, q);
                QVERIFY(f.changes().isEmpty());
            }
        }
        f.key(Event::Escape);   // still hidden: Esc cancels
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(!f.app->busy());
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join('\n')));
    }
    // The build the question names is part of the decision; another one found meanwhile restarts the questions.
    void anotherOtherBuildRestartsTheQuestions()
    {
        Fixture f(QStringLiteral("installed,other-build"));
        dismissUpdate(f);
        f.key(" ");
        f.key("s");
        QVERIFY(f.app->state().question.contains(QStringLiteral("abcdef012345")));
        f.rig.service.installation.installedSha256 = QStringLiteral("0123456789abcdef0123456789abcdef");
        f.app->refreshService();
        f.key("n");
        QVERIFY(!f.app->busy());
        QVERIFY(f.changes().isEmpty());
        QVERIFY(f.app->state().message.contains(QStringLiteral("changed while you were answering")));
        QVERIFY2(f.app->state().question.contains(QStringLiteral("0123456789ab")), qPrintable(f.app->state().question));
        f.key(Event::Escape);
        QVERIFY(f.changes().isEmpty());
    }

    // y (Update) installs this build.
    void otherBuildUpdateInstalls_data() { routes(); }
    void otherBuildUpdateInstalls()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed,other-build"), ColorMode::None, windows);
        linuxRoute(f, windows);
        dismissUpdate(f);
        f.key(" ");
        f.key("s");
        f.key("y");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        bool installed = false;
        for (const QString& e : f.rig.world.log.all()) installed = installed || e.startsWith(QLatin1String("service:installOrConfigure"));
        QVERIFY(installed);
        QVERIFY(!f.rig.service.installation.otherBuild);
    }

    // User report 2026-10-20: the window's Update (and its elevated relaunch, which asks as it opens) asks whether to
    // update another installed build; the TUI now asks on open, and on u.
    // The question names both builds and what updating does; n (and Esc) change nothing and the notice says how to
    // ask again (u); the service is not touched.
    void updateAskedOnOpenNoChangesNothing_data() { routes(); }
    void updateAskedOnOpenNoChangesNothing()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed,other-build"), ColorMode::None, windows);
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        const QString q = f.app->state().question;
        QVERIFY2(q.contains(QStringLiteral("The installed bridge is a different build. Installed: a different build "
                                           "(program sha256 abcdef012345). This program: ")),
                 qPrintable(q));
        QVERIFY2(q.contains(QStringLiteral("Update it to this build? Updating replaces the installed program with this one "
                                           "and keeps the settings. The bridge restarts, so connected clients are dropped.")),
                 qPrintable(q));
        QVERIFY2(f.screen(80, 24).contains(QStringLiteral("y Yes")), qPrintable(f.screen(80, 24)));
        QVERIFY2(f.screen(40, 20).contains(QStringLiteral("y Yes")), qPrintable(f.screen(40, 20)));
        const int calls = int(f.host.calls.size());
        f.key("n");
        QCOMPARE(f.app->state().screen, Screen::Ports);
        QVERIFY(!f.app->busy());
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join('\n')));
        QCOMPARE(int(f.host.calls.size()), calls);
        QVERIFY(f.rig.service.installation.otherBuild);
        // The notice stays, wrapped within the screen, with the key; u asks again.
        QVERIFY2(f.screen(80, 24).contains(QStringLiteral("Press u")) && f.screen(80, 24).contains(QStringLiteral("to update it to this one.")),
                 qPrintable(f.screen(80, 24)));
        for (const auto& [w, h] : {std::pair{59, 24}, std::pair{40, 20}})
            QVERIFY2(f.flat(w, h).contains(QString::fromLatin1(text::kOtherBuildNote)), qPrintable(f.screen(w, h)));
        f.key("u");
        QCOMPARE(f.app->state().question, q);
        f.key(Event::Escape);
        QVERIFY(f.changes().isEmpty());
        // Read-only: no question to answer; what updating needs instead, and Enter goes back.
        Fixture ro(QStringLiteral("installed,other-build,readonly"), ColorMode::None, windows);
        QCOMPARE(ro.app->state().screen, Screen::Result);
        const QString page = ro.flat(80, 24);
        // This program by its full path, quoted: a bare name would run the installed, other build.
        const QString command = windows ? QStringLiteral("\"C:\\Users\\op\\Downloads\\aether-serial-bridge.exe\" --tui")
                                        : QStringLiteral("sudo '/home/op/My Downloads/aether-serial-bridge-x86_64.AppImage' --tui");
        QVERIFY2(page.contains((windows ? QStringLiteral("open a terminal with Run as administrator and run %1.")
                                        : QStringLiteral("Updating needs root: run %1."))
                                   .arg(command)),
                 qPrintable(page));
        ro.key(Event::Return);
        QVERIFY2(ro.flat(59, 24).contains(command), qPrintable(ro.screen(59, 24)));   // the notice too
        QVERIFY2(ro.flat(40, 20).contains(QStringLiteral("To update it, run")), qPrintable(ro.screen(40, 20)));
        ro.key("u");
        QVERIFY(!page.contains(QStringLiteral("y Yes")));
        ro.key("y");
        ro.key(Event::Return);
        QCOMPARE(ro.app->state().screen, Screen::Ports);
        QVERIFY(ro.changes().isEmpty());
        // Not another build: no question, and u does nothing.
        Fixture same(QStringLiteral("installed"), ColorMode::None, windows);
        QCOMPARE(same.app->state().screen, Screen::Ports);
        same.key("u");
        QCOMPARE(same.app->state().screen, Screen::Ports);
    }

    // The AppImage's read-only TUI (the GUI executable, $APPIMAGE set): the update instruction is the window's own
    // terminal command for the job, sudo "<AppImage>" --install (INV-124), on the page, in the notice and in the banner;
    // never the AppImage's --tui, which AppRun hands to the window and so reopens this page. The GUI
    // executable alone names no command. Kills: elevatedCommand naming the GUI executable's own --tui; the banner
    // keeping the bare installed program; an empty command rendered as "run .".
    void appImageReadOnlyUpdateInstalls()
    {
#ifdef Q_OS_WIN
        QSKIP("Linux only: the GUI executable's TUI and the AppImage exist only there");
#else
        const auto appImage = [](TuiApp::Env& e) {
            e.readOnlyExecutable = true;
            e.appImage = true;
        };
        Fixture ro(QStringLiteral("installed,other-build,readonly"), ColorMode::None, false, {}, appImage);
        QCOMPARE(ro.app->state().screen, Screen::Result);
        const QString command =
            QStringLiteral("sudo \"/home/op/My Downloads/aether-serial-bridge-x86_64.AppImage\" --install");
        const QString page = ro.flat(80, 24);
        QVERIFY2(page.contains(QStringLiteral("Updating needs root: run %1.").arg(command)), qPrintable(page));
        QVERIFY2(!page.contains(QStringLiteral("--tui")), qPrintable(page));
        ro.key(Event::Return);
        QCOMPARE(ro.app->state().screen, Screen::Ports);
        const QString ports = ro.flat(59, 40);
        QVERIFY2(ports.contains(QString::fromLatin1(text::kOtherBuildNoteLinux).arg(command)), qPrintable(ports));
        QVERIFY2(ports.contains(QString::fromLatin1(text::kReadOnlyAppImage).arg(command)), qPrintable(ports));
        QVERIFY2(!ports.contains(QStringLiteral("AppImage\" --tui")) && !ports.contains(QStringLiteral("AppImage' --tui")),
                 qPrintable(ports));
        // The GUI executable with no headless program of this build beside it: the window's button, no command.
        Fixture alone(QStringLiteral("installed,other-build,readonly"), ColorMode::None, false, {},
                      [](TuiApp::Env& e) { e.readOnlyExecutable = true; });
        QVERIFY2(alone.flat(80, 24).contains(QString::fromLatin1(text::kUpdateWindowOnly)), qPrintable(alone.flat(80, 24)));
        alone.key(Event::Return);
        const QString alonePorts = alone.flat(59, 40);
        QVERIFY2(alonePorts.contains(QString::fromLatin1(text::kOtherBuildNoteWindowOnly)), qPrintable(alonePorts));
        QVERIFY2(alonePorts.contains(QString::fromLatin1(text::kReadOnlyGuiExecutableAlone)), qPrintable(alonePorts));
        QVERIFY2(!alonePorts.contains(QStringLiteral("sudo")), qPrintable(alonePorts));
        // Beside this build's headless program: that program's --tui, never the GUI executable's own path.
        const QString gui = QStringLiteral("/opt/asb/aether-serial-bridge-gui");
        const QString sibling = QStringLiteral("/opt/asb/aether-serial-bridge");
        Fixture beside(QStringLiteral("installed,other-build,readonly"), ColorMode::None, false, {},
                       [&](TuiApp::Env& e) {
                           e.readOnlyExecutable = true;
                           e.program = gui;
                           e.headlessProgram = sibling;
                       });
        const QString siblingCommand = QStringLiteral("sudo '%1' --tui").arg(sibling);
        const QString besidePage = beside.flat(80, 24);
        QVERIFY2(besidePage.contains(QStringLiteral("Updating needs root: run %1.").arg(siblingCommand)),
                 qPrintable(besidePage));
        QVERIFY2(!besidePage.contains(gui), qPrintable(besidePage));
        beside.key(Event::Return);
        const QString besidePorts = beside.flat(59, 40);
        QVERIFY2(besidePorts.contains(QString::fromLatin1(text::kOtherBuildNoteLinux).arg(siblingCommand)),
                 qPrintable(besidePorts));
        QVERIFY2(!besidePorts.contains(gui), qPrintable(besidePorts));
        // An inherited $APPIMAGE (or $APPDIR) is not this program: only a program under $APPDIR runs from it.
        const QString image = QStringLiteral("/home/op/a.AppImage");
        QCOMPARE(appImageOf(image, QStringLiteral("/tmp/.mount_abc"), QStringLiteral("/tmp/.mount_abc/usr/bin/aether-serial-bridge-gui")),
                 image);
        QCOMPARE(appImageOf(image, QString(), QStringLiteral("/opt/asb/aether-serial-bridge-gui")), QString());
        QCOMPARE(appImageOf(image, QStringLiteral("/tmp/.mount_abc"), QStringLiteral("/opt/asb/aether-serial-bridge-gui")),
                 QString());
        QCOMPARE(appImageOf(image, QStringLiteral("/tmp/.mount_ab"), QStringLiteral("/tmp/.mount_abc/usr/bin/x")), QString());
#endif
    }

    // y runs Save & apply's update, the same operation as s then y on its build question: the program is installed
    // (replaced) with the settings, Applying shows while it runs, then the result.
    void updateAskedOnOpenYesUpdates_data() { routes(); }
    void updateAskedOnOpenYesUpdates()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed,other-build"), ColorMode::None, windows);
        linuxRoute(f, windows);
        f.rig.world.log.clear();
        f.key(" ");   // ignored while the question is up
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        QVERIFY(f.changes().isEmpty());
        f.key("y");
        // Linux: Amp listens on every interface, so the exposure question still comes, as with s (Windows: Amp has
        // allowed clients, nothing more to ask).
        if (!windows) {
            QVERIFY2(f.app->state().question.contains(QStringLiteral("every network interface")), qPrintable(f.app->state().question));
            QVERIFY(!f.app->busy());
            f.key("y");
        }
        QVERIFY(f.app->busy());
        QVERIFY2(f.flat(80, 24).contains(QStringLiteral("Applying")), qPrintable(f.screen(80, 24)));
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        bool installed = false;
        for (const QString& e : f.rig.world.log.all()) installed = installed || e.startsWith(QLatin1String("service:installOrConfigure"));
        QVERIFY(installed);
        QVERIFY(!f.rig.service.installation.otherBuild);
        QCOMPARE(f.app->state().screen, Screen::Result);
        QCOMPARE(f.app->state().resultTitle, QStringLiteral("Done"));
    }

    // Y replaces only the build the update question named. Another one found while it was open
    // restarts the chain, which asks the build question about the new one; nothing is changed.
    void updateQuestionAnotherBuildRestarts()
    {
        Fixture f(QStringLiteral("installed,other-build"));
        dismissUpdate(f);
        f.key(" ");   // Amp off: no exposure question
        f.key("u");
        QVERIFY(f.app->state().question.contains(QStringLiteral("abcdef012345")));
        f.rig.service.installation.installedSha256 = QStringLiteral("0123456789abcdef0123456789abcdef");
        f.app->refreshService();
        f.key("y");
        QVERIFY(!f.app->busy());
        QVERIFY2(f.changes().isEmpty(), qPrintable(f.changes().join('\n')));
        QVERIFY(f.app->state().message.contains(QStringLiteral("changed while you were answering")));
        QVERIFY2(f.app->state().question.contains(QStringLiteral("0123456789ab")), qPrintable(f.app->state().question));
        f.key(Event::Escape);
        QVERIFY(f.changes().isEmpty());
    }

    // With unsaved edits the update saves and applies them too; the compact question says so, and
    // y is offered only while that is on the screen.
    void updateQuestionCompactNamesUnsavedChanges()
    {
        Fixture f(QStringLiteral("installed,other-build"));
        dismissUpdate(f);
        f.key(" ");   // an unsaved edit
        f.key("u");
        const QString full = f.flat(80, 24);
        QVERIFY2(full.contains(QStringLiteral("Your unsaved changes are saved and applied with it.")), qPrintable(full));
        const QString compact = f.flat(40, 9);
        QVERIFY2(compact.contains(QStringLiteral("y Yes")), qPrintable(f.screen(40, 9)));
        QVERIFY2(!compact.contains(QStringLiteral("Updating replaces")), qPrintable(f.screen(40, 9)));   // the compact form
        QVERIFY2(compact.contains(QStringLiteral("Your unsaved changes are saved and applied with it.")), qPrintable(f.screen(40, 9)));
        f.key(Event::Escape);
        QVERIFY(f.changes().isEmpty());
    }

    // The working copy is applied, so saved, not applied settings loaded from
    // config.pending.json are disclosed in both forms of the question, as unsaved edits are.
    void updateQuestionNamesPendingSettings()
    {
        Fixture f(QStringLiteral("installed,pending,other-build"));
        QCOMPARE(f.app->state().screen, Screen::Confirm);
        const QString sentence = QStringLiteral("Your saved, not yet applied changes are applied with it.");
        QVERIFY2(f.app->state().question.contains(sentence), qPrintable(f.app->state().question));
        QVERIFY2(f.app->state().questionShort.contains(sentence), qPrintable(f.app->state().questionShort));
        f.key(Event::Escape);
        f.key(" ");   // and an unsaved edit on top
        f.key("u");
        QVERIFY2(f.app->state().question.contains(QStringLiteral("Your saved, not yet applied changes and your unsaved "
                                                                  "changes are applied with it.")),
                 qPrintable(f.app->state().question));
        f.key(Event::Escape);
        QVERIFY(f.changes().isEmpty());
    }

    // An install that did not finish for another reason is finished without a question, as before.
    void unfinishedInstallIsFinishedWithoutAsking()
    {
        Fixture f(QStringLiteral("installed,unfinished-install"));
        f.key(" ");
        f.key("s");
        QVERIFY(f.app->state().screen != Screen::Confirm);
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        bool installed = false;
        for (const QString& e : f.rig.world.log.all()) installed = installed || e.startsWith(QLatin1String("service:installOrConfigure"));
        QVERIFY(installed);
    }

    // The build state is part of the captured decision: if a poll finds another build while a question is open, the
    // questions start again and the build question is asked; nothing is replaced unasked.
    void otherBuildChangeRestartsTheQuestions()
    {
        Fixture f(QStringLiteral("installed,unfinished-install"));
        f.key("s");   // Amp on every interface: the exposure question
        QVERIFY(f.app->state().question.contains(QStringLiteral("every network interface")));
        f.rig.service.installation.otherBuild = true;
        f.rig.service.installation.installedSha256 = QStringLiteral("abcdef0123456789abcdef0123456789");
        f.app->refreshService();
        f.key("y");
        QVERIFY(!f.app->busy());
        QVERIFY(f.changes().isEmpty());
        QVERIFY(f.app->state().message.contains(QStringLiteral("changed while you were answering")));
        f.key("y");   // the exposure question again
        QVERIFY2(f.app->state().question.contains(QStringLiteral("different build")), qPrintable(f.app->state().question));
        f.key("n");   // No: keep it
        QVERIFY(f.app->state().question.contains(QStringLiteral("will now save these settings, apply them and start the bridge")));
        f.key("n");
        QVERIFY(f.changes().isEmpty());
    }

    // Start never replaces the program: another build is started as it is (as the window does).
    void startStartsAnOtherBuildAsItIs()
    {
        Fixture f(QStringLiteral("installed,other-build"));
        dismissUpdate(f);
        f.key("x");
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        f.key(Event::Return);
        f.rig.world.log.clear();
        f.key("t");
        QVERIFY(f.app->state().screen != Screen::Confirm);
        QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
        bool started = false;
        for (const QString& e : f.rig.world.log.all()) {
            QVERIFY2(!e.startsWith(QLatin1String("service:installOrConfigure")), qPrintable(e));
            started = started || e == QLatin1String("service:start");
        }
        QVERIFY2(started, qPrintable(f.app->state().resultLines.join('\n')));
    }

    // ---- the help (owner request 2026-10-19: "we need some help in the tui about everything") ---------------------
    // The help names every field of the edit screen, with what it means, and the auto-start row, on both systems.
    // Kills: a field left out of the help's loop; a field without a hint or an explanation.
    void helpNamesEveryField_data() { routes(); }
    void helpNamesEveryField()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed"), ColorMode::None, windows);
        f.key("?");
        QCOMPARE(f.app->state().screen, Screen::Page);
        const QStringList lines = f.app->state().pageLines;
        for (int i = 0; i < kFieldCount; ++i) {
            const Field fld = Field(i);
            QVERIFY(!fieldLabel(fld).isEmpty());
            QVERIFY2(!fieldHint(fld, windows).isEmpty(), qPrintable(fieldLabel(fld)));
            const QString line = fieldLabel(fld) + QStringLiteral(": ") + fieldAbout(fld, windows);
            QVERIFY2(lines.contains(line), qPrintable(QStringLiteral("no help for %1").arg(fieldLabel(fld))));
            // Every field's explanation carries an example (e.g. ...), except the check boxes, which say what on and
            // off do.
            if (!isToggle(fld)) QVERIFY2(fieldAbout(fld, windows).contains(QStringLiteral("e.g. ")), qPrintable(fieldLabel(fld)));
        }
        // The auto-start row is not a form field: it has its own section.
        const int at = int(lines.indexOf(autoStartLabel(windows)));
        QVERIFY(at >= 0);
        QVERIFY(f.app->state().pageHeadings.contains(at));
        // The sections a user asked for.
        for (const char* h : {"What the bridge does", "Read-only or administrator", "Save & apply (s)", "Saved, not applied",
                              "Repair (R)", "Log (l) and serial ports (p)"})
            QVERIFY2(lines.contains(QString::fromLatin1(h)), h);
        QVERIFY(lines.contains(windows ? QStringLiteral("Anyone can look. To change anything, run aether-serial-bridge.exe "
                                                        "--tui from a terminal opened with Run as administrator.")
                                       : QStringLiteral("Anyone can look. To change anything, run sudo aether-serial-bridge --tui.")));
    }

    // Every key every screen answers is in the help's key table for that screen, and every row of the tables is on
    // the help page. The keys are found by pressing each one on a fresh TUI in each screen and seeing whether anything
    // changed, so a key bound later without a row here fails. Kills: a binding added (or a row dropped) without help.
    void helpNamesEveryKey()
    {
        struct Setup {
            const char* what;
            Screen screen;
            std::function<void(Fixture&)> go;
            bool typing = false;   // a text field: every character types (the table's "Typing" row)
            const char* scenario = "installed";
        };
        const std::vector<Setup> setups{
            {"ports", Screen::Ports, [](Fixture&) {}},
            // Dirty, on the auto-start row: Up and k move, r and P ask.
            {"ports (dirty, auto-start row)", Screen::Ports, [](Fixture& f) { f.key(" "); f.key("j"); }},
            // On a check box: typing does nothing there, so only the keys that act are found.
            {"edit (Enabled)", Screen::Edit, [](Fixture& f) { f.key(Event::Return); f.key(Event::Tab); }},
            {"edit (Name)", Screen::Edit, [](Fixture& f) { f.key(Event::Return); }, true},
            {"question", Screen::Confirm, [](Fixture& f) { f.key(" "); f.key("q"); }},
            {"result", Screen::Result, [](Fixture& f) {
                 f.key(" ");   // Amp off: nothing to ask about
                 f.key("s");
                 QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
             }},
            {"page (help)", Screen::Page, [](Fixture& f) { f.key("?"); }},
            {"page (scrolled)", Screen::Page, [](Fixture& f) { f.key("?"); f.key(Event::PageDown); }},
            // Read-only: the same keys answer, with the read-only sentence.
            {"ports (read-only)", Screen::Ports, [](Fixture&) {}, false, "installed,readonly"},
            // Read-only, every character typed in the form is answered with the reason (the Typing row), so the
            // characters are not probed one by one there; the other keys are, and they are seen by the footer too.
            {"edit (read-only, Enabled)", Screen::Edit, [](Fixture& f) { f.key(Event::Return); f.key(Event::Tab); }, true,
             "installed,readonly"},
            {"edit (read-only, Name)", Screen::Edit, [](Fixture& f) { f.key(Event::Return); }, true, "installed,readonly"},
        };
        struct Seen {
            Screen screen;
            QString text;
            bool quit;
            int calls;
            QStringList changes;
            bool busy;
            QString formError;   // a read-only key that only sets the form's footer
            bool operator==(const Seen&) const = default;
        };
        const auto seen = [](Fixture& f) {
            return Seen{f.app->state().screen, f.screen(80, 24), f.app->quitRequested(), int(f.host.calls.size()), f.changes(),
                        f.app->busy(), f.app->state().form.error};
        };
        const std::vector<Probe> all = probes();
        Fixture help(QStringLiteral("installed"));
        help.key("?");
        const QStringList page = help.app->state().pageLines;
        for (const Screen scr : {Screen::Ports, Screen::Edit, Screen::Confirm, Screen::Result, Screen::Page}) {
            for (const KeyHelp& k : keyHelp(scr)) {
                QVERIFY2(page.contains(k.shown + QStringLiteral(": ") + k.does), qPrintable(k.shown));
                for (const QString& name : k.keys)
                    QVERIFY2(std::any_of(all.begin(), all.end(), [&](const Probe& p) { return p.name == name; }), qPrintable(name));
            }
        }
        for (const Setup& s : setups) {
            QStringList table;
            for (const KeyHelp& k : keyHelp(s.screen)) table << k.keys;
            for (const Probe& p : all) {
                if (s.typing && p.event.is_character()) continue;
                Fixture f(QString::fromLatin1(s.scenario));
                s.go(f);
                QVERIFY2(f.app->state().screen == s.screen, s.what);
                // A footer already showing the reason would hide a key that sets it again.
                if (s.screen == Screen::Edit) QVERIFY2(f.app->state().form.error.isEmpty(), s.what);
                const Seen before = seen(f);
                f.key(p.event);
                if (seen(f) == before) continue;
                QVERIFY2(table.contains(p.name),
                         qPrintable(QStringLiteral("%1 does something on the %2 screen but the help does not name it")
                                        .arg(p.name, QString::fromLatin1(s.what))));
            }
        }
    }

    // The edit screen shows, under the form, the hint for the field being edited, and only that one; each of its lines
    // one line at 80 columns (one line in all, but two for Allowed on Windows: the window's example, then its
    // sentence), wrapped whole at 40; gone when the screen is too short for the form and its headings. Kills: the hint
    // of a fixed field; the hint taking a row the form needs.
    void editHintFollowsTheFocus_data() { routes(); }
    void editHintFollowsTheFocus()
    {
        QFETCH(bool, windows);
        Fixture f(QStringLiteral("installed"), ColorMode::None, windows);
        f.key(Event::Return);
        for (int i = 0; i < kFieldCount; ++i) {
            QCOMPARE(f.app->state().form.focus, i);
            const QStringList hint = fieldHint(Field(i), windows);
            QCOMPARE(int(hint.size()), windows && Field(i) == Field::Allowed ? 2 : 1);
            for (const QString& line : hint) {
                QCOMPARE(int(wrap(line.toStdString(), 80).size()), 1);
                QVERIFY2(f.screen(80, 24).contains(line), qPrintable(f.screen(80, 24)));   // a row of its own at 80
                QVERIFY2(f.flat(40, 20).contains(line), qPrintable(f.screen(40, 20)));
                // At 13 rows the form and its headings take every row but the keys: no hint, every field still drawn.
                QVERIFY(!f.flat(80, 13).contains(line));
            }
            for (int j = 0; j < kFieldCount; ++j)
                if (j != i)
                    for (const QString& other : fieldHint(Field(j), windows))
                        QVERIFY2(!f.flat(80, 24).contains(other), qPrintable(fieldLabel(Field(j))));
            // From there up the hint only takes what is left: every field, the headings and the keys stay.
            for (int h = 13; h <= 16; ++h) {
                const QString scr = f.screen(80, h);
                for (int j = 0; j < kFieldCount; ++j) QVERIFY2(scr.contains(fieldLabel(Field(j))), qPrintable(scr));
                QVERIFY2(scr.startsWith(QStringLiteral("Edit port")), qPrintable(scr));
                QVERIFY2(scr.section(QLatin1Char('\n'), -1).startsWith(QStringLiteral("Tab next")), qPrintable(scr));
                // h - 13 spare rows: a one-line hint, set off by a blank line from 15; the two-line hint cut at 14
                // (its first line, ended by an ellipsis) and whole from 15, without the blank line until 16.
                QCOMPARE(scr.contains(hint.first()), h >= 14);
                if (hint.size() == 2) QCOMPARE(scr.contains(hint.last()), h >= 15);
            }
            f.key(Event::Tab);
        }
    }

    // What the help says about Allowed matches the code: the example is a list the validator accepts, a keyword is
    // named only on Windows and is one the firewall accepts, and each system's sentence says what it does with it.
    // Kills: an example the field would refuse; a keyword on Linux; the Linux hint claiming enforcement.
    void allowedHelpIsTrue()
    {
        const QString example = QString::fromLatin1(help::kAllowedExample);
        QVERIFY(AllowedClients::check(example).isEmpty());
        QVERIFY(AllowedClients::tokens(example).has_value());
        QVERIFY(AllowedClients::check(QStringLiteral("192.0.2.10-192.0.2.20")).isEmpty());
        QVERIFY(fieldAbout(Field::Allowed, false).contains(QStringLiteral("192.0.2.10-192.0.2.20")));
        QVERIFY(std::find(kAcceptedFirewallKeywords.begin(), kAcceptedFirewallKeywords.end(),
                          std::string_view(help::kAllowedKeywordExample)) != kAcceptedFirewallKeywords.end());
        for (const bool windows : {false, true}) {
            // Both systems are explained, whichever this is; Windows in the window's own words (help::allowedEffect).
            QVERIFY(fieldAbout(Field::Allowed, windows).contains(example));
            QVERIFY(fieldAbout(Field::Allowed, windows).contains(help::allowedEffect(true)));
            QVERIFY(fieldAbout(Field::Allowed, windows).contains(QStringLiteral("On Linux the list is saved but not enforced")));
        }
        const QString linuxHint = fieldHint(Field::Allowed, false).join(QLatin1Char(' '));
        QVERIFY(linuxHint.contains(example));
        QVERIFY(linuxHint.contains(QStringLiteral("does not enforce")));
        QVERIFY(!linuxHint.contains(QString::fromLatin1(help::kAllowedKeywordExample)));
        // Nothing checks that Windows Firewall is on, and other allow rules add up. The Windows texts (the
        // TUI hint is the window's sentence) say what the rule admits while the firewall is on, never that the list
        // is enforced. With the entry example first, as the window's card shows it.
        QCOMPARE(fieldHint(Field::Allowed, true), QStringList({help::allowedExampleLine(true), help::allowedEffect(true)}));
        QVERIFY(help::allowedEffect(true).contains(QStringLiteral("While Windows Firewall is on")));
        for (const QString& t : {fieldHint(Field::Allowed, true).join(QLatin1Char(' ')), help::allowedEffect(true),
                                 help::allowedExampleLine(true)})
            QVERIFY2(!t.contains(QStringLiteral("enforce"), Qt::CaseInsensitive), qPrintable(t));
        QVERIFY(help::allowedEffect(false).contains(QStringLiteral("does not enforce")));
        QVERIFY(!help::allowedExampleLine(false).contains(QString::fromLatin1(help::kAllowedKeywordExample)));
        QVERIFY(help::allowedExampleLine(true).contains(QString::fromLatin1(help::kAllowedKeywordExample)));
        // Linux recommends the by-id name for the device.
        QVERIFY(fieldHint(Field::Device, false).join(QLatin1Char(' ')).contains(QStringLiteral("/dev/serial/by-id/")));
        // The Listen hint names this PC on Windows and this machine on Linux, as its explanation does.
        for (const bool windows : {false, true}) {
            const QString here = windows ? QStringLiteral("this PC") : QStringLiteral("this machine");
            const QString other = windows ? QStringLiteral("this machine") : QStringLiteral("this PC");
            const QString hint = fieldHint(Field::Listen, windows).join(QLatin1Char(' '));
            QVERIFY2(hint.contains(here) && !hint.contains(other), qPrintable(hint));
            QVERIFY(fieldAbout(Field::Listen, windows).contains(here));
        }
        QVERIFY(fieldAbout(Field::Device, false).contains(QStringLiteral("/dev/serial/by-id/")));
    }

    // Stop revokes start.token (Barrier B0), and a clean editor reads auto-start from it, so after x
    // auto-start reads off and s would take the untick's path; t starts the bridge again and brings the token back.
    // The help says exactly that: the x row and the auto-start section send the user to t, and to auto-start before s.
    // Kills: help-stop-row-claims-save-restarts (the x row offering Save & apply as a way to start again).
    void stopHelpMatchesWhatSaveDoesNext_data() { routes(); }
    void stopHelpMatchesWhatSaveDoesNext()
    {
        QFETCH(bool, windows);
        {
            Fixture f(QStringLiteral("installed"), ColorMode::None, windows);
            QVERIFY(f.app->state().autoStart);
            f.key("x");
            QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
            f.key(Event::Return);
            QCOMPARE(f.app->state().screen, Screen::Ports);
            QVERIFY(!f.app->state().dirty);
            QVERIFY(!f.app->state().autoStart);
            // s now asks to turn auto-start off (Windows: remove the service), it does not start the bridge.
            f.key("s");
            QCOMPARE(f.app->state().screen, Screen::Confirm);
            // Linux first asks about Amp listening on every interface with nothing enforcing Allowed.
            if (f.app->state().question.contains(QStringLiteral("every network interface"))) f.key("y");
            QVERIFY2(f.app->state().question.startsWith(QStringLiteral("Turning off auto-start")),
                     qPrintable(f.app->state().question));
            f.key(Event::Escape);
            // t starts it again, and auto-start reads on again.
            f.rig.world.log.clear();
            f.key("t");
            QTRY_VERIFY_WITH_TIMEOUT(!f.app->busy(), 20000);
            QVERIFY2(f.rig.world.log.all().contains(QStringLiteral("service:start")), qPrintable(f.app->state().resultLines.join('\n')));
            f.key(Event::Return);
            QVERIFY(f.app->state().autoStart);
        }
        const QList<KeyHelp> ports = keyHelp(Screen::Ports);
        const auto x = std::find_if(ports.begin(), ports.end(), [](const KeyHelp& k) { return k.keys == QStringList{QStringLiteral("x")}; });
        QVERIFY(x != ports.end());
        QVERIFY2(x->does.contains(QStringLiteral("turns auto-start off")), qPrintable(x->does));
        QVERIFY2(x->does.contains(QStringLiteral("t starts the bridge again")), qPrintable(x->does));
        QVERIFY2(x->does.contains(QStringLiteral("turn auto-start back on first")), qPrintable(x->does));
        QVERIFY2(!x->does.contains(QStringLiteral("t or Save & apply")), qPrintable(x->does));
        // The auto-start section and "Saved, not applied" say the same; no line offers s alone as a restart.
        const QStringList page = helpPage(windows, QString(), QString(), QString()).lines;
        const int at = int(page.indexOf(autoStartLabel(windows)));
        QVERIFY(at >= 0);
        QVERIFY2(page.mid(at).join(QLatin1Char(' ')).contains(QStringLiteral("Stop (x) and a Save & apply that did not finish turn it off too")),
                 qPrintable(page.mid(at, 4).join('\n')));
        for (const QString& l : page)
            QVERIFY2(!l.contains(QStringLiteral("starts again with t or Save & apply")) && !l.contains(QStringLiteral("s applies them, D")),
                     qPrintable(l));
    }

    // The Windows Allowed hint is the window's example line, whose middle dot is drawn as '-' on a
    // terminal without UTF-8, so the edit screen stays ASCII there.
    void allowedHintWithoutUtf8()
    {
        Fixture f(QStringLiteral("installed"), ColorMode::None, true);
        f.key(Event::Return);
        while (f.app->state().form.focus != int(Field::Allowed)) f.key(Event::Tab);
        auto st = f.app->state();
        st.utf8 = false;
        QString all;
        for (const std::string& l : renderText(st, 80, 24)) all += QString::fromStdString(l) + QLatin1Char('\n');
        for (const QChar c : all) QVERIFY2(c.unicode() < 0x80, qPrintable(all));
        QVERIFY2(all.contains(QString(help::allowedExampleLine(true)).replace(QChar(0x00B7), QLatin1Char('-'))), qPrintable(all));
        QVERIFY2(all.contains(help::allowedEffect(true)), qPrintable(all));
    }

    // The help reads the same without colour: the headings are bold, never coloured (INV-72), and without UTF-8 it is
    // all ASCII.
    void helpWithoutColourOrUtf8()
    {
        Fixture f(QStringLiteral("installed"));
        f.key("?");
        ftxui::Screen screen(80, 24);
        ftxui::Render(screen, render(f.app->state(), 80, 24));
        int bold = 0;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 80; ++x) {
                const ftxui::Cell& c = screen.CellAt(x, y);
                QCOMPARE(c.foreground_color, ftxui::Color::Default);
                QCOMPARE(c.background_color, ftxui::Color::Default);
                if (c.bold) ++bold;
            }
        QVERIFY(bold > 0);   // the title and the headings
        for (const QString& l : f.app->state().pageLines)
            for (const QChar c : l) QVERIFY2(c.unicode() < 0x80 || l.startsWith(QStringLiteral("Data folder")), qPrintable(l));
    }

    // ---- v1.1 WS-19: the documentation images (INV-135) ------------------------------------------------------------
    // Run by tools/docs_shots.py only (skipped otherwise): one frame per screen, from the demo fixture, as FTXUI prints
    // it (true colour, SGR codes), to <name>.ans, and beside it <name>.json with every line the frame shows.
    void docsShots()
    {
        const QString dir = qEnvironmentVariable("ASB_DOCS_SHOTS_DIR");
        if (dir.isEmpty()) QSKIP("ASB_DOCS_SHOTS_DIR not set (tools/docs_shots.py runs this)");
        const QString why = docs::refusal();
        QVERIFY2(why.isEmpty(), qPrintable(why));
#ifdef _WIN32
        const QString os = QStringLiteral("windows");
#else
        const QString os = qEnvironmentVariable("ASB_DOCS_SHOTS_OS") == QLatin1String("pi") ? QStringLiteral("pi") : QStringLiteral("linux");
#endif
        const bool windows = os == QLatin1String("windows");
        // The demo station: the fixture's config committed and running, its status, its installation.
        const auto demo = [&](const QString& installation, bool elevated) {
            return [=](Fixture& f) {
                f.rig.folder.state = FakeFolder::DirState::Directory;
                f.rig.installedRunning(docs::config(windows));
                f.rig.service.installation.flavor = windows ? Installation::Flavor::Windows : Installation::Flavor::Linux;
                docs::applyInstallation(f.rig.service.installation, installation);
                f.rig.service.status = docs::statusOver(f.rig.service.status, windows);
                f.host.isElevated = elevated;
                f.host.isWindows = windows;
                f.host.dataDir = docs::environment(windows).dataFolder;
                f.host.buildName = QStringLiteral("1.1.0, program sha256 fedcba987654");
                f.rig.world.log.clear();
            };
        };
        struct Frame {
            QString name, state, installation;
            bool elevated;
            QString keys;   // what the user types first
            int w, h;
        };
        QList<Frame> frames;
        if (os == QLatin1String("pi")) {
            frames << Frame{QStringLiteral("tui-read-only"), QStringLiteral("read-only (not root)"), QStringLiteral("current"), false, {}, 80, 24}
                   << Frame{QStringLiteral("tui-read-only-40x20"), QStringLiteral("read-only (not root), 40 x 20"), QStringLiteral("current"), false, {}, 40, 20};
        } else {
            frames << Frame{QStringLiteral("tui-ports"), QStringLiteral("running, one client connected"), QStringLiteral("current"), true, {}, 100, 30};
            if (!windows)
                frames << Frame{QStringLiteral("tui-exposure-question"), QStringLiteral("the exposure question on Save"), QStringLiteral("current"), true, QStringLiteral("  s"), 80, 24}
                       << Frame{QStringLiteral("tui-build-question"), QStringLiteral("the build question on Save"), QStringLiteral("other-build"), true, QStringLiteral("  sy"), 80, 24};
        }
        // The frame is printed as a true-colour terminal would get it (not a pipe's detected support), then restored.
        const ftxui::Terminal::Color detected = ftxui::Terminal::ColorSupport();
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
        const auto restore = qScopeGuard([detected] { ftxui::Terminal::SetColorSupport(detected); });
        for (const Frame& fr : frames) {
            Fixture f(QStringLiteral("fresh"), ColorMode::TrueColor, windows, demo(fr.installation, fr.elevated));
            f.app->refreshStatus();
            if (fr.installation == QLatin1String("other-build")) f.key(Event::Escape);   // the update question on open
            for (const QChar c : fr.keys) f.key(Event::Character(char(c.toLatin1())));
            if (fr.name == QLatin1String("tui-exposure-question") || fr.name == QLatin1String("tui-build-question"))
                QVERIFY2(f.app->state().screen == Screen::Confirm, qPrintable(f.screen(fr.w, fr.h)));
            if (fr.name == QLatin1String("tui-build-question"))
                QVERIFY2(f.app->state().question.contains(QStringLiteral("build")), qPrintable(f.app->state().question));
            ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(fr.w), ftxui::Dimension::Fixed(fr.h));
            ftxui::Render(screen, render(f.app->state(), fr.w, fr.h));
            QFile out(dir + QLatin1Char('/') + fr.name + QStringLiteral(".ans"));
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(QByteArray::fromStdString(screen.ToString()));
            out.close();
            docs::Shot shot{fr.name, os, fr.state, QStringLiteral("aethersdr.com palette"), 1, fr.w, fr.h, {}};
            for (const std::string& line : renderText(f.app->state(), fr.w, fr.h)) shot.texts << QString::fromStdString(line);
            QVERIFY(docs::writeManifest(dir, shot));
        }
    }

    void wrapping()
    {
        QCOMPARE(wrap("a bb ccc", 4), (std::vector<std::string>{"a bb", "ccc"}));
        QCOMPARE(wrap("abcdefgh", 3), (std::vector<std::string>{"abc", "def", "gh"}));
        QCOMPARE(wrap("", 5), (std::vector<std::string>{""}));
    }
};

QTEST_GUILESS_MAIN(tst_Tui)
#include "tst_Tui.moc"
