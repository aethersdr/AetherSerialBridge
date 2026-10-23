#include "core/AtomicJson.h"

#include <QRandomGenerator>
#include <QRegularExpression>

namespace asb {

NameGen defaultNameGen(const QString& base)
{
    return [base]() {
        quint32 words[4];
        QRandomGenerator::system()->fillRange(words);
        QString hex;
        for (quint32 w : words) hex += QStringLiteral("%1").arg(w, 8, 16, QLatin1Char('0'));
        return base + QLatin1Char('.') + hex + QLatin1String(kTempSuffix);
    };
}

bool isTempName(const QString& name)
{
    static const QRegularExpression re(QStringLiteral(R"(^.+\.[0-9a-f]{32}\.asbtmp\z)"));
    return re.match(name).hasMatch();
}

WriteResult atomicWrite(FileIo& io, const DirHandle& dir, const QString& name, const QByteArray& bytes,
                        OwnerPolicy policy, const NameGen& gen)
{
    WriteResult r;
    // The temp is exclusively created at a fresh name: a planted file or link at that name makes
    // createTemp fail; it is never reused, followed or removed (INV-15).
    std::unique_ptr<TempFile> temp = io.createTemp(dir, policy, gen ? gen : defaultNameGen(name), &r.error);
    if (!temp) return r;
    // Data durable before the rename publishes it (INV-77). Any failure until commitAs leaves the
    // target as it was; `temp` going out of scope removes the temp file.
    if (!temp->write(bytes, &r.error)) return r;
    if (!temp->flush(&r.error)) return r;
    const TempFile::Commit c = temp->commitAs(name);
    r.replaced = c.replaced;
    r.durable = c.replaced && c.durable;
    r.error = c.error;
    return r;
}

}  // namespace asb
