#pragma once

// Log sinks of the side-channel worker (master §4.7, S5, WS-4). A sink is called ONLY on the worker
// thread (INV-68, INV-83); it may block or fail, which delays the log but never the engine (INV-6).
//
//  FileLogSink     the Windows service log: one plain-text file, rolled over
//                  to "<name>.1" when it is larger than 1,000,000 bytes. A failed rollover skips that
//                  append, so each file holds at most 1,000,000 bytes + one line and the two files at
//                  most 2 MB + 2 lines (N1). Never throws.
//  JournalLogSink  Linux --service: one line per entry on stdout with the sd-daemon priority prefix
//                  ("<6>" info, "<4>" warning, "<3>" error, "<2>" fatal, "<7>" debug).
//  ConsoleLogSink  diagnostic --run: "HH:mm:ss level message" on stderr.

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QtGlobal>

#include <cstdio>
#include <memory>

namespace asb {

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(QtMsgType, const QDateTime& when, const QString& text) = 0;
};

// The file operations FileLogSink needs, so tests can fail any of them deterministically (an "unwritable"
// file test would silently pass on Windows and as root; master §8.0).
class LogFileOps {
public:
    virtual ~LogFileOps() = default;
    virtual qint64 size() = 0;                                   // -1 when the log does not exist
    virtual bool rollover(QString* err) = 0;                     // log → log.1, replacing log.1
    virtual bool append(const QByteArray& bytes, QString* err) = 0;
};

// The real file at `path` (QFile append; std::filesystem::rename for the rollover, which replaces the
// target on both OSes).
std::unique_ptr<LogFileOps> makeLogFileOps(const QString& path);

class FileLogSink final : public LogSink {
public:
    static constexpr qint64 kDefaultMaxBytes = 1'000'000;
    explicit FileLogSink(std::unique_ptr<LogFileOps> ops, qint64 maxBytes = kDefaultMaxBytes);
    void write(QtMsgType, const QDateTime& when, const QString& text) override;
    // "yyyy-MM-dd HH:mm:ss Information message" + the platform newline.
    static QByteArray formatLine(QtMsgType, const QDateTime& when, const QString& text);

private:
    std::unique_ptr<LogFileOps> m_ops;
    qint64 m_maxBytes;
};

class JournalLogSink final : public LogSink {
public:
    explicit JournalLogSink(std::FILE* out = stdout) : m_out(out) {}
    void write(QtMsgType, const QDateTime& when, const QString& text) override;
    static QByteArray formatLine(QtMsgType, const QString& text);

private:
    std::FILE* m_out;
};

class ConsoleLogSink final : public LogSink {
public:
    explicit ConsoleLogSink(std::FILE* out = stderr) : m_out(out) {}
    void write(QtMsgType, const QDateTime& when, const QString& text) override;

private:
    std::FILE* m_out;
};

// The level names the log uses: Debug, Information, Warning, Error, Critical.
QString logLevelName(QtMsgType);

}  // namespace asb
