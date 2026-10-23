#pragma once

// The window's program locator (v1.1 WS-14 §2; used by WS-15's staged install and WS-18's session): the headless
// program this window installs or runs, and whether its bytes are the ones the window was built with (the digest
// compiled into the window, HeadlessDigest.h, D61).
//
//   Linux, inside an AppImage   $APPDIR/usr/libexec/aether-serial-bridge, but only when $APPDIR is the directory two
//                               levels above this executable's own directory (the image's usr/bin): an APPDIR
//                               inherited from some other AppImage is ignored.
//   Linux, otherwise            aether-serial-bridge beside this executable (development and test builds).
//   Windows                     aether-serial-bridge.exe beside this executable.
//
// A mismatch disables what would install or run that program, with problem() as the reason (WS-17 shows it).

#include <QByteArray>
#include <QString>

#include <mutex>
#include <utility>

namespace asb::app {

struct WindowProgram {
    QString path;
    QString sha256;          // the file's digest when it was read whole (empty otherwise)
    bool digestOk = false;   // sha256 == the digest compiled into the window
    QString problem;         // why not, for the window: empty when digestOk
};

// Pure: where the program is, given this executable's absolute path and the APPDIR environment value.
QString windowProgramPath(const QString& ownExecutable, const QString& appDir);

// One program file, read whole and hashed on the first check() only (never on the GUI thread); every check() then
// judges that digest against the `expectedSha256` it is given, so the answer follows the argument.
class WindowProgramProbe {
public:
    explicit WindowProgramProbe(QString path) : m_path(std::move(path)) {}
    WindowProgram check(const QByteArray& expectedSha256);

private:
    std::mutex m_mutex;
    QString m_path;
    bool m_read = false;
    QString m_sha256;      // when the file was read whole
    QString m_readError;   // otherwise
};

// The program this window installs or runs (windowProgramPath for this process), through one probe for the process's
// life, compared with `expectedSha256` (lower-case hex).
WindowProgram windowProgram(const QByteArray& expectedSha256);

}  // namespace asb::app
