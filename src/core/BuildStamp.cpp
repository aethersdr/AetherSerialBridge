#include "core/BuildStamp.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

#include <cstring>

namespace asb::core {
namespace {

// Bounds on what the reader will believe: no real executable has more sections or a longer section-name table.
constexpr quint64 kMaxSections = 4096;
constexpr quint64 kMaxNameTable = 1 << 20;

// One read of exactly `length` bytes at `offset`, both checked against the file size first (no overflow).
std::optional<QByteArray> exact(qint64 size, const ReadAt& readAt, quint64 offset, quint64 length)
{
    if (size < 0 || offset > quint64(size) || length > quint64(size) - offset) return std::nullopt;
    QByteArray b = readAt(qint64(offset), qint64(length));
    if (quint64(b.size()) != length) return std::nullopt;
    return b;
}

quint64 field(const QByteArray& b, qsizetype at, int width, bool bigEndian)
{
    quint64 v = 0;
    for (int i = 0; i < width; ++i) {
        const quint64 byte = quint8(b.at(at + (bigEndian ? i : width - 1 - i)));
        v = (v << 8) | byte;
    }
    return v;
}

std::optional<BuildStamp> fromRecord(const QByteArray& record)
{
    if (quint64(record.size()) != kBuildStampSize) return std::nullopt;
    if (std::memcmp(record.constData(), kBuildStampMagic, sizeof kBuildStampMagic) != 0) return std::nullopt;
    const QByteArray body = record.mid(qsizetype(sizeof kBuildStampMagic));
    const qsizetype end = body.indexOf('\0');
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(end < 0 ? body : body.left(end), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("schema")).toInt(-1) != 1) return std::nullopt;
    if (o.value(QStringLiteral("product")).toString() != QLatin1String(kBuildStampProduct)) return std::nullopt;
    const QString version = o.value(QStringLiteral("version")).toString();
    static const QRegularExpression sane(QStringLiteral("^[0-9A-Za-z.+-]{1,64}$"));
    if (!sane.match(version).hasMatch()) return std::nullopt;
    return BuildStamp{version};
}

// ELF: the section header table (e_shoff, e_shentsize, e_shnum) and the section-name table (e_shstrndx).
std::optional<BuildStamp> fromElf(qint64 size, const ReadAt& readAt, const QByteArray& ident)
{
    const bool is64 = ident.at(4) == 2;
    if (!is64 && ident.at(4) != 1) return std::nullopt;
    const bool be = ident.at(5) == 2;
    if (!be && ident.at(5) != 1) return std::nullopt;
    const auto header = exact(size, readAt, 0, is64 ? 64 : 52);
    if (!header) return std::nullopt;
    const QByteArray& h = *header;
    const quint64 shoff = is64 ? field(h, 0x28, 8, be) : field(h, 0x20, 4, be);
    const quint64 shentsize = field(h, is64 ? 0x3A : 0x2E, 2, be);
    const quint64 shnum = field(h, is64 ? 0x3C : 0x30, 2, be);
    const quint64 shstrndx = field(h, is64 ? 0x3E : 0x32, 2, be);
    // An extended count (shnum 0, SHN_XINDEX) is not something our executables produce: refused, never guessed.
    if (shnum == 0 || shnum > kMaxSections || shentsize != (is64 ? 64u : 40u) || shstrndx >= shnum) return std::nullopt;
    const auto table = exact(size, readAt, shoff, shnum * shentsize);
    if (!table) return std::nullopt;
    struct Section {
        quint64 name, type, offset, size;
    };
    auto section = [&](quint64 i) {
        const qsizetype at = qsizetype(i * shentsize);
        return is64 ? Section{field(*table, at, 4, be), field(*table, at + 4, 4, be), field(*table, at + 0x18, 8, be),
                              field(*table, at + 0x20, 8, be)}
                    : Section{field(*table, at, 4, be), field(*table, at + 4, 4, be), field(*table, at + 0x10, 4, be),
                              field(*table, at + 0x14, 4, be)};
    };
    const Section strtab = section(shstrndx);
    if (strtab.size > kMaxNameTable) return std::nullopt;
    const auto names = exact(size, readAt, strtab.offset, strtab.size);
    if (!names) return std::nullopt;
    const QByteArray want = QByteArray(kBuildStampElfSection) + '\0';
    for (quint64 i = 0; i < shnum; ++i) {
        const Section s = section(i);
        if (s.name >= quint64(names->size()) || names->mid(qsizetype(s.name), want.size()) != want) continue;
        // SHT_PROGBITS, at least one whole record, inside the file.
        if (s.type != 1 || s.size < kBuildStampSize) return std::nullopt;
        const auto record = exact(size, readAt, s.offset, kBuildStampSize);
        if (!record) return std::nullopt;
        return fromRecord(*record);
    }
    return std::nullopt;
}

// PE: e_lfanew -> "PE\0\0", the COFF header (NumberOfSections, SizeOfOptionalHeader), then the section table.
std::optional<BuildStamp> fromPe(qint64 size, const ReadAt& readAt)
{
    const auto dos = exact(size, readAt, 0, 0x40);
    if (!dos) return std::nullopt;
    const quint64 lfanew = field(*dos, 0x3C, 4, false);
    const auto coff = exact(size, readAt, lfanew, 24);
    if (!coff || !coff->startsWith(QByteArray("PE\0\0", 4))) return std::nullopt;
    const quint64 count = field(*coff, 4 + 2, 2, false);
    const quint64 optional = field(*coff, 4 + 16, 2, false);
    if (count == 0 || count > kMaxSections) return std::nullopt;
    const auto table = exact(size, readAt, lfanew + 24 + optional, count * 40);
    if (!table) return std::nullopt;
    const QByteArray want(kBuildStampPeSection, 8);
    for (quint64 i = 0; i < count; ++i) {
        const qsizetype at = qsizetype(i * 40);
        if (table->mid(at, 8) != want) continue;
        const quint64 virtualSize = field(*table, at + 8, 4, false);
        const quint64 rawSize = field(*table, at + 16, 4, false);
        const quint64 rawOffset = field(*table, at + 20, 4, false);
        if (virtualSize < kBuildStampSize || rawSize < kBuildStampSize) return std::nullopt;
        const auto record = exact(size, readAt, rawOffset, kBuildStampSize);
        if (!record) return std::nullopt;
        return fromRecord(*record);
    }
    return std::nullopt;
}

}  // namespace

std::optional<BuildStamp> parseBuildStamp(qint64 size, const ReadAt& readAt)
{
    const auto ident = exact(size, readAt, 0, 16);
    if (!ident) return std::nullopt;
    if (ident->startsWith("\x7f" "ELF")) return fromElf(size, readAt, *ident);
    if (ident->startsWith("MZ")) return fromPe(size, readAt);
    return std::nullopt;
}

std::optional<BuildStamp> readBuildStamp(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const qint64 size = f.size();
    return parseBuildStamp(size, [&f](qint64 offset, qint64 length) {
        if (!f.seek(offset)) return QByteArray();
        return f.read(length);
    });
}

std::optional<BuildStamp> buildStampOf(const QByteArray& image)
{
    return parseBuildStamp(image.size(), [&image](qint64 offset, qint64 length) {
        return image.mid(qsizetype(offset), qsizetype(length));
    });
}

QString describeInstalledVersion(const std::optional<QString>& installedVersion, const QString& thisVersion)
{
    if (!installedVersion) return {};
    if (installedVersion->isEmpty()) return QStringLiteral("an earlier build, before 1.1");
    const QVersionNumber installed = QVersionNumber::fromString(*installedVersion);
    const QVersionNumber mine = QVersionNumber::fromString(thisVersion);
    if (!installed.isNull() && !mine.isNull() && QVersionNumber::compare(installed, mine) > 0)
        return QStringLiteral("a newer build, %1").arg(*installedVersion);
    return QStringLiteral("version %1").arg(*installedVersion);
}

}  // namespace asb::core
