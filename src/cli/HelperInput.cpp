#include "cli/HelperInput.h"

#include <cerrno>
#include <climits>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace asb::cli::helper {
namespace {

QString errnoText(int e)
{
    return QString::fromLocal8Bit(std::strerror(e));
}

// Puts the descriptor's status flags back as they were (stdin may share its open file description with a
// terminal or with stdout).
class FlagsRestorer {
public:
    FlagsRestorer(int fd, int flags) : m_fd(fd), m_flags(flags) {}
    ~FlagsRestorer()
    {
        if (m_flags >= 0) ::fcntl(m_fd, F_SETFL, m_flags);
    }
    FlagsRestorer(const FlagsRestorer&) = delete;
    FlagsRestorer& operator=(const FlagsRestorer&) = delete;

private:
    int m_fd, m_flags;
};

}  // namespace

ReadResult readBounded(int fd, qsizetype cap, std::chrono::milliseconds deadline)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point end = Clock::now() + deadline;
    ReadResult r;
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        r.error = QStringLiteral("standard input is not open: %1").arg(errnoText(errno));
        return r;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        r.error = QStringLiteral("cannot make standard input nonblocking: %1").arg(errnoText(errno));
        return r;
    }
    const FlagsRestorer restore(fd, flags);

    QByteArray buf(cap + 1, Qt::Uninitialized);
    qsizetype have = 0;
    for (;;) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
        if (left.count() <= 0) {
            r.status = ReadResult::Status::Deadline;
            return r;
        }
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        const int n = ::poll(&p, 1, int(std::min<qint64>(left.count(), INT_MAX)));
        if (n < 0) {
            if (errno == EINTR) continue;
            r.error = QStringLiteral("poll: %1").arg(errnoText(errno));
            return r;
        }
        if (n == 0) continue;   // the deadline is judged at the top of the loop
        if (!(p.revents & (POLLIN | POLLHUP))) {
            r.error = QStringLiteral("standard input failed (poll events 0x%1)").arg(p.revents, 0, 16);
            return r;
        }
        const ssize_t got = ::read(fd, buf.data() + have, size_t(cap + 1 - have));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            r.error = QStringLiteral("read: %1").arg(errnoText(errno));
            return r;
        }
        if (got == 0) {
            buf.truncate(have);
            r.status = ReadResult::Status::Ok;
            r.bytes = buf;
            return r;
        }
        have += got;
        if (have > cap) {
            r.status = ReadResult::Status::TooLarge;   // no further read
            return r;
        }
    }
}

}  // namespace asb::cli::helper
