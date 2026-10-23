// Log sinks (WS-4, master §4.7): INV-68 bounded rollover (each file ≤ the limit
// + one line; a failed rollover skips the append), never throwing, the journal priority prefixes.

#include "core/LogSinks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

using namespace asb;

namespace {

// An in-memory model of the two files with failure injection.
struct MemFiles {
    QByteArray log, prev;
    bool logExists = false, prevExists = false;
    bool failRollover = false, failAppend = false;
    QStringList ops;
};

class MemOps final : public LogFileOps {
public:
    explicit MemOps(MemFiles& f) : m_f(f) {}
    qint64 size() override { return m_f.logExists ? m_f.log.size() : -1; }
    bool rollover(QString* err) override
    {
        m_f.ops << QStringLiteral("rollover");
        if (m_f.failRollover) {
            if (err) *err = QStringLiteral("injected");
            return false;
        }
        m_f.prev = m_f.log;
        m_f.prevExists = true;
        m_f.log.clear();
        m_f.logExists = false;
        return true;
    }
    bool append(const QByteArray& b, QString* err) override
    {
        m_f.ops << QStringLiteral("append");
        if (m_f.failAppend) {
            if (err) *err = QStringLiteral("injected");
            return false;
        }
        m_f.log += b;
        m_f.logExists = true;
        return true;
    }

private:
    MemFiles& m_f;
};

const QDateTime kWhen = QDateTime::fromString(QStringLiteral("2026-10-14T09:30:05Z"), Qt::ISODate);

void logEverything(LogSink& s)
{
    s.write(QtInfoMsg, kWhen, QStringLiteral("plain 1"));
    s.write(QtWarningMsg, kWhen, QStringLiteral("warn 2"));
    s.write(QtCriticalMsg, kWhen, QStringLiteral("error 3"));
    s.write(QtFatalMsg, kWhen, QStringLiteral("critical"));
}

}  // namespace

class tst_LogSink : public QObject {
    Q_OBJECT

private slots:
    void lineLayout()
    {
        const QByteArray line = FileLogSink::formatLine(QtInfoMsg, kWhen, QStringLiteral("COM3 listening"));
        const QString local = kWhen.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
#ifdef Q_OS_WIN
        const QByteArray nl = "\r\n";
#else
        const QByteArray nl = "\n";
#endif
        QCOMPARE(line, (local + QStringLiteral(" Information COM3 listening")).toUtf8() + nl);
        QVERIFY(FileLogSink::formatLine(QtWarningMsg, kWhen, QStringLiteral("x")).contains(" Warning     x"));
        QVERIFY(FileLogSink::formatLine(QtCriticalMsg, kWhen, QStringLiteral("x")).contains(" Error       x"));
        QVERIFY(FileLogSink::formatLine(QtFatalMsg, kWhen, QStringLiteral("x")).contains(" Critical    x"));
    }

    // INV-68 (N1): the rename failed → this append is skipped, the file does not grow past the bound.
    void failedRolloverDoesNotAppend()
    {
        MemFiles f;
        f.log = QByteArray(120, 'x');
        f.logExists = true;
        f.failRollover = true;
        FileLogSink sink(std::make_unique<MemOps>(f), 100);
        sink.write(QtInfoMsg, kWhen, QStringLiteral("must not land"));
        QCOMPARE(f.ops, QStringList{QStringLiteral("rollover")});
        QCOMPARE(f.log, QByteArray(120, 'x'));
        f.failRollover = false;
        sink.write(QtInfoMsg, kWhen, QStringLiteral("lands"));
        QCOMPARE(f.prev, QByteArray(120, 'x'));
        QVERIFY(f.log.contains("lands"));
    }

    // INV-68 (N1): each file ≤ limit + one line; together ≤ 2 × (limit + one line). Rollover failures
    // and append failures are mixed in at random.
    void sizeBound()
    {
        constexpr qint64 kMax = 1000;
        MemFiles f;
        FileLogSink sink(std::make_unique<MemOps>(f), kMax);
        QRandomGenerator rng(4242);
        qint64 longest = 0;
        for (int i = 0; i < 20000; ++i) {
            f.failRollover = rng.bounded(10) == 0;
            f.failAppend = rng.bounded(20) == 0;
            const QString text = QString(int(rng.bounded(1, 300)), QLatin1Char(char('a' + i % 26)));
            longest = std::max<qint64>(longest, FileLogSink::formatLine(QtInfoMsg, kWhen, text).size());
            sink.write(QtInfoMsg, kWhen, text);
            QVERIFY2(f.log.size() <= kMax + longest, qPrintable(QString::number(f.log.size())));
            QVERIFY(f.prev.size() <= kMax + longest);
        }
        QVERIFY(f.prevExists);
        QCOMPARE(FileLogSink::kDefaultMaxBytes, qint64(1'000'000));
    }

    void debugLinesAreNotWritten()
    {
        MemFiles f;
        FileLogSink sink(std::make_unique<MemOps>(f));
        sink.write(QtDebugMsg, kWhen, QStringLiteral("noise"));
        QVERIFY(f.ops.isEmpty());
    }

    void aFailingAppendNeverThrows()
    {
        MemFiles f;
        f.failAppend = true;
        FileLogSink sink(std::make_unique<MemOps>(f));
        logEverything(sink);
        QCOMPARE(f.ops.size(), 4);
    }

    // Rolls over at the limit and keeps one previous file, on real files.
    void rollsOverAtTheLimitAndKeepsOnePreviousFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("bridge.log"));
        FileLogSink sink(makeLogFileOps(path), 50);
        for (int i = 0; i < 10; ++i) sink.write(QtInfoMsg, kWhen, QStringLiteral("line %1 padding padding").arg(i));
        QVERIFY(QFile::exists(path + QStringLiteral(".1")));
        QVERIFY(!QFile::exists(path + QStringLiteral(".2")));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("line 9"));
    }

    // A blocked rollover never throws: ".1" is a non-empty directory, so the rename fails; the append is
    // then skipped (appending anyway would let the log grow without bound).
    void aBlockedRolloverNeverThrows()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("bridge.log"));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(100, 'x'));
        }
        QVERIFY(QDir().mkpath(path + QStringLiteral(".1")));
        {
            QFile keep(path + QStringLiteral(".1/keep"));
            QVERIFY(keep.open(QIODevice::WriteOnly));
            keep.write("k");
        }
        FileLogSink sink(makeLogFileOps(path), 10);
        logEverything(sink);
        QVERIFY(QFileInfo(path + QStringLiteral(".1")).isDir());
        QCOMPARE(QFileInfo(path).size(), qint64(100));
    }

    // A log path that is a directory never throws.
    void aLogPathThatIsADirectoryNeverThrows()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("bridge.log"));
        QVERIFY(QDir().mkpath(path));
        FileLogSink sink(makeLogFileOps(path));
        logEverything(sink);
        QVERIFY(QFileInfo(path).isDir());
    }

    void journalPrefixes()
    {
        QCOMPARE(JournalLogSink::formatLine(QtDebugMsg, QStringLiteral("d")), QByteArray("<7>d\n"));
        QCOMPARE(JournalLogSink::formatLine(QtInfoMsg, QStringLiteral("i")), QByteArray("<6>i\n"));
        QCOMPARE(JournalLogSink::formatLine(QtWarningMsg, QStringLiteral("w")), QByteArray("<4>w\n"));
        QCOMPARE(JournalLogSink::formatLine(QtCriticalMsg, QStringLiteral("e")), QByteArray("<3>e\n"));
        QCOMPARE(JournalLogSink::formatLine(QtFatalMsg, QStringLiteral("f")), QByteArray("<2>f\n"));
    }
};

QTEST_GUILESS_MAIN(tst_LogSink)
#include "tst_LogSink.moc"
