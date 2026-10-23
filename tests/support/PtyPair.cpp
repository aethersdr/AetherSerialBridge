#include "support/PtyPair.h"

#include <QElapsedTimer>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace asb::test {

PtyPair::PtyPair()
{
    const int fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        m_error = QStringLiteral("posix_openpt: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }
    if (grantpt(fd) != 0 || unlockpt(fd) != 0) {
        m_error = QStringLiteral("grantpt/unlockpt: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(fd);
        return;
    }
    char name[128] = {};
    if (ptsname_r(fd, name, sizeof name) != 0) {
        m_error = QStringLiteral("ptsname_r: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(fd);
        return;
    }
    m_slave = QString::fromLocal8Bit(name);
    m_master = fd;
}

PtyPair::~PtyPair() { closeMaster(); }

bool PtyPair::writeMaster(const QByteArray& bytes)
{
    qsizetype done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(m_master, bytes.constData() + done, size_t(bytes.size() - done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

QByteArray PtyPair::readMaster(int want, int timeoutMs)
{
    QByteArray out;
    QElapsedTimer t;
    t.start();
    while (out.size() < want && m_master >= 0) {
        const int left = int(timeoutMs - t.elapsed());
        if (left <= 0) break;
        pollfd p{m_master, POLLIN, 0};
        const int r = ::poll(&p, 1, left);
        if (r <= 0) continue;
        char buf[4096];
        const ssize_t n = ::read(m_master, buf, sizeof buf);
        if (n > 0) out.append(buf, qsizetype(n));
        else if (n < 0 && errno != EINTR && errno != EAGAIN) break;
    }
    return out;
}

void PtyPair::closeMaster()
{
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
}

}  // namespace asb::test
