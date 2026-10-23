#pragma once

// Shared set-up for the portable WS-6 tests: a FakeNtSeams machine with C:\ProgramData (the real default
// DACL: Users may create folders there) and C:\Program Files (TrustedInstaller-owned, CREATOR OWNER
// inherit-only, as on windows-latest), plus descriptor helpers.

#include "platform/win/SecurityDescriptor.h"
#include "support/FakeNtSeams.h"

namespace asb::test::win6 {

namespace sd = asb::win::sd;

inline const QString kSy = QString::fromLatin1(sd::kSystem);
inline const QString kBa = QString::fromLatin1(sd::kAdministrators);
inline const QString kBu = QString::fromLatin1(sd::kUsers);
inline const QString kUser = QStringLiteral("S-1-5-21-1000-2000-3000-1001");
inline const QString kData = QStringLiteral("C:\\ProgramData\\AetherSerialBridge");
inline const QString kLockDir = QStringLiteral("C:\\Program Files\\AetherSerialBridge");

inline sd::Ace allow(const QString& sid, quint32 mask, quint8 flags = 0)
{
    return sd::Ace{sd::kAccessAllowed, flags, mask, sid};
}

inline QByteArray descriptor(const QString& owner, QList<sd::Ace> aces, bool isProtected = false)
{
    sd::Descriptor d;
    d.daclPresent = true;
    d.daclProtected = isProtected;
    d.dacl = std::move(aces);
    return sd::build(owner, d);
}

// ACE lists as text, for QCOMPARE with a readable failure on every Qt version.
inline QString show(const QList<sd::Ace>& aces)
{
    sd::Descriptor d;
    d.daclPresent = true;
    d.dacl = aces;
    return sd::toDisplay(d);
}

inline sd::Descriptor parsed(const QByteArray& b)
{
    const std::optional<sd::Descriptor> d = sd::parse(b);
    return d ? *d : sd::Descriptor{};
}

// Changes the type byte of ACE number `index` in a self-relative descriptor (an undecodable ACE kind).
inline QByteArray withAceType(QByteArray b, int index, quint8 type)
{
    const qsizetype dacl = qsizetype(quint8(b[16]) | (quint8(b[17]) << 8) | (quint8(b[18]) << 16) | (quint8(b[19]) << 24));
    qsizetype at = dacl + 8;
    for (int i = 0; i < index; ++i) at += quint8(b[at + 2]) | (quint8(b[at + 3]) << 8);
    b[at] = char(type);
    return b;
}

inline void setUpMachine(FakeNtSeams& fs)
{
    const quint8 oici = sd::kObjectInherit | sd::kContainerInherit;
    fs.mkdir(QStringLiteral("C:\\ProgramData"),
             descriptor(kSy,
                        {allow(kSy, sd::kFullControl, oici), allow(kBa, sd::kFullControl, oici),
                         allow(QString::fromLatin1(sd::kCreatorOwner), 0x10000000, oici | sd::kInheritOnly),
                         allow(kBu, sd::kReadExecute, oici), allow(kBu, 0x116 /*add file/subdir, write ea/attr*/, sd::kContainerInherit)},
                        true));
    const QString ti = QString::fromLatin1(sd::kTrustedInstaller);
    fs.mkdir(QStringLiteral("C:\\Program Files"),
             descriptor(ti,
                        {allow(ti, sd::kFullControl), allow(ti, 0x10000000, sd::kContainerInherit | sd::kInheritOnly),
                         allow(kSy, 0x1301BF), allow(kSy, 0x10000000, oici | sd::kInheritOnly), allow(kBa, 0x1301BF),
                         allow(kBa, 0x10000000, oici | sd::kInheritOnly), allow(kBu, sd::kReadExecute),
                         allow(kBu, 0xA0000000, oici | sd::kInheritOnly),
                         allow(QString::fromLatin1(sd::kCreatorOwner), 0x10000000, oici | sd::kInheritOnly)},
                        true));
}

}  // namespace asb::test::win6
