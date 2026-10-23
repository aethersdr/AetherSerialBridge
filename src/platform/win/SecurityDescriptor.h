#pragma once

// Self-relative security descriptors as bytes (WS-6; INV-16, INV-19, INV-97, INV-104). Pure: no Windows
// headers, so the decoding the trust decision rests on, and the descriptors the adapters pass to the
// create calls, are tested on every OS. The native layer (NtSeams.cpp) only moves these bytes between
// the kernel (GetSecurityInfo / SetKernelObjectSecurity / the SECURITY_ATTRIBUTES of a create) and here.
//
// Decoding rules (INV-19): an ACE whose type is not a basic ACCESS_ALLOWED (0) or ACCESS_DENIED (1) is
// AceKind::Unknown, untrusted whatever its SID or mask; a NULL DACL (present, no ACL) or an absent DACL
// grants everyone everything and decodes as one Unknown entry; inherit-only entries are kept (they are
// what new children receive); anything malformed fails the whole decode, which the adapters treat as an
// unreadable descriptor (untrusted).

#include "core/AceJudge.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace asb::win::sd {

// ACE types and flags (winnt.h values).
inline constexpr quint8 kAccessAllowed = 0x0;
inline constexpr quint8 kAccessDenied = 0x1;
inline constexpr quint8 kObjectInherit = 0x1;
inline constexpr quint8 kContainerInherit = 0x2;
inline constexpr quint8 kNoPropagate = 0x4;
inline constexpr quint8 kInheritOnly = 0x8;
inline constexpr quint8 kInherited = 0x10;

// SECURITY_DESCRIPTOR_CONTROL bits.
inline constexpr quint16 kDaclPresent = 0x0004;
inline constexpr quint16 kDaclAutoInherited = 0x0400;
inline constexpr quint16 kDaclProtected = 0x1000;
inline constexpr quint16 kSelfRelative = 0x8000;

// Well-known SIDs.
inline constexpr char kSystem[] = "S-1-5-18";
inline constexpr char kAdministrators[] = "S-1-5-32-544";
inline constexpr char kUsers[] = "S-1-5-32-545";
inline constexpr char kCreatorOwner[] = "S-1-3-0";
inline constexpr char kTrustedInstaller[] = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

// File access masks.
inline constexpr quint32 kFullControl = 0x1F01FF;    // FILE_ALL_ACCESS
inline constexpr quint32 kReadExecute = 0x1200A9;    // FILE_GENERIC_READ | FILE_GENERIC_EXECUTE

struct Ace {
    quint8 type = kAccessAllowed;
    quint8 flags = 0;
    quint32 mask = 0;
    QString sid;            // empty when the type is not one we decode (Unknown)
    AceKind kind() const;
    bool operator==(const Ace&) const = default;
};

struct Descriptor {
    std::optional<QString> owner;   // nullopt: no owner in the descriptor
    bool daclPresent = false;
    bool daclNull = false;          // present but no ACL: everyone has every right
    bool daclProtected = false;
    bool daclAutoInherited = false;
    QList<Ace> dacl;
};

// "S-1-5-32-544" <-> the binary SID. nullopt on anything malformed.
std::optional<QByteArray> sidToBinary(const QString&);
std::optional<QString> sidFromBinary(const QByteArray&);

// A self-relative descriptor. nullopt when malformed (bounds, revision, counts, sizes); *error says why.
std::optional<Descriptor> parse(const QByteArray& selfRelative, QString* error = nullptr);

// Builds a self-relative descriptor: the owner (empty = none) and, when `dacl` is set, a DACL with the
// control bits of the Descriptor (protected, auto-inherited). Every ACE must be a basic allow/deny with a
// valid SID; an invalid input gives an empty QByteArray.
QByteArray build(const QString& owner, const std::optional<Descriptor>& dacl = std::nullopt);

// The judge's view (AceJudge): a NULL or absent DACL, and every non-basic ACE, become AceKind::Unknown.
QList<AceInfo> toAceInfos(const Descriptor&);

// The DACL a new regular file in a folder with `parentDacl` receives (NTFS inheritance for a leaf):
// every entry with OBJECT_INHERIT, with the inheritance flags cleared and INHERITED set; generic rights
// mapped to file rights; CREATOR OWNER replaced by `owner`.
QList<Ace> inheritedForFile(const QList<Ace>& parentDacl, const QString& owner);

// The data folder (master §4.4): owner Administrators; protected; SYSTEM and
// Administrators full control, Users read and execute, each inherited by files and subfolders.
QByteArray dataFolderDescriptor();
// apply.lock / engine.lock / engine.lock.id (INV-97, INV-104): owner Administrators; protected; SYSTEM and
// Administrators full control only; no inheritance flags.
QByteArray lockFileDescriptor();
// The lock directory (%ProgramFiles%\AetherSerialBridge): owner Administrators; protected; SYSTEM and
// Administrators full control, Users read and execute (the installed binary runs from here), inherited.
QByteArray lockDirectoryDescriptor();
// Owner only (no DACL: the file inherits its folder's DACL). Config policy: Administrators. Runtime: SYSTEM.
QByteArray ownerOnlyDescriptor(const QString& owner);

// A lock file's descriptor is exactly what lockFileDescriptor() makes, up to masks: owner SYSTEM or
// Administrators, a protected DACL whose every entry is an allow for SYSTEM or Administrators.
bool isLockFileSecure(const Descriptor&, QString* why);

// Same list of entries (order, type, flags, mask, SID).
bool sameEntries(const QList<Ace>&, const QList<Ace>&);

QString toDisplay(const Descriptor&);   // SDDL-like, for messages and test failures

}  // namespace asb::win::sd
