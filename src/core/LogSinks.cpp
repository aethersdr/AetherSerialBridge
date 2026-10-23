#include "core/LogSinks.h"

#include <QFile>
#include <QFileInfo>

#include <filesystem>
#include <system_error>

namespace asb {

QString logLevelName(QtMsgType t)
{
    switch (t) {
    case QtDebugMsg: return QStringLiteral("Debug");
    case QtInfoMsg: return QStringLiteral("Information");
    case QtWarningMsg: return QStringLiteral("Warning");
    case QtCriticalMsg: return QStringLiteral("Error");
    case QtFatalMsg: return QStringLiteral("Critical");
    }
    return QStringLiteral("Information");
}

namespace {

class QtLogFileOps final : public LogFileOps {
public:
    explicit QtLogFileOps(const QString& path) : m_path(path) {}

    qint64 size() override
    {
        const QFileInfo fi(m_path);
        return fi.exists() ? fi.size() : -1;
    }

    bool rollover(QString* err) override
    {
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path(m_path.toStdU16String()),
                                std::filesystem::path((m_path + QStringLiteral(".1")).toStdU16String()), ec);
        if (ec && err) *err = QString::fromStdString(ec.message());
        return !ec;
    }

    bool append(const QByteArray& bytes, QString* err) override
    {
        QFile f(m_path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            if (err) *err = f.errorString();
            return false;
        }
        const bool ok = f.write(bytes) == bytes.size();
        if (!ok && err) *err = f.errorString();
        f.close();
        return ok;
    }

private:
    QString m_path;
};

}  // namespace

std::unique_ptr<LogFileOps> makeLogFileOps(const QString& path) { return std::make_unique<QtLogFileOps>(path); }

FileLogSink::FileLogSink(std::unique_ptr<LogFileOps> ops, qint64 maxBytes) : m_ops(std::move(ops)), m_maxBytes(maxBytes) {}

QByteArray FileLogSink::formatLine(QtMsgType type, const QDateTime& when, const QString& text)
{
#ifdef Q_OS_WIN
    static const QString nl = QStringLiteral("\r\n");
#else
    static const QString nl = QStringLiteral("\n");
#endif
    // "<yyyy-MM-dd HH:mm:ss> <level, 11 wide> <message><newline>"
    return (when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) + QLatin1Char(' ')
            + logLevelName(type).leftJustified(11) + QLatin1Char(' ') + text + nl)
        .toUtf8();
}

void FileLogSink::write(QtMsgType type, const QDateTime& when, const QString& text)
{
    if (type == QtDebugMsg) return;   // Information and above
    // Larger than the limit → roll over first; if that fails, this line is skipped (the bound: each
    // file ≤ maxBytes + one line).
    const qint64 size = m_ops->size();
    if (size > m_maxBytes && !m_ops->rollover(nullptr)) return;
    m_ops->append(formatLine(type, when, text), nullptr);   // a failed append is dropped, never thrown
}

QByteArray JournalLogSink::formatLine(QtMsgType type, const QString& text)
{
    const char* prefix = "<6>";
    switch (type) {
    case QtDebugMsg: prefix = "<7>"; break;
    case QtInfoMsg: prefix = "<6>"; break;
    case QtWarningMsg: prefix = "<4>"; break;
    case QtCriticalMsg: prefix = "<3>"; break;
    case QtFatalMsg: prefix = "<2>"; break;
    }
    return QByteArray(prefix) + text.toUtf8() + '\n';
}

void JournalLogSink::write(QtMsgType type, const QDateTime&, const QString& text)
{
    const QByteArray line = formatLine(type, text);
    std::fwrite(line.constData(), 1, size_t(line.size()), m_out);
    std::fflush(m_out);
}

void ConsoleLogSink::write(QtMsgType type, const QDateTime& when, const QString& text)
{
    const QByteArray line = (when.toLocalTime().toString(QStringLiteral("HH:mm:ss")) + QLatin1Char(' ')
                             + logLevelName(type) + QLatin1Char(' ') + text + QLatin1Char('\n'))
                                .toUtf8();
    std::fwrite(line.constData(), 1, size_t(line.size()), m_out);
    std::fflush(m_out);
}

}  // namespace asb
