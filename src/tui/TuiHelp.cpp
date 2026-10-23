#include "tui/TuiHelp.h"

#include "core/HelpText.h"
#include "tui/TuiView.h"

namespace asb::tui {

// ASCII only below: the help is read on terminals without UTF-8 too. (The one exception is the Windows Allowed hint,
// the window's own example line with its middle dot; the edit screen draws that dot as '-' without UTF-8.)

QString fieldLabel(Field f)
{
    switch (f) {
    case Field::Name: return QStringLiteral("Name");
    case Field::Enabled: return QStringLiteral("Enabled");
    case Field::Device: return QStringLiteral("Device");
    case Field::Baud: return QStringLiteral("Baud");
    case Field::Format: return QStringLiteral("Format");
    case Field::Listen: return QStringLiteral("Listen on");
    case Field::TcpPort: return QStringLiteral("TCP port");
    case Field::Allowed: return QStringLiteral("Allowed");
    case Field::Dtr: return QStringLiteral("Assert DTR");
    case Field::Rts: return QStringLiteral("Assert RTS");
    }
    return {};
}

QStringList fieldHint(Field f, bool windows)
{
    const QString example = QString::fromLatin1(help::kAllowedExample);
    switch (f) {
    case Field::Name: return {QStringLiteral("A label for the port, e.g. KAT500 (up to 40 characters).")};
    case Field::Enabled: return {QStringLiteral("Space or x: bridge this port, or keep it in the list but off.")};
    case Field::Device:
        return {windows ? QStringLiteral("The serial port, e.g. COM3. p on the ports screen lists them.")
                        : QStringLiteral("Use the /dev/serial/by-id/ name: ttyUSB numbers can swap on replug.")};
    case Field::Baud: return {QStringLiteral("The serial speed, e.g. 38400. It must match the device.")};
    case Field::Format: return {QStringLiteral("Data bits, parity and stop bits, e.g. 8N1.")};
    case Field::Listen:
        return {QStringLiteral("127.0.0.1 this %1 only, 0.0.0.0 every interface, or one address.")
                    .arg(windows ? QStringLiteral("PC") : QStringLiteral("machine"))};
    case Field::TcpPort: return {QStringLiteral("The port AetherSDR connects to, e.g. 4001. One per serial port.")};
    case Field::Allowed:
        // Windows: the window's two texts, as the port card shows them (the example, then what the rule admits); each
        // fits one line at 80 columns.
        if (windows) return {help::allowedExampleLine(true), help::allowedEffect(true)};
        return {QStringLiteral("e.g. %1. Blank: any address. Linux does not enforce it.").arg(example)};
    case Field::Dtr: return {QStringLiteral("Raise DTR while a client is connected. Off unless the device needs it.")};
    case Field::Rts: return {QStringLiteral("Raise RTS while a client is connected. Off unless the device needs it.")};
    }
    return {};
}

QString fieldAbout(Field f, bool windows)
{
    switch (f) {
    case Field::Name:
        return QStringLiteral("a label for the port, e.g. KAT500. Letters, digits, spaces and . ( ) - only, up to 40 "
                              "characters. When it is blank the device is shown instead.");
    case Field::Enabled:
        return QStringLiteral("[x] bridges the port. [ ] keeps it in the list without bridging it: its TCP port is "
                              "not opened.");
    case Field::Device:
        return windows ? QStringLiteral("the serial port the device is on, e.g. COM3. p on the ports screen lists the "
                                        "serial ports.")
                       : QStringLiteral("the serial device, e.g. /dev/serial/by-id/usb-FTDI_FT232R_USB_UART_DEMO0001-"
                                        "if00-port0. Use the /dev/serial/by-id/ name: it stays the same when the device "
                                        "is unplugged and plugged in again, while /dev/ttyUSB0 and /dev/ttyUSB1 can "
                                        "swap. p on the ports screen lists the serial ports.");
    case Field::Baud:
        return QStringLiteral("the serial speed, e.g. 38400. It must match the speed set on the device.");
    case Field::Format:
        return QStringLiteral("data bits, parity and stop bits, e.g. 8N1: 5 to 8 data bits; parity N (none), E (even), "
                              "O (odd), M (mark) or S (space); 1, 1.5 or 2 stop bits.");
    case Field::Listen:
        return QStringLiteral("the address on this %1 that AetherSDR connects to: 127.0.0.1 for this %1 only, one of "
                              "its LAN or VPN addresses (e.g. 192.0.2.20), or 0.0.0.0 for every network interface.")
            .arg(windows ? QStringLiteral("PC") : QStringLiteral("machine"));
    case Field::TcpPort:
        return QStringLiteral("the TCP port AetherSDR connects to, e.g. 4001: 1 to 65535, a different one for each "
                              "port.");
    case Field::Allowed:
        // Windows: FirewallPlan::build (the rule's RemoteAddress; blank: Any). Linux: NullFirewall (D7).
        return QStringLiteral("who may connect: addresses, blocks or ranges separated by commas, e.g. %1 or "
                              "192.0.2.10-192.0.2.20. Blank means any address. On Windows: %2 Keywords such as %3 "
                              "work there too. On Linux the list is saved but not enforced: use the system firewall to "
                              "restrict who can connect.")
            .arg(QString::fromLatin1(help::kAllowedExample), help::allowedEffect(true),
                 QString::fromLatin1(help::kAllowedKeywordExample));
    case Field::Dtr:
        return QStringLiteral("raise the DTR line while a client is connected. Leave it off unless the device needs "
                              "it: some amplifiers treat DTR as a power or PTT line.");
    case Field::Rts:
        return QStringLiteral("raise the RTS line while a client is connected. Leave it off unless the device needs "
                              "it: some amplifiers treat RTS as a power or PTT line.");
    }
    return {};
}

QList<KeyHelp> keyHelp(Screen s)
{
    const KeyHelp quit{QStringLiteral("Ctrl+C"), {QStringLiteral("Ctrl+C")}, QStringLiteral("quit.")};
    switch (s) {
    case Screen::Ports:
        return {
            {QStringLiteral("Up/Down or j/k"), {QStringLiteral("Up"), QStringLiteral("Down"), QStringLiteral("j"), QStringLiteral("k")},
             QStringLiteral("select a port, or the auto-start row under the ports.")},
            {QStringLiteral("Enter"), {QStringLiteral("Enter")},
             QStringLiteral("edit the selected port (on the auto-start row: turn it on or off).")},
            {QStringLiteral("a"), {QStringLiteral("a")}, QStringLiteral("add a port.")},
            {QStringLiteral("d"), {QStringLiteral("d")}, QStringLiteral("delete the selected port.")},
            {QStringLiteral("Space"), {QStringLiteral("Space")}, QStringLiteral("turn the selected port, or auto-start, on or off.")},
            {QStringLiteral("s"), {QStringLiteral("s")}, QStringLiteral("Save & apply: see below.")},
            {QStringLiteral("r"), {QStringLiteral("r")},
             QStringLiteral("reload the settings from disk. Unsaved changes are dropped (it asks first).")},
            {QStringLiteral("t"), {QStringLiteral("t")}, QStringLiteral("start the bridge with the applied settings.")},
            {QStringLiteral("x"), {QStringLiteral("x")},
             QStringLiteral("stop the bridge. It also turns auto-start off: t starts the bridge again with the applied "
                            "settings; to use Save & apply (s) instead, turn auto-start back on first.")},
            {QStringLiteral("p"), {QStringLiteral("p")},
             QStringLiteral("list the serial ports. They are only listed, never opened.")},
            {QStringLiteral("l"), {QStringLiteral("l")}, QStringLiteral("show the log and the latest status of each port.")},
            {QStringLiteral("R"), {QStringLiteral("R")}, QStringLiteral("repair the data folder: see below.")},
            {QStringLiteral("P"), {QStringLiteral("P")}, QStringLiteral("load the saved, not applied settings.")},
            {QStringLiteral("D"), {QStringLiteral("D")}, QStringLiteral("discard the saved, not applied settings (it asks first).")},
            {QStringLiteral("u"), {QStringLiteral("u")},
             QStringLiteral("when a different build of the bridge is installed: update it to this one, keeping the "
                            "settings (it asks first).")},
            {QStringLiteral("?"), {QStringLiteral("?")}, QStringLiteral("this help.")},
            {QStringLiteral("q or Ctrl+C"), {QStringLiteral("q"), QStringLiteral("Ctrl+C")},
             QStringLiteral("quit. With unsaved changes it asks first.")},
        };
    case Screen::Edit:
        return {
            {QStringLiteral("Tab or Down"), {QStringLiteral("Tab"), QStringLiteral("Down")}, QStringLiteral("next field.")},
            {QStringLiteral("Shift+Tab or Up"), {QStringLiteral("Shift+Tab"), QStringLiteral("Up")}, QStringLiteral("previous field.")},
            {QStringLiteral("Typing, Backspace"), {QStringLiteral("Backspace")},
             QStringLiteral("change a text field (read-only: the form says why it cannot).")},
            {QStringLiteral("Space or x"), {QStringLiteral("Space"), QStringLiteral("x")},
             QStringLiteral("tick or untick Enabled, Assert DTR or Assert RTS (read-only: the form says why it "
                            "cannot).")},
            {QStringLiteral("Enter"), {QStringLiteral("Enter")},
             QStringLiteral("accept the port and go back to the list (read-only: just go back). Nothing is saved until "
                            "Save & apply (s).")},
            {QStringLiteral("Esc"), {QStringLiteral("Esc")}, QStringLiteral("cancel. If you changed something it asks first.")},
            quit,
        };
    case Screen::Confirm:
        return {
            {QStringLiteral("y"), {QStringLiteral("y"), QStringLiteral("Y")}, QStringLiteral("yes.")},
            {QStringLiteral("n"), {QStringLiteral("n"), QStringLiteral("N")},
             QStringLiteral("no. When another build of the bridge is installed, n keeps that build and still applies "
                            "your settings to it.")},
            {QStringLiteral("Esc"), {QStringLiteral("Esc")}, QStringLiteral("cancel: nothing is done.")},
            quit,
        };
    case Screen::Result:
        return {
            {QStringLiteral("Enter, Esc or q"), {QStringLiteral("Enter"), QStringLiteral("Esc"), QStringLiteral("q")},
             QStringLiteral("back to the ports.")},
            quit,
        };
    case Screen::Page:
        return {
            {QStringLiteral("Up/Down or j/k"), {QStringLiteral("Up"), QStringLiteral("Down"), QStringLiteral("j"), QStringLiteral("k")},
             QStringLiteral("scroll one line.")},
            {QStringLiteral("PgUp/PgDn"), {QStringLiteral("PgUp"), QStringLiteral("PgDn")}, QStringLiteral("scroll one page.")},
            {QStringLiteral("Esc or q"), {QStringLiteral("Esc"), QStringLiteral("q")}, QStringLiteral("back to the ports.")},
            quit,
        };
    }
    return {};
}

HelpPage helpPage(bool windows, const QString& readOnlyReason, const QString& dataFolder, const QString& version)
{
    HelpPage h;
    auto heading = [&h](const QString& t) {
        if (!h.lines.isEmpty()) h.lines << QString();
        h.headings << int(h.lines.size());
        h.lines << t;
    };
    auto keys = [&h](Screen s) {
        for (const KeyHelp& k : keyHelp(s)) h.lines << k.shown + QStringLiteral(": ") + k.does;
    };
    const QString machine = windows ? QStringLiteral("PC") : QStringLiteral("machine");

    heading(QStringLiteral("What the bridge does"));
    h.lines << QStringLiteral("Each port joins one serial device to one TCP port. AetherSDR connects to the TCP port and "
                              "talks to the device as if it were plugged into its own %1. One client at a time: a new "
                              "connection replaces the current one. The device is opened only while a client is "
                              "connected.")
                   .arg(machine);

    heading(QStringLiteral("Read-only or administrator"));
    h.lines << (windows ? QStringLiteral("Anyone can look. To change anything, run aether-serial-bridge.exe --tui from a "
                                         "terminal opened with Run as administrator.")
                        : QStringLiteral("Anyone can look. To change anything, run sudo aether-serial-bridge --tui."));
    h.lines << (readOnlyReason.isEmpty() ? QStringLiteral("You can change ports now.")
                                         : QStringLiteral("Now: %1.").arg(readOnlyReason));

    heading(QStringLiteral("The ports screen"));
    keys(Screen::Ports);

    heading(QStringLiteral("Editing a port"));
    h.lines << QStringLiteral("Enter on a port, or a to add one, opens its form. The line under the form explains the "
                              "field you are on.");
    for (int i = 0; i < kFieldCount; ++i) h.lines << fieldLabel(Field(i)) + QStringLiteral(": ") + fieldAbout(Field(i), windows);
    h.lines << QString() << QStringLiteral("Keys in the form:");
    keys(Screen::Edit);

    heading(autoStartLabel(windows));
    h.lines << (windows ? QStringLiteral("The row under the ports. On: the bridge runs as a Windows service and starts "
                                         "with Windows. Turning it off and saving removes the service: the ports stop "
                                         "being bridged until you turn it back on (it asks first).")
                        : QStringLiteral("The row under the ports. On: the bridge starts at boot. Turning it off and "
                                         "saving stops the bridge and keeps it from starting: the ports stop being "
                                         "bridged until you turn it back on (it asks first)."));
    // Stop and a failed apply revoke start.token (Barrier B0), and a clean editor reads auto-start from it, so s would
    // then take the untick's path (TuiApp::captureIntent), not start the bridge.
    h.lines << QStringLiteral("Stop (x) and a Save & apply that did not finish turn it off too. Then t starts the bridge "
                              "again with the applied settings; to use s instead, turn auto-start back on first.");

    heading(QStringLiteral("Save & apply (s)"));
    h.lines << QStringLiteral("Your changes take effect only when you press s. With auto-start on, Save & apply saves the "
                              "settings, stops the bridge and starts it again with them, so a connected client "
                              "(AetherSDR) is dropped and has to connect again. If the bridge is not installed yet, it "
                              "is installed first. With auto-start off, see above.");
    h.lines << QStringLiteral("It asks first when a port would listen on every network interface with nothing "
                              "restricting who can connect.");

    heading(QStringLiteral("Saved, not applied"));
    h.lines << QStringLiteral("Settings that were saved but not applied are kept in config.pending.json: a Save & apply "
                              "that did not finish, or a change made with the command line. The header then says "
                              "\"Saved, not applied\" and they are the settings shown here. s applies them with "
                              "auto-start on (a Save & apply that did not finish turned it off: turn it back on first). "
                              "D discards them without starting or stopping the bridge. P loads them again.");

    heading(QStringLiteral("Repair (R)"));
    h.lines << QStringLiteral("When the data folder is not trusted (for example something in it has the wrong owner or permissions), "
                              "nothing is read from it and the header says so. Repair stops the bridge, moves the "
                              "untrusted folder aside with everything in it (nothing is deleted) and creates a fresh "
                              "one. It asks first.");

    heading(QStringLiteral("Log (l) and serial ports (p)"));
    h.lines << (windows ? QStringLiteral("l shows the end of bridge.log in the data folder, then the latest status of "
                                         "each port.")
                        : QStringLiteral("The bridge logs to the system journal: journalctl -u aether-serial-bridge. "
                                         "l shows that command and the latest status of each port."));
    h.lines << QStringLiteral("p lists the serial ports this %1 has. It never opens them, so a running bridge is not "
                              "disturbed.")
                   .arg(machine);

    heading(QStringLiteral("Questions"));
    keys(Screen::Confirm);
    heading(QStringLiteral("Results"));
    keys(Screen::Result);
    heading(QStringLiteral("This page, the log and the port list"));
    keys(Screen::Page);

    heading(QStringLiteral("About"));
    h.lines << QStringLiteral("Aether Serial Bridge %1").arg(version);
    h.lines << QStringLiteral("Data folder: %1").arg(dataFolder);
    return h;
}

}  // namespace asb::tui
