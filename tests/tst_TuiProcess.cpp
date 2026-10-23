// tst_TuiProcess (WS-12, Linux): the TUI as a process on a pseudo-terminal (master §8.4). The executables' TUI
// (aether-serial-bridge --tui, and the GUI executable without a display) where no service is needed, and
// asb_tui_testhost (the same tui::run on the fake service host, support/TuiScenario.h) for everything that drives an
// operation. Every child is killed when its test ends, so a mutant that hangs fails the test instead of ctest.
//
// Owned: INV-71 statusUpdatesWhileIdle, INV-72 noColorHasNoSgrColour (with its control). Integration rows run here:
// INV-107 barrierDeadlineKeepsDirty (WS-10), INV-118 closeRefusedWhileBusy and ctrlCAndQRefusedWhileBusy (WS-5),
// INV-35 cancelChangesNothing (WS-10), INV-1/32 listScreenOpensNothing (WS-4/WS-8; strace).

#include "core/ConfigJson.h"
#include "core/StatusStore.h"
#include "tui/TuiText.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <csignal>
#include <optional>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace asb;

namespace {

// Text without terminal control sequences (CSI, OSC, DCS, two-byte escapes); "\r\n" becomes "\n".
QString strip(const QByteArray& raw)
{
    static const QRegularExpression seq(QStringLiteral(
        "\\x1b\\[[0-?]*[ -/]*[@-~]|\\x1b\\][^\\x07\\x1b]*(?:\\x07|\\x1b\\\\)|\\x1bP[^\\x1b]*\\x1b\\\\|\\x1b[^\\[\\]P]"));
    QString s = QString::fromUtf8(raw);
    s.remove(seq);
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return s;
}

// Every SGR colour parameter in the stream (INV-72's list: 30-37, 38, 39, 40-47, 48, 49, 90-97, 100-107).
QStringList colourSgr(const QByteArray& raw, int* sgrCount = nullptr)
{
    static const QRegularExpression sgr(QStringLiteral("\\x1b\\[([0-9;:]*)m"));
    QStringList bad;
    int n = 0;
    auto it = sgr.globalMatch(QString::fromLatin1(raw));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        ++n;
        for (const QString& p : m.captured(1).split(QRegularExpression(QStringLiteral("[;:]")))) {
            bool ok = false;
            const int v = p.toInt(&ok);
            if (!ok) continue;
            if ((v >= 30 && v <= 39) || (v >= 40 && v <= 49) || (v >= 90 && v <= 97) || (v >= 100 && v <= 107)) {
                bad << m.captured(0).mid(1);
                break;
            }
        }
    }
    if (sgrCount) *sgrCount = n;
    return bad;
}

class Pty {
public:
    struct Spec {
        QString exe;
        QStringList args;
        QList<QPair<QByteArray, QByteArray>> env;   // set
        QList<QByteArray> unset;
        int cols = 80, rows = 24;
        QString straceTo;                           // run under strace -f, writing here
        void set(const QByteArray& k, const QByteArray& v) { env.append(qMakePair(k, v)); }
    };

    ~Pty()
    {
        if (m_pid > 0 && !m_exited) {
            ::kill(m_pid, SIGKILL);
            int st = 0;
            ::waitpid(m_pid, &st, 0);
        }
        if (m_fd >= 0) ::close(m_fd);
    }

    bool start(const Spec& s)
    {
        winsize ws{};
        ws.ws_col = static_cast<unsigned short>(s.cols);
        ws.ws_row = static_cast<unsigned short>(s.rows);
        std::vector<QByteArray> argv;
        if (!s.straceTo.isEmpty()) {
            const QString strace = QStandardPaths::findExecutable(QStringLiteral("strace"));
            if (strace.isEmpty()) return false;
            argv = {strace.toLocal8Bit(), "-f", "-qq", "-e", "trace=open,openat,openat2,creat", "-o", s.straceTo.toLocal8Bit()};
        }
        argv.push_back(s.exe.toLocal8Bit());
        for (const QString& a : s.args) argv.push_back(a.toLocal8Bit());
        std::vector<char*> cargv;
        for (QByteArray& a : argv) cargv.push_back(a.data());
        cargv.push_back(nullptr);
        const pid_t pid = ::forkpty(&m_fd, nullptr, nullptr, &ws);
        if (pid < 0) return false;
        if (pid == 0) {
            for (const QByteArray& u : s.unset) ::unsetenv(u.constData());
            for (const auto& [k, v] : s.env) ::setenv(k.constData(), v.constData(), 1);
            ::execv(cargv[0], cargv.data());
            ::_exit(127);
        }
        m_pid = pid;
        ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL) | O_NONBLOCK);
        return true;
    }

    void send(const QByteArray& b)
    {
        pump(0);
        (void)!::write(m_fd, b.constData(), size_t(b.size()));
    }
    // One key at a time, as a person types (FTXUI reads an escape alone as Esc only when nothing follows it).
    void keys(const QByteArray& b)
    {
        for (const char c : b) {
            send(QByteArray(1, c));
            pump(120);
        }
    }

    qsizetype mark()
    {
        pump(0);
        return m_out.size();
    }
    const QByteArray& raw() const { return m_out; }

    bool waitFor(const QString& text, int timeoutMs, qsizetype from = 0)
    {
        QElapsedTimer t;
        t.start();
        do {
            pump(20);
            if (strip(m_out.mid(from)).contains(text)) return true;
        } while (t.elapsed() < timeoutMs && !exited());
        pump(50);
        return strip(m_out.mid(from)).contains(text);
    }

    bool exited()
    {
        if (m_exited) return true;
        int st = 0;
        const pid_t r = ::waitpid(m_pid, &st, WNOHANG);
        if (r == m_pid) {
            m_exited = true;
            m_status = st;
        }
        return m_exited;
    }

    // The exit code, -signal when a signal ended it, nullopt when it did not end in time.
    std::optional<int> waitExit(int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        while (!exited() && t.elapsed() < timeoutMs) pump(20);
        pump(0);
        if (!m_exited) return std::nullopt;
        if (WIFEXITED(m_status)) return WEXITSTATUS(m_status);
        return -WTERMSIG(m_status);
    }

    void signal(int sig) { ::kill(m_pid, sig); }
    QString tail() const { return strip(m_out.right(6000)); }

    void pump(int ms)
    {
        pollfd p{m_fd, POLLIN, 0};
        int wait = ms;
        for (;;) {
            if (::poll(&p, 1, wait) <= 0) return;
            char buf[65536];
            const ssize_t n = ::read(m_fd, buf, sizeof buf);
            if (n <= 0) return;
            m_out.append(buf, n);
            wait = 0;
        }
    }

private:
    int m_fd = -1;
    pid_t m_pid = -1;
    bool m_exited = false;
    int m_status = 0;
    QByteArray m_out;
};

Pty::Spec baseSpec(const QString& exe)
{
    Pty::Spec s;
    s.exe = exe;
    s.env = {{"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"}, {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"}};
    s.unset = {"NO_COLOR", "DISPLAY", "WAYLAND_DISPLAY", "ASB_TEST_GUARD_PROBE"};
    return s;
}

QString headless()
{
    return qEnvironmentVariable("ASB_HEADLESS_EXE");
}
QString testhost()
{
    return qEnvironmentVariable("ASB_TUI_TESTHOST");
}

StatusReport amp(BridgeState state, const QString& client = {})
{
    StatusReport r;
    r.pid = 4000;
    PortStatus p;
    p.name = QStringLiteral("Amp");
    p.comPort = QStringLiteral("COM3");
    p.bindAddress = QStringLiteral("0.0.0.0");
    p.tcpPort = 4001;
    p.state = state;
    if (!client.isEmpty()) p.client = client;
    r.ports << p;
    return r;
}

bool writeStatus(const QString& path, const StatusReport& r)
{
    QSaveFile f(path);   // atomic: the TUI never reads half a file
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(StatusStore::toJson(r));
    return f.commit();
}

// What the fake service host recorded, minus reads (folder inspections and reads, service queries).
QStringList changesIn(const QString& logFile)
{
    QFile f(logFile);
    if (!f.open(QIODevice::ReadOnly)) return {QStringLiteral("(no event log)")};
    QStringList out;
    for (const QString& e : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (e.startsWith(QLatin1String("folder:inspect")) || e.startsWith(QLatin1String("folder:read")) ||
            e == QLatin1String("service:detect") || e == QLatin1String("service:query") || e.startsWith(QLatin1String("file ")))
            continue;
        out << e;
    }
    return out;
}

QString eventLog(const QString& logFile)
{
    QFile f(logFile);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// Quits a TUI that may ask about unsaved changes, and returns its exit code.
std::optional<int> quit(Pty& p)
{
    const qsizetype m = p.mark();
    p.keys("q");
    if (p.waitFor(QString::fromLatin1(tui::text::kQuitDirty), 1500, m)) p.keys("y");
    return p.waitExit(20000);
}

}  // namespace

class tst_TuiProcess : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(QFile::exists(headless()), "ASB_HEADLESS_EXE");
        QVERIFY2(QFile::exists(testhost()), "ASB_TUI_TESTHOST");
    }

    // ---- the executables' TUI -------------------------------------------------------------------------------------
    // No arguments on a terminal: the TUI (dispatch, WS-9 §4.10); q quits with 0.
    void headlessNoArgumentsOpensTheTui()
    {
        Pty p;
        QVERIFY(p.start(baseSpec(headless())));
        QVERIFY2(p.waitFor(QString::fromLatin1(tui::text::kTitle), 15000), qPrintable(p.tail()));
        QVERIFY2(p.waitFor(QStringLiteral("Start automatically at boot"), 5000), qPrintable(p.tail()));
        QCOMPARE(quit(p), std::optional<int>(0));
    }

    // Section 6.3: TERM=dumb (or no terminal) gets the help, exit 2.
    void dumbTerminalGetsHelp()
    {
        Pty::Spec s = baseSpec(headless());
        s.args = {QStringLiteral("--tui")};
        s.set("TERM", "dumb");
        Pty p;
        QVERIFY(p.start(s));
        QCOMPARE(p.waitExit(15000), std::optional<int>(2));
        QVERIFY2(strip(p.raw()).contains(QStringLiteral("Exit codes:")), qPrintable(p.tail()));
    }

    // The Linux GUI executable without a display: the TUI, read only (it never runs the Applier).
    void guiExecutableTuiIsReadOnly()
    {
        const QString gui = qEnvironmentVariable("ASB_GUI_EXE");
        if (gui.isEmpty()) QSKIP("the GUI executable is not part of this configuration");
        Pty p;
        QVERIFY(p.start(baseSpec(gui)));
        QVERIFY2(p.waitFor(QStringLiteral("this is the GUI executable"), 15000), qPrintable(p.tail()));
        p.keys(" ");
        QVERIFY2(p.waitFor(QStringLiteral("this is the GUI executable; run sudo"), 5000), qPrintable(p.tail()));
        QCOMPARE(quit(p), std::optional<int>(0));
    }

    // ---- INV-71 -----------------------------------------------------------------------------------------------------
    // Idle (no key pressed), the status poll on Qt's loop still redraws the screen: the loop is Qt's, and FTXUI is
    // only run once per timer tick (a blocking Loop::Run() would starve every Qt timer).
    void statusUpdatesWhileIdle()
    {
        QTemporaryDir dir;
        const QString status = dir.filePath(QStringLiteral("status.json"));
        QVERIFY(writeStatus(status, amp(BridgeState::Listening)));
        Pty::Spec s = baseSpec(testhost());
        s.set("ASB_TEST_STATUS_FILE", status.toLocal8Bit());
        Pty p;
        QVERIFY(p.start(s));
        QVERIFY2(p.waitFor(QStringLiteral("Listening"), 15000), qPrintable(p.tail()));
        const qsizetype m = p.mark();
        QVERIFY(writeStatus(status, amp(BridgeState::Connected, QStringLiteral("10.9.8.7:5555"))));
        QVERIFY2(p.waitFor(QStringLiteral("Connected · 10.9.8.7:5555"), 10000, m), qPrintable(p.tail()));
        QCOMPARE(quit(p), std::optional<int>(0));
    }

    // ---- INV-72 -----------------------------------------------------------------------------------------------------
    void noColorHasNoSgrColour_data() { screens(); }
    void noColorHasNoSgrColour()
    {
        QFETCH(QString, which);
        int sgr = 0;
        const QByteArray out = drive(which, true);
        QVERIFY(!out.isEmpty());
        const QStringList bad = colourSgr(out, &sgr);
        QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("colour SGR under NO_COLOR: %1").arg(bad.mid(0, 10).join(QLatin1Char(' ')))));
        QVERIFY2(sgr > 0, "no SGR at all: the stream was not read");   // bold / reverse remain
    }
    // The control: the same screens without NO_COLOR do emit colour (the scanner sees it).
    void colourWithoutNoColor_data() { screens(); }
    void colourWithoutNoColor()
    {
        QFETCH(QString, which);
        const QByteArray out = drive(which, false);
        QVERIFY(!colourSgr(out).isEmpty());
    }

    // ---- INV-107 (integration) ------------------------------------------------------------------------------------
    void barrierDeadlineKeepsDirty()
    {
        QTemporaryDir dir;
        const QString log = dir.filePath(QStringLiteral("events.log"));
        Pty::Spec s = baseSpec(testhost());
        s.set("ASB_TEST_SERVICE_HOST", "installed,barrier-deadline");
        s.set("ASB_TEST_EVENT_LOG", log.toLocal8Bit());
        Pty p;
        QVERIFY(p.start(s));
        QVERIFY2(p.waitFor(QStringLiteral("[x] Amp"), 15000), qPrintable(p.tail()));
        p.keys(" s");   // Amp off (no exposure question), Save & apply
        QVERIFY2(p.waitFor(QStringLiteral("The bridge's state is not known"), 20000), qPrintable(p.tail()));
        qsizetype m = p.mark();
        p.keys("\r");
        // The edit is still in the editor, still unsaved.
        QVERIFY2(p.waitFor(QString::fromLatin1(tui::text::kUnsaved), 5000, m), qPrintable(p.tail()));
        QVERIFY2(p.waitFor(QStringLiteral("[ ] Amp"), 5000, m), qPrintable(p.tail()));
        // A reload from disk shows the old committed config.
        m = p.mark();
        p.keys("r");
        QVERIFY2(p.waitFor(QString::fromLatin1(tui::text::kReloadDirty), 5000, m), qPrintable(p.tail()));
        m = p.mark();
        p.keys("y");
        QVERIFY2(p.waitFor(QStringLiteral("[x] Amp"), 5000, m), qPrintable(p.tail()));
        QCOMPARE(quit(p), std::optional<int>(0));
        const QString events = eventLog(log);
        QVERIFY2(events.contains(QStringLiteral("file config.json ")), qPrintable(events));
        QVERIFY2(!events.contains(QStringLiteral("file config.pending.json")), qPrintable(events));
        QVERIFY2(!events.contains(QStringLiteral("commitAs config.json")), qPrintable(events));
    }

    // ---- INV-118 (integration) ------------------------------------------------------------------------------------
    void closeRefusedWhileBusy() { busyRow(false); }
    void ctrlCAndQRefusedWhileBusy() { busyRow(true); }

    // SIGHUP while busy: the TUI stops drawing and exits after the final result (master §4.6).
    void sighupWhileBusyExitsAfterFinished()
    {
        QTemporaryDir dir;
        const QString release = dir.filePath(QStringLiteral("release"));
        const QString log = dir.filePath(QStringLiteral("events.log"));
        Pty::Spec s = baseSpec(testhost());
        s.set("ASB_TEST_SERVICE_HOST", "installed,block=firewall:sync");
        s.set("ASB_TEST_RELEASE", release.toLocal8Bit());
        s.set("ASB_TEST_EVENT_LOG", log.toLocal8Bit());
        Pty p;
        QVERIFY(p.start(s));
        QVERIFY2(p.waitFor(QStringLiteral("[x] Amp"), 15000), qPrintable(p.tail()));
        p.keys(" s");
        QVERIFY2(p.waitFor(QStringLiteral("is still processing"), 20000), qPrintable(p.tail()));
        p.signal(SIGHUP);
        QTest::qWait(1500);
        QVERIFY2(!p.exited(), "SIGHUP ended the TUI while its operation was outstanding");
        (void)QFile(release).open(QIODevice::WriteOnly);
        QCOMPARE(p.waitExit(30000), std::optional<int>(0));
        const QString events = eventLog(log);
        QVERIFY2(events.contains(QStringLiteral("firewall:sync")), qPrintable(events));
    }

    // ---- INV-35 (integration) -------------------------------------------------------------------------------------
    void cancelChangesNothing_data()
    {
        QTest::addColumn<QByteArray>("keys");
        QTest::addColumn<QString>("question");
        // Off and on again: dirty, Amp on every interface (Linux does not enforce its allowed clients); No.
        QTest::newRow("exposure") << QByteArray("  s") << QStringLiteral("every network interface");
        // Auto-start off: the exposure question Yes, the untick question No.
        QTest::newRow("untick") << QByteArray("j sy") << QStringLiteral("Turning off auto-start");
    }
    void cancelChangesNothing()
    {
        QFETCH(QByteArray, keys);
        QFETCH(QString, question);
        QTemporaryDir dir;
        const QString log = dir.filePath(QStringLiteral("events.log"));
        Pty::Spec s = baseSpec(testhost());
        s.set("ASB_TEST_EVENT_LOG", log.toLocal8Bit());
        Pty p;
        QVERIFY(p.start(s));
        QVERIFY2(p.waitFor(QStringLiteral("[x] Amp"), 15000), qPrintable(p.tail()));
        qsizetype m = p.mark();
        p.keys(keys);
        QVERIFY2(p.waitFor(question, 5000, m), qPrintable(p.tail()));
        m = p.mark();
        p.keys("n");
        QVERIFY2(p.waitFor(QString::fromLatin1(tui::text::kNothingChanged), 5000, m), qPrintable(p.tail()));
        QCOMPARE(quit(p), std::optional<int>(0));
        const QStringList changes = changesIn(log);
        QVERIFY2(changes.isEmpty(), qPrintable(QStringLiteral("changed after No: %1").arg(changes.join(QStringLiteral("; ")))));
    }

    // ---- INV-1 / INV-32 (integration) -----------------------------------------------------------------------------
    // The list screen names a pty-backed adapter by its stable and kernel names without opening it, its pts path or
    // any /proc fd alias of it.
    void listScreenOpensNothing()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("strace")).isEmpty())
            QFAIL("strace is not installed: this row needs it (INV-32)");
        int master = -1, slave = -1;
        char name[128] = {};
        QVERIFY(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
        const QString pts = QString::fromLocal8Bit(name);
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("dev"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("by-id"))));
        QVERIFY(QFile::link(pts, dir.filePath(QStringLiteral("dev/ttyUSB7"))));
        QVERIFY(QFile::link(dir.filePath(QStringLiteral("dev/ttyUSB7")), dir.filePath(QStringLiteral("by-id/usb-Test_Adapter-if00-port0"))));
        const QString trace = dir.filePath(QStringLiteral("tui.trace"));
        Pty::Spec s = baseSpec(testhost());
        s.set("ASB_TEST_SERIAL_BYID", dir.filePath(QStringLiteral("by-id")).toLocal8Bit());
        s.set("ASB_TEST_SERIAL_DEV", dir.filePath(QStringLiteral("dev")).toLocal8Bit());
        s.straceTo = trace;
        // LeakSanitizer cannot run under ptrace (the sanitizer leg): no leak check for the traced child.
        QByteArray asan = qgetenv("ASAN_OPTIONS");
        s.set("ASAN_OPTIONS", asan.isEmpty() ? QByteArray("detect_leaks=0") : asan + ":detect_leaks=0");
        Pty p;
        QVERIFY(p.start(s));
        QVERIFY2(p.waitFor(QString::fromLatin1(tui::text::kTitle), 30000), qPrintable(p.tail()));
        const qsizetype m = p.mark();
        p.keys("p");
        QVERIFY2(p.waitFor(QStringLiteral("ttyUSB7"), 10000, m), qPrintable(p.tail()));
        QVERIFY2(p.waitFor(QStringLiteral("usb-Test_Adapter-if00-port0"), 5000, m), qPrintable(p.tail()));
        // Esc, then wait for the ports screen: a q that follows a lone Esc too closely is read as Alt+q, and a slow
        // traced child on a loaded runner would then never see the quit (round-1 CI, no-qtgui).
        const qsizetype back = p.mark();
        p.keys("\x1b");
        QVERIFY2(p.waitFor(QStringLiteral("Start automatically"), 10000, back), qPrintable(p.tail()));
        QCOMPARE(quit(p), std::optional<int>(0));
        ::close(master);
        ::close(slave);
        QFile t(trace);
        QVERIFY(t.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(t.readAll()).split(QLatin1Char('\n'));
        QVERIFY2(lines.size() > 5, "the trace is empty");
        QStringList bad;
        for (const QString& l : lines)
            if (l.contains(pts) || l.contains(QLatin1String("ttyUSB7")) || l.contains(QLatin1String("usb-Test_Adapter")) ||
                (l.contains(QLatin1String("/proc/")) && l.contains(QLatin1String("/fd/"))))
                bad << l;
        QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("a device path was opened: %1").arg(bad.join(QLatin1Char('\n')))));
    }

private:
    void screens()
    {
        QTest::addColumn<QString>("which");
        QTest::newRow("headless") << QStringLiteral("headless");
        QTest::newRow("testhost") << QStringLiteral("testhost");
    }

    // Visits every screen of one TUI and returns everything it wrote.
    QByteArray drive(const QString& which, bool noColor)
    {
        Pty::Spec s = baseSpec(which == QLatin1String("headless") ? headless() : testhost());
        if (which == QLatin1String("headless")) s.args = {QStringLiteral("--tui")};
        else s.set("ASB_TEST_SERVICE_HOST", "installed,pending");
        if (noColor) s.set("NO_COLOR", "1");
        Pty p;
        if (!p.start(s)) return {};
        if (!p.waitFor(QString::fromLatin1(tui::text::kTitle), 15000)) return {};
        auto visit = [&p](const QByteArray& keys, const QString& expect) {
            const qsizetype m = p.mark();
            p.keys(keys);
            return p.waitFor(expect, 5000, m);
        };
        bool ok = visit("?", QStringLiteral("Help")) && visit("\x1b", QStringLiteral("Start automatically")) &&
                  visit("p", QStringLiteral("Serial ports")) && visit("\x1b", QStringLiteral("Start automatically")) &&
                  visit("l", QStringLiteral("Latest status")) && visit("\x1b", QStringLiteral("Start automatically"));
        if (which == QLatin1String("testhost")) {
            ok = ok && visit("\r", QStringLiteral("Edit port")) && visit("\x1b", QStringLiteral("Start automatically")) &&
                 visit("  ", QString::fromLatin1(tui::text::kUnsaved)) && visit("s", QStringLiteral("Confirm")) &&
                 visit("n", QString::fromLatin1(tui::text::kNothingChanged));
        }
        if (!ok) qWarning().noquote() << p.tail();
        if (quit(p) != std::optional<int>(0) || !ok) return {};
        return p.raw();
    }

    void busyRow(bool ctrlC)
    {
        QTemporaryDir dir;
        const QString release = dir.filePath(QStringLiteral("release"));
        Pty::Spec s = baseSpec(testhost());
        s.set("ASB_TEST_SERVICE_HOST", "installed,block=firewall:sync");
        s.set("ASB_TEST_RELEASE", release.toLocal8Bit());
        Pty p;
        QVERIFY(p.start(s));
        QVERIFY2(p.waitFor(QStringLiteral("[x] Amp"), 15000), qPrintable(p.tail()));
        p.keys(" s");
        QVERIFY2(p.waitFor(QStringLiteral("Applying"), 20000), qPrintable(p.tail()));
        // The interim notification is a status line; the operation is still outstanding.
        QVERIFY2(p.waitFor(QStringLiteral("is still processing"), 20000), qPrintable(p.tail()));
        const QList<QByteArray> attempts = ctrlC ? QList<QByteArray>{"\x03", "\x03", "q"} : QList<QByteArray>{"q", "q"};
        for (const QByteArray& k : attempts) {
            const qsizetype m = p.mark();
            p.keys(k);
            QVERIFY2(p.waitFor(QString::fromLatin1(tui::text::kBusyQuit), 5000, m), qPrintable(p.tail()));
            QTest::qWait(300);
            QVERIFY2(!p.exited(), "the TUI quit while an operation was outstanding");
        }
        if (ctrlC) {
            // A real SIGINT (not the key) is refused the same way.
            const qsizetype m = p.mark();
            p.signal(SIGINT);
            QVERIFY2(p.waitFor(QString::fromLatin1(tui::text::kBusyQuit), 5000, m), qPrintable(p.tail()));
            QTest::qWait(300);
            QVERIFY(!p.exited());
        }
        const qsizetype m = p.mark();
        (void)QFile(release).open(QIODevice::WriteOnly);
        QVERIFY2(p.waitFor(QStringLiteral("Enter continue"), 30000, m), qPrintable(p.tail()));
        QVERIFY(!p.exited());
        p.keys("\r");
        QCOMPARE(quit(p), std::optional<int>(0));
    }
};

QTEST_GUILESS_MAIN(tst_TuiProcess)
#include "tst_TuiProcess.moc"
