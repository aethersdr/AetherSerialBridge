#include "cli/CommandLine.h"

#include <array>
#include <utility>

namespace asb::cli {
namespace {

struct Name {
    const char* text;
    Command command;
};

constexpr std::array<Name, 24> kCommands = {{
    {"--help", Command::Help},
    {"-h", Command::Help},
    {"/?", Command::Help},
    {"--version", Command::Version},
    {"--gui", Command::Gui},
    {"--tui", Command::Tui},
    {"--install", Command::Install},
    {"--apply", Command::Apply},
    {"--start", Command::Start},
    {"--stop", Command::Stop},
    {"--uninstall", Command::Uninstall},
    {"--repair", Command::Repair},
    {"--status", Command::Status},
    {"--show-config", Command::ShowConfig},
    {"--list-ports", Command::ListPorts},
    {"--run", Command::Run},
    {"--service", Command::Service},
    {"--add-port", Command::AddPort},
    {"--remove-port", Command::RemovePort},
    {"--set-enabled", Command::SetEnabled},
    {"--discard-pending", Command::DiscardPending},
    {"--helper", Command::Helper},
    {"--self-test-init", Command::SelfTestInit},
    {"--uninstall-from-apps", Command::UninstallFromApps},
}};

// ASCII lower case only (a non-ASCII letter is never part of an option name).
QString lowerAscii(const QString& s)
{
    QString r = s;
    for (QChar& c : r)
        if (c.unicode() >= 'A' && c.unicode() <= 'Z') c = QChar(c.unicode() + ('a' - 'A'));
    return r;
}

std::optional<Command> commandFor(const QString& arg)
{
    const QString a = lowerAscii(arg);
    for (const Name& n : kCommands)
        if (a == QLatin1String(n.text)) return n.command;
    return std::nullopt;
}

// Digits only, no sign, no spaces, no overflow.
std::optional<int> number(const QString& s)
{
    if (s.isEmpty() || s.size() > 9) return std::nullopt;
    int n = 0;
    for (const QChar c : s) {
        if (c.unicode() < '0' || c.unicode() > '9') return std::nullopt;
        n = n * 10 + (c.unicode() - '0');
    }
    return n;
}

std::optional<bool> onOff(const QString& s)
{
    const QString v = lowerAscii(s);
    if (v == QLatin1String("on") || v == QLatin1String("true") || v == QLatin1String("yes") || v == QLatin1String("1"))
        return true;
    if (v == QLatin1String("off") || v == QLatin1String("false") || v == QLatin1String("no") || v == QLatin1String("0"))
        return false;
    return std::nullopt;
}

bool takesNothingElse(Command c)
{
    switch (c) {
    case Command::Help:
    case Command::Version:
    case Command::Gui:
    case Command::Tui:
    case Command::Service:
    case Command::Helper:
    case Command::SelfTestInit:
        return true;
    default:
        return false;
    }
}

bool takesConfig(Command c)
{
    switch (c) {
    case Command::Run:
    case Command::Status:
    case Command::ShowConfig:
    case Command::AddPort:
    case Command::RemovePort:
    case Command::SetEnabled:
        return true;
    default:
        return false;
    }
}

class Parser {
public:
    explicit Parser(const QStringList& args) : m_args(args) {}

    Parsed run()
    {
        for (m_i = 0; m_i < m_args.size() && m_p.ok(); ++m_i) step();
        if (m_p.ok()) finish();
        if (!m_p.ok()) {
            Parsed bad;
            bad.usageError = m_p.usageError;
            return bad;
        }
        return m_p;
    }

private:
    void fail(QString why)
    {
        if (m_p.ok()) m_p.usageError = std::move(why);
    }

    // The next argument as the value of `what`; never one that starts with "--".
    std::optional<QString> value(const QString& owner, const QString& what)
    {
        if (m_i + 1 >= m_args.size() || m_args[m_i + 1].startsWith(QLatin1String("--"))) {
            fail(QStringLiteral("%1 needs %2").arg(owner, what));
            return std::nullopt;
        }
        return m_args[++m_i];
    }

    bool once(bool& seen, const QString& name)
    {
        if (seen) {
            fail(QStringLiteral("%1 was given twice").arg(name));
            return false;
        }
        seen = true;
        return true;
    }

    void step()
    {
        const QString arg = m_args[m_i];
        const QString lower = lowerAscii(arg);
        if (const std::optional<Command> c = commandFor(arg)) {
            if (m_p.command != Command::None) {
                fail(m_p.command == *c ? QStringLiteral("%1 was given twice").arg(commandName(*c))
                                       : QStringLiteral("%1 and %2 cannot be combined; give one command")
                                             .arg(commandName(m_p.command), commandName(*c)));
                return;
            }
            m_p.command = *c;
            positionals();
            return;
        }
        if (lower == QLatin1String("--force-recreate")) {
            if (once(m_seenForce, lower)) m_p.forceRecreate = true;
            return;
        }
        if (lower == QLatin1String("--yes")) {
            if (once(m_seenYes, lower)) m_p.yes = true;
            return;
        }
        if (lower == QLatin1String("--remove-settings")) {
            if (once(m_seenRemoveSettings, lower)) m_p.removeSettings = true;
            return;
        }
        if (lower == QLatin1String("--confirmed")) {
            if (once(m_seenConfirmed, lower)) m_p.confirmed = true;
            return;
        }
        if (lower.startsWith(QLatin1String("--intent="))) {
            if (!once(m_seenIntent, QStringLiteral("--intent"))) return;
            const QString word = lower.mid(9);
            if (word != QLatin1String("install") && word != QLatin1String("update") && word != QLatin1String("uninstall") &&
                word != QLatin1String("none"))
                return fail(QStringLiteral("--intent takes install, update, uninstall or none, not '%1'").arg(arg.mid(9)));
            m_p.intent = word;
            return;
        }
        if (lower == QLatin1String("--config")) {
            if (!once(m_seenConfig, lower)) return;
            if (const auto v = value(QStringLiteral("--config"), QStringLiteral("a file path"))) m_p.config = *v;
            return;
        }
        if (lower == QLatin1String("--baud")) {
            if (!once(m_seenBaud, lower)) return;
            const auto v = value(lower, QStringLiteral("a baud rate"));
            if (!v) return;
            const std::optional<int> n = number(*v);
            if (!n) return fail(QStringLiteral("'%1' is not a valid baud rate").arg(*v));
            m_p.addPort.baud = *n;
            return;
        }
        if (lower == QLatin1String("--format")) {
            if (!once(m_seenFormat, lower)) return;
            if (const auto v = value(lower, QStringLiteral("a format such as 8N1"))) m_p.addPort.format = *v;
            return;
        }
        if (lower == QLatin1String("--bind")) {
            if (!once(m_seenBind, lower)) return;
            if (const auto v = value(lower, QStringLiteral("an IP address"))) m_p.addPort.bind = *v;
            return;
        }
        if (lower == QLatin1String("--allow")) {
            if (!once(m_seenAllow, lower)) return;
            if (const auto v = value(lower, QStringLiteral("a list of addresses"))) m_p.addPort.allow = *v;
            return;
        }
        if (arg.startsWith(QLatin1Char('-')) || arg.startsWith(QLatin1Char('/')))
            fail(QStringLiteral("unknown option '%1'").arg(arg));
        else
            fail(QStringLiteral("unexpected argument '%1'").arg(arg));
    }

    // The values a command takes right after its name.
    void positionals()
    {
        const QString name = commandName(m_p.command);
        switch (m_p.command) {
        case Command::AddPort: {
            const auto n = value(name, QStringLiteral("a port name, a device and a TCP port"));
            if (!n) return;
            const auto d = value(name, QStringLiteral("a device after the port name"));
            if (!d) return;
            const auto t = value(name, QStringLiteral("a TCP port after the device"));
            if (!t) return;
            const std::optional<int> port = number(*t);
            if (!port) return fail(QStringLiteral("'%1' is not a valid TCP port").arg(*t));
            m_p.addPort.name = *n;
            m_p.addPort.device = *d;
            m_p.addPort.tcpPort = *port;
            return;
        }
        case Command::RemovePort:
            if (const auto n = value(name, QStringLiteral("a port name"))) m_p.portName = *n;
            return;
        case Command::SetEnabled: {
            const auto n = value(name, QStringLiteral("a port name"));
            if (!n) return;
            const auto v = value(name, QStringLiteral("on or off after the port name"));
            if (!v) return;
            const std::optional<bool> e = onOff(*v);
            if (!e) return fail(QStringLiteral("expected on or off, not '%1'").arg(*v));
            m_p.portName = *n;
            m_p.enable = *e;
            return;
        }
        default:
            return;
        }
    }

    void finish()
    {
        const Command c = m_p.command;
        if (c == Command::None) {
            if (m_args.isEmpty()) return;   // the caller decides (GUI, TUI or help)
            return fail(QStringLiteral("no command given (%1 is an option)").arg(m_args.front()));
        }
        const QString name = commandName(c);
        if (takesNothingElse(c) && m_args.size() != 1 + (m_p.intent ? 1 : 0))
            return fail(QStringLiteral("%1 takes no other arguments").arg(name));
        if (m_p.forceRecreate && c != Command::Install && c != Command::Apply)
            return fail(QStringLiteral("--force-recreate can only be used with --install or --apply"));
        if (m_p.yes && c != Command::Repair) return fail(QStringLiteral("--yes can only be used with --repair"));
        if (m_p.removeSettings && c != Command::Uninstall)
            return fail(QStringLiteral("--remove-settings can only be used with --uninstall"));
        if (m_p.intent && c != Command::Gui) return fail(QStringLiteral("--intent can only be used with --gui"));
        if (m_p.confirmed && c != Command::UninstallFromApps)
            return fail(QStringLiteral("--confirmed can only be used with --uninstall-from-apps"));
        if ((m_seenBaud || m_seenFormat || m_seenBind || m_seenAllow) && c != Command::AddPort)
            return fail(QStringLiteral("--baud, --format, --bind and --allow can only be used with --add-port"));
        if (m_p.config && !takesConfig(c))
            return fail(QStringLiteral("--config cannot be used with %1; the service always uses the installed config.json")
                            .arg(name));
        if (c == Command::Run && !m_p.config)
            return fail(QStringLiteral("--run needs --config <file>: a scratch config outside the data folder. The "
                                       "installed config runs only as the service (use --start)"));
    }

    const QStringList& m_args;
    qsizetype m_i = 0;
    Parsed m_p;
    bool m_seenForce = false, m_seenYes = false, m_seenConfig = false, m_seenRemoveSettings = false, m_seenConfirmed = false,
         m_seenIntent = false;
    bool m_seenBaud = false, m_seenFormat = false, m_seenBind = false, m_seenAllow = false;
};

}  // namespace

QString commandName(Command c)
{
    switch (c) {
    case Command::None: return QStringLiteral("(no command)");
    case Command::Help: return QStringLiteral("--help");
    default:
        for (const Name& n : kCommands)
            if (n.command == c) return QString::fromLatin1(n.text);
    }
    return {};
}

Parsed parse(const QStringList& args)
{
    return Parser(args).run();
}

bool isMutating(Command c, bool scratchConfig)
{
    switch (c) {
    case Command::Install:
    case Command::Apply:
    case Command::Start:
    case Command::Stop:
    case Command::Uninstall:
    case Command::Repair:
    case Command::DiscardPending:
        return true;
    case Command::AddPort:
    case Command::RemovePort:
    case Command::SetEnabled:
        return !scratchConfig;
    default:
        return false;
    }
}

}  // namespace asb::cli
