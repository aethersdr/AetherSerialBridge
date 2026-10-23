#include "core/ConfigFiles.h"

#include "core/ConfigJson.h"

namespace asb::ConfigFiles {
namespace {

Loaded read(const Reader& reader, const QString& file, bool missingIsEmpty)
{
    Loaded l;
    const RawRead raw = reader(file, kConfigMaxBytes);
    switch (raw.status) {
    case RawRead::Status::Missing:
        if (missingIsEmpty) l.config = BridgeConfig{};
        return l;
    case RawRead::Status::Failed:
        l.present = true;
        l.error = QStringLiteral("Could not read %1: %2").arg(file, raw.error);
        return l;
    case RawRead::Status::Ok:
        break;
    }
    l.present = true;
    l.bytes = raw.bytes;
    if (raw.bytes.size() > kConfigMaxBytes) {
        l.error = QStringLiteral("Could not read %1: larger than %2 bytes").arg(file).arg(kConfigMaxBytes);
        return l;
    }
    QString why;
    l.config = fromJson(raw.bytes, &why);
    if (!l.config) l.error = QStringLiteral("Could not read %1: %2").arg(file, why);
    return l;
}

}  // namespace

Loaded readCommitted(const Reader& reader)
{
    return read(reader, QString::fromLatin1(kConfig), true);
}

Loaded readPending(const Reader& reader)
{
    return read(reader, QString::fromLatin1(kPending), false);
}

Loaded readNamed(const Reader& reader, const QString& name)
{
    return read(reader, name, true);
}

WriteResult writePending(FileIo& io, const DirHandle& dir, const QByteArray& bytes, const NameGen& gen)
{
    return atomicWrite(io, dir, QString::fromLatin1(kPending), bytes, OwnerPolicy::Config, gen);
}

}  // namespace asb::ConfigFiles
