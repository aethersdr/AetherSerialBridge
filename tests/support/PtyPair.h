#pragma once

// A pseudo-terminal pair for the real-QSerialPort tests (WS-4, tst_SerialPty; Linux). The slave path
// (/dev/pts/N) is what the bridge opens as its serial port; the test plays the device on the master
// fd. Made in-process with posix_openpt, so no helper (socat) is needed and the tests never skip.

#include <QByteArray>
#include <QString>

namespace asb::test {

class PtyPair {
public:
    PtyPair();
    ~PtyPair();
    PtyPair(const PtyPair&) = delete;
    PtyPair& operator=(const PtyPair&) = delete;

    bool ok() const { return m_master >= 0; }
    QString error() const { return m_error; }
    QString slavePath() const { return m_slave; }
    // TCGETS on a pty master reads the slave's termios (the slave itself is held exclusively by
    // QSerialPort, TIOCEXCL).
    int masterFd() const { return m_master; }

    bool writeMaster(const QByteArray&);
    // Reads until `want` bytes arrived or `timeoutMs` passed.
    QByteArray readMaster(int want, int timeoutMs);
    // Hang up: the slave side then reads EIO.
    void closeMaster();

private:
    int m_master = -1;
    QString m_slave, m_error;
};

}  // namespace asb::test
