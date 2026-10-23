// Test builds only: see TestSeams_linux.h.

#include "app/TestSeams_linux.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <unistd.h>

#include <chrono>
#include <thread>

namespace asb::app::testseams {

QStringList pausePoints()
{
    return QString::fromLocal8Bit(qgetenv("ASB_TEST_PAUSE")).split(QLatin1Char(','), Qt::SkipEmptyParts);
}

[[noreturn]] void pauseHere(const QString& point)
{
    const QByteArray line = QByteArrayLiteral("ASB-PAUSED ") + point.toLocal8Bit() + '\n';
    const ssize_t w = ::write(STDERR_FILENO, line.constData(), size_t(line.size()));
    (void)w;
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

QStringList failPoints()
{
    return QString::fromLocal8Bit(qgetenv("ASB_TEST_FAIL")).split(QLatin1Char(','), Qt::SkipEmptyParts);
}

void maybePause(const QString& point)
{
    if (pausePoints().contains(point)) pauseHere(point);
}

// ASB_TEST_FAIL (comma separated): firewall:<method> throws, as a failing native call would.
void maybeFail(const QString& point)
{
    if (failPoints().contains(point))
        throw PlatformError(QStringLiteral("injected failure at %1 (ASB_TEST_FAIL)").arg(point).toStdString());
}

QByteArray resultJson(const Result& r, const QStringList& reports, asb::posix::LinuxPlatform& p)
{
    QJsonObject o;
    o[QStringLiteral("outcome")] = enumName(r.outcome);
    o[QStringLiteral("persisted")] = enumName(r.persisted);
    o[QStringLiteral("persistedSha256")] = QString::fromLatin1(r.persistedSha256);
    o[QStringLiteral("step")] = r.step;
    o[QStringLiteral("message")] = r.message;
    o[QStringLiteral("postcondition")] = r.postcondition;
    o[QStringLiteral("fileChanges")] = QJsonArray::fromStringList(r.fileChanges);
    o[QStringLiteral("serviceChanges")] = QJsonArray::fromStringList(reports);
    o[QStringLiteral("applyLock")] = p.applyLock().refusalReason();
    o[QStringLiteral("engineLock")] = p.engineLock().lastError();
    o[QStringLiteral("holder")] = p.engineLock().holderDescription();
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

}  // namespace asb::app::testseams
