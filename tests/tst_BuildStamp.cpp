// The build stamp (v1.1 D61, INV-120): every executable carries one in its own section; the reader finds it through
// the file's section table only, bounded by the file's size, and a program without the section (every v1.0 build)
// has none. The stamp is wording only: tst_InstallationEvidence::stampNeverChangesClassification.

#include "core/BuildInfo.h"
#include "core/BuildStamp.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <cstring>

using namespace asb::core;

namespace {

QByteArray record(const QByteArray& json, const QByteArray& magic = QByteArray(kBuildStampMagic, 16))
{
    QByteArray r = magic + json;
    r.resize(qsizetype(kBuildStampSize), '\0');
    return r;
}

QByteArray goodJson(const char* version = "1.1.0")
{
    return QByteArray(R"({"schema":1,"product":"aether-serial-bridge","version":")") + version + "\"}";
}

template <typename T>
void put(QByteArray& b, qsizetype at, T v)
{
    if (b.size() < at + qsizetype(sizeof(T))) b.resize(at + qsizetype(sizeof(T)), '\0');
    qToLittleEndian(v, b.data() + at);
}

// A minimal ELF64 (little-endian) with sections: null, <name> (PROGBITS, `body`), .shstrtab.
struct ElfSpec {
    QByteArray name = ".asb_stamp";
    QByteArray body = record(goodJson());
    quint64 sectionSize = 0;      // 0: body's size
    quint64 sectionOffset = 0;    // 0: where the body is
    quint64 shoff = 0;            // 0: where the table is
    quint32 type = 1;             // SHT_PROGBITS
};
QByteArray elf(const ElfSpec& s)
{
    QByteArray b(64, '\0');
    std::memcpy(b.data(), "\x7f" "ELF", 4);
    b[4] = 2;   // ELFCLASS64
    b[5] = 1;   // little-endian
    b[6] = 1;
    put<quint16>(b, 16, 2);       // ET_EXEC
    put<quint16>(b, 18, 62);      // x86-64
    put<quint32>(b, 20, 1);
    put<quint16>(b, 52, 64);
    const qsizetype bodyAt = b.size();
    b += s.body;
    const qsizetype namesAt = b.size();
    const QByteArray names = QByteArray("\0", 1) + s.name + QByteArray("\0", 1) + ".shstrtab" + QByteArray("\0", 1);
    b += names;
    while (b.size() % 8) b += '\0';
    const qsizetype tableAt = b.size();
    b.resize(tableAt + 3 * 64, '\0');
    auto sh = [&](int i, quint32 name, quint32 type, quint64 off, quint64 size) {
        const qsizetype at = tableAt + i * 64;
        put<quint32>(b, at, name);
        put<quint32>(b, at + 4, type);
        put<quint64>(b, at + 24, off);
        put<quint64>(b, at + 32, size);
    };
    sh(1, 1, s.type, s.sectionOffset ? s.sectionOffset : quint64(bodyAt), s.sectionSize ? s.sectionSize : quint64(s.body.size()));
    sh(2, quint32(1 + s.name.size() + 1), 3, quint64(namesAt), quint64(names.size()));
    put<quint64>(b, 40, s.shoff ? s.shoff : quint64(tableAt));
    put<quint16>(b, 58, 64);   // e_shentsize
    put<quint16>(b, 60, 3);    // e_shnum
    put<quint16>(b, 62, 2);    // e_shstrndx
    return b;
}

// A minimal PE32+: DOS header, "PE\0\0", the COFF header, one section <name> whose raw data is `body`.
QByteArray pe(const QByteArray& name, const QByteArray& body, quint32 rawOffset = 0x200, quint32 virtualSize = 256)
{
    QByteArray b(0x40, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    put<quint32>(b, 0x3C, 0x40);
    b += QByteArray("PE\0\0", 4);
    put<quint16>(b, 0x44, 0x8664);   // Machine
    put<quint16>(b, 0x46, 1);        // NumberOfSections
    put<quint16>(b, 0x54, 0);        // SizeOfOptionalHeader
    put<quint16>(b, 0x56, 0x22);
    b.resize(0x58 + 40, '\0');
    QByteArray n = name.left(8);
    n.resize(8, '\0');
    std::memcpy(b.data() + 0x58, n.constData(), 8);
    put<quint32>(b, 0x58 + 8, virtualSize);
    put<quint32>(b, 0x58 + 12, 0x1000);
    put<quint32>(b, 0x58 + 16, 0x200);
    put<quint32>(b, 0x58 + 20, rawOffset);
    b.resize(0x200, '\0');
    QByteArray raw = body;
    raw.resize(0x200, '\0');
    b += raw;
    return b;
}

QByteArray ownImage()
{
    QFile f(QCoreApplication::applicationFilePath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString thisVersion()
{
    return QString::fromLatin1(version().data(), qsizetype(version().size()));
}

}  // namespace

class tst_BuildStamp : public QObject {
    Q_OBJECT

private slots:
    // This test executable links the stamp (BuildInfo.cpp), as every product executable does: the reader finds it in
    // the file, as written, through the file's own section table (ELF on Linux, PE on Windows).
    void readsOwnStamp()
    {
        const QByteArray mine = ownBuildStampRecord();
        QCOMPARE(mine.size(), qsizetype(kBuildStampSize));
        QVERIFY(mine.startsWith(QByteArray(kBuildStampMagic, 16)));
        const std::optional<BuildStamp> s = readBuildStamp(QCoreApplication::applicationFilePath());
        QVERIFY2(s.has_value(), "this executable has no readable build stamp");
        QCOMPARE(s->version, thisVersion());
        const std::optional<BuildStamp> fromBytes = buildStampOf(ownImage());
        QVERIFY(fromBytes.has_value());
        QCOMPARE(fromBytes->version, thisVersion());
        QCOMPARE(buildStampOf(elf({}))->version, QStringLiteral("1.1.0"));
    }

    // A program without the section (every v1.0 build) has no stamp: "an earlier build". The record's bytes elsewhere
    // in the file are never found by scanning.
    void v10ProgramHasNone()
    {
        QByteArray image = ownImage();
        QVERIFY(!image.isEmpty());
        image.replace(QByteArray(kBuildStampElfSection), QByteArray(".asb_stamq"));
        image.replace(QByteArray(kBuildStampPeSection, 8), QByteArray(".asbstmq"));
        QVERIFY(image.contains(QByteArray(kBuildStampMagic, 16)));   // the record is still in the file
        QVERIFY(!buildStampOf(image).has_value());
        ElfSpec other;
        other.name = ".data";
        QVERIFY(!buildStampOf(elf(other)).has_value());
        QVERIFY(!buildStampOf(QByteArray(4096, 'x')).has_value());
        QVERIFY(!buildStampOf(QByteArray()).has_value());
        QTemporaryDir d;
        QVERIFY(!readBuildStamp(d.filePath(QStringLiteral("missing"))).has_value());
    }

    // A stamp section shorter than one record, a record cut off by the end of the file, a damaged record: refused.
    void truncatedSectionRefused()
    {
        ElfSpec shortSection;
        shortSection.sectionSize = 100;
        QVERIFY(!buildStampOf(elf(shortSection)).has_value());
        const QByteArray whole = elf({});
        const qsizetype at = whole.indexOf(QByteArray(kBuildStampMagic, 16));
        QVERIFY(at > 0);
        // The section table says a record starts 100 bytes before the end of the file: it is cut off.
        ElfSpec cut;
        cut.sectionOffset = quint64(whole.size() - 100);
        QVERIFY(!buildStampOf(elf(cut)).has_value());
        QVERIFY(!buildStampOf(whole.left(at + 100)).has_value());   // the file ends inside the record (and the table)
        ElfSpec badMagic;
        badMagic.body = record(goodJson(), QByteArray("ASB-BUILD-STAMQ\0", 16));
        QVERIFY(!buildStampOf(elf(badMagic)).has_value());
        ElfSpec badJson;
        badJson.body = record(R"({"schema":2,"product":"aether-serial-bridge","version":"1.1.0"})");
        QVERIFY(!buildStampOf(elf(badJson)).has_value());
        badJson.body = record(R"({"schema":1,"product":"something-else","version":"1.1.0"})");
        QVERIFY(!buildStampOf(elf(badJson)).has_value());
        badJson.body = record(R"({"schema":1,"product":"aether-serial-bridge","version":"1.1.0 && x"})");
        QVERIFY(!buildStampOf(elf(badJson)).has_value());
        ElfSpec nobits;
        nobits.type = 8;   // SHT_NOBITS: no bytes in the file
        QVERIFY(!buildStampOf(elf(nobits)).has_value());
    }

    // Offsets the file gives that point outside it (the section table, the section, an overflow) are refused, never
    // followed.
    void offsetsOutsideFileRefused()
    {
        ElfSpec table;
        table.shoff = 1u << 30;
        QVERIFY(!buildStampOf(elf(table)).has_value());
        ElfSpec section;
        section.sectionOffset = 1u << 30;
        QVERIFY(!buildStampOf(elf(section)).has_value());
        ElfSpec overflow;
        overflow.sectionOffset = ~quint64(0) - 16;
        QVERIFY(!buildStampOf(elf(overflow)).has_value());
        overflow.shoff = ~quint64(0) - 8;
        QVERIFY(!buildStampOf(elf(overflow)).has_value());
        QVERIFY(!buildStampOf(pe(".asbstmp", record(goodJson()), 0x7fff0000)).has_value());
        QByteArray lfanew = pe(".asbstmp", record(goodJson()));
        put<quint32>(lfanew, 0x3C, 0x7fffffff);
        QVERIFY(!buildStampOf(lfanew).has_value());
    }

    // PE: the ".asbstmp" section is found through the section table (the Windows executables, MSVC).
    void peSectionFound()
    {
        const std::optional<BuildStamp> s = buildStampOf(pe(".asbstmp", record(goodJson("1.2.0"))));
        QVERIFY(s.has_value());
        QCOMPARE(s->version, QStringLiteral("1.2.0"));
        QVERIFY(!buildStampOf(pe(".rdata", record(goodJson()))).has_value());
        QVERIFY(!buildStampOf(pe(".asbstmp", record(goodJson()), 0x200, 100)).has_value());   // shorter than a record
        QByteArray noPe = pe(".asbstmp", record(goodJson()));
        noPe[0x41] = 'X';
        QVERIFY(!buildStampOf(noPe).has_value());
    }

    // The question's wording (D61): an earlier build, this or an older version, a newer build.
    void describesTheInstalledVersion_data()
    {
        QTest::addColumn<bool>("read");
        QTest::addColumn<QString>("installed");
        QTest::addColumn<QString>("mine");
        QTest::addColumn<QString>("words");
        QTest::newRow("not read") << false << QString() << "1.1.0" << QString();
        QTest::newRow("no stamp") << true << QString() << "1.1.0" << "an earlier build, before 1.1";
        QTest::newRow("same") << true << "1.1.0" << "1.1.0" << "version 1.1.0";
        QTest::newRow("older") << true << "1.1.0" << "1.2.0" << "version 1.1.0";
        QTest::newRow("newer") << true << "1.2.0" << "1.1.0" << "a newer build, 1.2.0";
    }
    void describesTheInstalledVersion()
    {
        QFETCH(bool, read);
        QFETCH(QString, installed);
        QFETCH(QString, mine);
        QFETCH(QString, words);
        const std::optional<QString> v = read ? std::optional<QString>(installed) : std::nullopt;
        QCOMPARE(describeInstalledVersion(v, mine), words);
    }
};

QTEST_GUILESS_MAIN(tst_BuildStamp)
#include "tst_BuildStamp.moc"
