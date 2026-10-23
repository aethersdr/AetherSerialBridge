#include "platform/posix/ServiceIdentity.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace asb::posix::ServiceIdentity {

bool isManagedUnit(const QByteArray& content)
{
    QByteArray c = content;
    if (c.endsWith('\n')) c.chop(1);
    // One line, byte for byte: no second line, no trailing blanks, no other hierarchy.
    return c == QByteArray(kExpectedLine);
}

bool check(QString* why, const char* path)
{
    // Plain read(2) on purpose: this runs before anything else in --service (no Qt file classes, no locale).
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (why) *why = QStringLiteral("cannot read %1: %2").arg(QString::fromLocal8Bit(path), QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    QByteArray content;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        content.append(buf, int(n));
        if (content.size() > 64 * 1024) break;
    }
    ::close(fd);
    if (isManagedUnit(content)) return true;
    if (why) {
        QByteArray shown = content.left(512);
        shown.replace('\n', "; ");
        *why = QStringLiteral("this process's cgroup is \"%1\", not \"%2\"").arg(QString::fromLocal8Bit(shown), QString::fromLatin1(kExpectedLine));
    }
    return false;
}

}  // namespace asb::posix::ServiceIdentity
