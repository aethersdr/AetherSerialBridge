#pragma once

// start.token, the activation fence (S18, D52, INV-113), implemented once over the FileIo seam and the
// trusted data folder, so the revocation order is the same on every OS and testable on the FileIo
// operation log. The platform part — the regular-file, owner and not-writable-by-others checks on the
// opened handle, the create-time owner of the new token — is the adapters' IDataFolder::readFile and
// FileIo (WS-6, WS-8).
//
//   revoke (B0): openExisting (no link/reparse followed) -> truncate -> write {"revoked":true} with a
//     full-write loop -> flush the FILE -> remove by handle -> flush the directory (best effort).
//     ENOENT = nothing to revoke. Any other open failure = nothing was changed. A truncate/write/flush
//     failure = the token may already be invalid (every partial state is rejected by read()).
//   writeFresh (R9): {"token":"<32 hex>","generation":"<16 hex>"} by atomicWrite (Config policy), with
//     replacement and durability reported separately.
//   read (--service): Service-mode inspection, then the file through the trusted read path, <= 256
//     bytes, exactly the form writeFresh writes; the revoked form, and anything else, is refused.

#include "core/Interfaces.h"

namespace asb {

namespace StartToken {
inline constexpr qint64 kMaxBytes = 256;
inline constexpr char kRevoked[] = "{\"revoked\":true}";
QByteArray format(const QByteArray& token, const FirewallGeneration&);
// The exact form only; `why` names what is wrong (revoked, malformed, ...).
std::optional<IStartToken::Token> parse(const QByteArray& bytes, QString* why);
QByteArray newToken();   // 128 bits from the system CSPRNG, 32 lower-case hex digits
}  // namespace StartToken

class FileStartToken final : public IStartToken {
public:
    FileStartToken(IDataFolder& folder, FileIo& io, NameGen gen = {}) : m_folder(folder), m_io(io), m_gen(std::move(gen)) {}
    Revocation revoke(Deadline, QString* err) override;
    Written writeFresh(const FirewallGeneration&, Deadline) override;
    std::optional<Token> read(QString* why) override;

private:
    IDataFolder& m_folder;
    FileIo& m_io;
    NameGen m_gen;
};

}  // namespace asb
