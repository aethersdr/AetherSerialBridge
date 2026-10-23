#include "core/StartToken.h"

#include "core/AtomicJson.h"
#include "core/ConfigFiles.h"

#include <QRandomGenerator>
#include <QRegularExpression>

namespace asb {
namespace StartToken {

QByteArray format(const QByteArray& token, const FirewallGeneration& g)
{
    return QByteArrayLiteral("{\"token\":\"") + token + QByteArrayLiteral("\",\"generation\":\"") + g.id +
           QByteArrayLiteral("\"}");
}

std::optional<IStartToken::Token> parse(const QByteArray& bytes, QString* why)
{
    auto fail = [why](const QString& w) {
        if (why) *why = w;
        return std::nullopt;
    };
    if (bytes.isEmpty()) return fail(QStringLiteral("start.token is empty"));
    if (bytes.size() > kMaxBytes) return fail(QStringLiteral("start.token is larger than %1 bytes").arg(kMaxBytes));
    if (bytes == QByteArray(kRevoked)) return fail(QStringLiteral("start.token was revoked"));
    static const QRegularExpression re(
        QStringLiteral(R"re(\A\{"token":"([0-9a-f]{32})","generation":"([0-9a-f]{16})"\}\z)re"));
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(bytes));
    if (!m.hasMatch() || bytes.contains('\0')) return fail(QStringLiteral("start.token is malformed"));
    return IStartToken::Token{m.captured(1).toLatin1(), FirewallGeneration{m.captured(2).toLatin1()}};
}

QByteArray newToken()
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QByteArray hex;
    for (quint32 w : words) hex += QByteArray::number(w, 16).rightJustified(8, '0');
    return hex;
}

}  // namespace StartToken

IStartToken::Revocation FileStartToken::revoke(Deadline, QString* err)
{
    const QString name = QString::fromLatin1(ConfigFiles::kStartToken);
    QString e;
    std::unique_ptr<ExistingFile> f = m_io.openExisting(m_folder.handle(), name, &e);
    if (!f) {
        if (e == fileNotFoundError()) return Revocation::Absent;
        if (err) *err = e;
        return Revocation::NotOpened;
    }
    // From here on the visible bytes may already be invalid; every partial state is refused by read().
    if (!f->truncate(&e) || !f->writeAll(QByteArray(StartToken::kRevoked), &e) || !f->flush(&e)) {
        if (err) *err = e;
        return Revocation::NotFlushed;
    }
    // Revocation is complete: a resurrected entry holds the revoked form. Deletion and the directory
    // flush are best effort.
    if (f->removeByHandle(&e)) m_io.syncDir(m_folder.handle(), &e);
    return Revocation::Done;
}

IStartToken::Written FileStartToken::writeFresh(const FirewallGeneration& g, Deadline)
{
    Written w;
    w.token = StartToken::newToken();
    const WriteResult r = atomicWrite(m_io, m_folder.handle(), QString::fromLatin1(ConfigFiles::kStartToken),
                                      StartToken::format(w.token, g), OwnerPolicy::Config, m_gen);
    w.replaced = r.replaced;
    w.durable = r.durable;
    w.error = r.error;
    return w;
}

std::optional<IStartToken::Token> FileStartToken::read(QString* why)
{
    const Inspection insp = m_folder.inspect(TrustMode::Service);
    if (insp.action != DirAction::Trust) {
        if (why) *why = QStringLiteral("the data folder is not trusted");
        return std::nullopt;
    }
    QString err;
    const std::optional<QByteArray> bytes =
        m_folder.readFile(insp, QString::fromLatin1(ConfigFiles::kStartToken), StartToken::kMaxBytes, &err);
    if (!bytes) {
        if (why) *why = err == fileNotFoundError() ? QStringLiteral("start.token is missing") : QStringLiteral("start.token: %1").arg(err);
        return std::nullopt;
    }
    return StartToken::parse(*bytes, why);
}

}  // namespace asb
