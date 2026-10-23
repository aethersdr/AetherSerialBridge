#pragma once

// Self-elevation (WS-16, D64): the only place either executable starts a process with ShellExecuteExW
// (process_creation_audit allows SelfElevate.cpp.obj|ShellExecuteExW and nothing else; its control proves an object
// calling ShellExecuteExW anywhere else fails the audit). It starts this process's OWN image (GetModuleFileNameW of
// the executable, never a path from the caller) with the "runas" verb and fixed arguments, and waits for it. Used by
// the console exe's --uninstall-from-apps before elevation, and by the window's Restart as administrator (WS-17); only
// in processes that hold no Applier object.
//
// Test builds: ASB_TEST_ELEVATOR=record:<file> writes the request (the image and the command line, one per line) to
// <file> instead of calling the shell, and reports a started process that exited 0.

#include <QString>
#include <QStringList>

namespace asb::win::SelfElevate {

struct Outcome {
    bool started = false;     // the elevated process ran (exitCode is its exit code)
    bool cancelled = false;   // the user said No at the UAC prompt (ERROR_CANCELLED): nothing ran
    int exitCode = 0;
    QString error;            // neither: why it could not be started
};

// How the elevated process is started: the console exe's --uninstall-from-apps waits for it with no window of its own
// (WS-16); the window's Restart as administrator (WS-17) shows it and does not wait (this window closes).
struct Mode {
    bool wait = true;
    bool show = false;
};

// Windows only.
Outcome relaunch(const QStringList& arguments, Mode mode = {});

// One argument quoted by the rules CommandLineToArgvW and the MSVC runtime read back (portable; tested on every OS).
inline QString quoteArgument(const QString& a)
{
    if (!a.isEmpty() && !a.contains(QLatin1Char(' ')) && !a.contains(QLatin1Char('\t')) && !a.contains(QLatin1Char('"')))
        return a;
    QString out = QStringLiteral("\"");
    qsizetype backslashes = 0;
    for (const QChar c : a) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"')) out += QString(backslashes * 2 + 1, QLatin1Char('\\'));
        else out += QString(backslashes, QLatin1Char('\\'));
        backslashes = 0;
        out += c;
    }
    out += QString(backslashes * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}

inline QString commandLine(const QStringList& arguments)
{
    QStringList quoted;
    for (const QString& a : arguments) quoted << quoteArgument(a);
    return quoted.join(QLatin1Char(' '));
}

}  // namespace asb::win::SelfElevate
