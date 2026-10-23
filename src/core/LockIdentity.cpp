#include "core/LockIdentity.h"

#include <QRandomGenerator>
#include <QRegularExpression>

namespace asb::LockIdentity {

QByteArray newId()
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QByteArray hex;
    for (quint32 w : words) hex += QByteArray::number(w, 16).rightJustified(8, '0');
    return hex;
}

QByteArray format(const QByteArray& id)
{
    return QByteArrayLiteral("{\"lockId\":\"") + id + QByteArrayLiteral("\"}");
}

std::optional<QByteArray> parse(const QByteArray& bytes)
{
    if (bytes.isEmpty() || bytes.size() > kMaxBytes || bytes.contains('\0')) return std::nullopt;
    static const QRegularExpression re(QStringLiteral(R"re(\A\{"lockId":"([0-9a-f]{32})"\}\z)re"));
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(bytes));
    if (!m.hasMatch()) return std::nullopt;
    return m.captured(1).toLatin1();
}

PairState classify(const LockFileRead& lock, const LockFileRead& record)
{
    using S = LockFileRead::Status;
    const bool recordMissing = record.status == S::Missing;
    const std::optional<QByteArray> recordId =
        record.status == S::Ok ? parse(record.bytes) : std::nullopt;   // Failed/Unreadable/corrupt -> nullopt
    switch (lock.status) {
    case S::Missing:
        return recordMissing ? PairState::Fresh : PairState::RecordOnly;
    case S::Unreadable:
        if (recordMissing) return PairState::Partial;
        return recordId ? PairState::Unknown : PairState::Mismatched;
    case S::Failed:
        return PairState::Mismatched;
    case S::Ok:
        break;
    }
    const std::optional<QByteArray> lockId = parse(lock.bytes);
    if (!lockId) return PairState::Mismatched;
    if (recordMissing) return PairState::Partial;
    if (!recordId) return PairState::Mismatched;
    return *lockId == *recordId ? PairState::Matching : PairState::Mismatched;
}

QString describe(PairState s)
{
    switch (s) {
    case PairState::Fresh: return QStringLiteral("neither engine.lock nor engine.lock.id exists");
    case PairState::Partial: return QStringLiteral("engine.lock exists but engine.lock.id is missing");
    case PairState::RecordOnly: return QStringLiteral("engine.lock.id exists but engine.lock is missing");
    case PairState::Mismatched: return QStringLiteral("engine.lock and engine.lock.id do not match");
    case PairState::Matching: return QStringLiteral("engine.lock and engine.lock.id match");
    case PairState::Unknown: return QStringLiteral("engine.lock cannot be read without holding it");
    }
    return {};
}

Recovery recoveryFor(PairState s)
{
    switch (s) {
    case PairState::Fresh: return Recovery::PublishLockThenRecord;
    case PairState::Partial: return Recovery::PublishRecordFromHeldLock;
    case PairState::RecordOnly:
    case PairState::Mismatched: return Recovery::ReplaceBoth;
    case PairState::Matching:
    case PairState::Unknown: return Recovery::None;
    }
    return Recovery::None;
}

WriteResult publish(FileIo& io, const DirHandle& dir, const QString& name, const QByteArray& id)
{
    return atomicWrite(io, dir, name, format(id), OwnerPolicy::Config);
}

}  // namespace asb::LockIdentity
