#pragma once

// The helper's bounded read of its request (master §5.7 input, S10, D34, INV-89). POSIX only.
//
// The descriptor is made nonblocking (and put back as it was afterwards); the read loops poll() against ONE
// deadline for the whole request (a slow trickle of bytes does not extend it), and reads into a buffer of
// cap + 1 bytes, each read() bounded by the room left. The (cap + 1)th byte ends the read at once (TooLarge):
// nothing more is read and nothing larger is ever held. EOF before the deadline is the only success.

#include <QByteArray>
#include <QString>

#include <chrono>

namespace asb::cli::helper {

struct ReadResult {
    enum class Status { Ok, TooLarge, Deadline, Error } status = Status::Error;
    QByteArray bytes;   // Ok: everything up to EOF (at most cap bytes)
    QString error;      // Error: what failed
};

ReadResult readBounded(int fd, qsizetype cap, std::chrono::milliseconds deadline);

}  // namespace asb::cli::helper
