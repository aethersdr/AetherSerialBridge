#pragma once

// Judges access-control entries on the data folder and its children (with
// AceKind::Unknown never trusted). An entry is untrusted when it ALLOWS any right that can change the
// object or its security (write or append data, write attributes or extended attributes, delete,
// delete child, WRITE_DAC, WRITE_OWNER, or the generic write/all bits) to anyone outside the trusted
// owners. Deny entries never widen access; read/execute is fine. An entry whose type the adapter could
// not decode (anything but a basic allow/deny ACE) is `Unknown` and untrusted whatever its SID and
// mask (INV-19 is decoded natively by WS-6; this is the judgement).
//
// CREATOR OWNER (S-1-3-0) and OWNER RIGHTS (S-1-3-4) are not special-cased: they stand for whoever
// creates or owns an object, which may be a non-administrator.

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace asb {

enum class AceKind { Allow, Deny, Unknown };

struct AceInfo {
    QString sid;
    quint32 rights = 0;      // the raw access mask
    AceKind kind = AceKind::Allow;
};

// Who may own (and write) the data folder and the files in it.
struct TrustPolicy {
    QStringList trustedOwners;   // Windows: SYSTEM, BUILTIN\Administrators. POSIX adapters name uid 0.
    QString alsoTrusted;         // the elevated administrator running the tool (object-creator owner policy)

    bool isTrustedOwner(const std::optional<QString>& sid) const;
    // SYSTEM + Administrators (+ the running administrator's SID, if given).
    static TrustPolicy windows(const QString& alsoTrusted = {});
};

namespace AceJudge {

// Win32 access-mask bits (FileSystemRights / generic rights).
inline constexpr quint32 WriteData = 0x2;                  // also CreateFiles
inline constexpr quint32 AppendData = 0x4;                 // also CreateDirectories
inline constexpr quint32 WriteExtendedAttributes = 0x10;
inline constexpr quint32 DeleteSubdirectoriesAndFiles = 0x40;
inline constexpr quint32 WriteAttributes = 0x100;
inline constexpr quint32 Delete = 0x10000;
inline constexpr quint32 ChangePermissions = 0x40000;      // WRITE_DAC
inline constexpr quint32 TakeOwnership = 0x80000;          // WRITE_OWNER
inline constexpr quint32 GenericAll = 0x10000000;
inline constexpr quint32 GenericWrite = 0x40000000;

inline constexpr quint32 ReadAndExecute = 0x1200A9;
inline constexpr quint32 Modify = 0x1301BF;
inline constexpr quint32 FullControl = 0x1F01FF;

inline constexpr quint32 Dangerous = WriteData | AppendData | WriteExtendedAttributes | DeleteSubdirectoriesAndFiles |
                                     WriteAttributes | Delete | ChangePermissions | TakeOwnership | GenericAll |
                                     GenericWrite;

bool isUntrusted(const AceInfo&, const TrustPolicy&);
bool anyUntrusted(const QList<AceInfo>&, const TrustPolicy&);

}  // namespace AceJudge
}  // namespace asb
