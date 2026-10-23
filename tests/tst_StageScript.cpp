// The staging script (v1.1 WS-15 §1, INV-122): the window embeds packaging/linux/stage.sh byte for byte, it passes
// shellcheck as POSIX sh, and its stderr lines are exactly the catalogue the systemd suite's bootstrap_* cases assert
// (tests/systemd/stage_catalogue.json), both ways. The script's behaviour as root is the systemd suite's.

#include "cli/StageScript.h"
#include "cli/StagedHelper.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTest>

using namespace asb::cli;

namespace {

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const QString kScriptFile = QStringLiteral(ASB_SOURCE_DIR "/packaging/linux/stage.sh");

}  // namespace

class tst_StageScript : public QObject {
    Q_OBJECT

private slots:
    void embeddedEqualsPackagingFile()
    {
        const QByteArray file = readFile(kScriptFile);
        QVERIFY(!file.isEmpty());
        QCOMPARE(QByteArray(staged::kStageScript), file);
        QCOMPARE(staged::script(), file);
        // The arguments the window gives pkexec carry it unchanged.
        const QStringList args = staged::pkexecArguments(QByteArray(64, 'a'), 12, 34);
        QCOMPARE(args, (QStringList{"/bin/sh", "-c", QString::fromUtf8(file), "asb-stage", QString(64, QLatin1Char('a')), "12", "34"}));
    }
    void shellcheckClean()
    {
        const QString sc = QStandardPaths::findExecutable(QStringLiteral("shellcheck"));
        if (sc.isEmpty()) QSKIP("no shellcheck here; the static-checks leg runs it on packaging/linux/stage.sh");
        QProcess p;
        p.start(sc, {QStringLiteral("-s"), QStringLiteral("sh"), kScriptFile});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY2(p.exitCode() == 0, p.readAllStandardOutput().constData());
    }
    // Every `die N "..."` (and the signal trap's line) is in the catalogue with its exit code, and every catalogue
    // entry is in the script.
    void catalogueMatchesScript()
    {
        const QString script = QString::fromUtf8(readFile(kScriptFile));
        QSet<QString> inScript;
        const auto norm = [](QString line) {
            return line.replace(QLatin1String("$d"), QLatin1String("<dir>"))
                .replace(QLatin1String("$plen"), QLatin1String("<plen>"))
                .replace(QLatin1String("$rlen"), QLatin1String("<rlen>"))
                .replace(QLatin1String("$rc"), QLatin1String("<rc>"));
        };
        static const QRegularExpression die(QStringLiteral("die ([0-9]) \"([^\"]*)\""));
        for (auto it = die.globalMatch(script); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            inScript << m.captured(1) + QLatin1Char(' ') + norm(m.captured(2));
        }
        static const QRegularExpression trap(QStringLiteral("echo \"asb-stage: ([^\"]*)\" >&2; exit ([0-9])"));
        for (auto it = trap.globalMatch(script); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            inScript << m.captured(2) + QLatin1Char(' ') + norm(m.captured(1));
        }
        QVERIFY(inScript.size() >= 10);
        const QJsonDocument doc = QJsonDocument::fromJson(readFile(QStringLiteral(ASB_SOURCE_DIR "/tests/systemd/stage_catalogue.json")));
        QSet<QString> inCatalogue;
        for (const QJsonValue& v : doc.object().value(QStringLiteral("lines")).toArray())
            inCatalogue << QString::number(v.toObject().value(QStringLiteral("exit")).toInt()) + QLatin1Char(' ') +
                               v.toObject().value(QStringLiteral("line")).toString();
        QSet<QString> missing = inScript - inCatalogue, extra = inCatalogue - inScript;
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("not in the catalogue: ") + QStringList(missing.values()).join(QStringLiteral("; "))));
        QVERIFY2(extra.isEmpty(), qPrintable(QStringLiteral("not in the script: ") + QStringList(extra.values()).join(QStringLiteral("; "))));
        // The exits of the script's own are 3, 4 and 6 only: never the helper's 0/1/2/5, never pkexec's 126/127.
        for (const QString& e : inScript) QVERIFY2(e.startsWith(QLatin1Char('3')) || e.startsWith(QLatin1Char('4')) || e.startsWith(QLatin1Char('6')), qPrintable(e));
    }
    void stagedPathAndLineHelpers()
    {
        QVERIFY(staged::isStagedProgramPath(QStringLiteral("/usr/local/lib/aether-serial-bridge/staging.AbC12xYz/aether-serial-bridge")));
        QVERIFY(!staged::isStagedProgramPath(QStringLiteral("/usr/local/lib/aether-serial-bridge/staging.AbC12xYz/aether-serial-bridge\n")));
        QVERIFY(!staged::isStagedProgramPath(QStringLiteral("/usr/local/lib/aether-serial-bridgeX/staging.AbC12xYz/aether-serial-bridge")));
        QCOMPARE(staged::stageLine("x\nasb-stage: bad lengths\n"), QStringLiteral("bad lengths"));
        QCOMPARE(staged::stageLine("error: asb-stage: x\n"), QString());
        QCOMPARE(staged::unsafeDirectory(QStringLiteral("/usr/local is not a directory only root can write")), QStringLiteral("/usr/local"));
        QCOMPARE(staged::unsafeDirectory(QStringLiteral("/ is not a directory only root can write")), QStringLiteral("/"));
        QCOMPARE(staged::unsafeDirectory(QStringLiteral("bad lengths")), QString());
    }
};

QTEST_GUILESS_MAIN(tst_StageScript)
#include "tst_StageScript.moc"
