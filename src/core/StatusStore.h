#pragma once

// Status types and status.json (WS-3). The service publishes a StatusReport about once a second
// (through the WS-4 side-channel worker, AtomicJson with OwnerPolicy::Runtime); viewers read it
// ReadOnly and bounded. The Applier's ACK (D25, D52, INV-109) is decided from `startToken`, `pid`,
// `configSha256` and `configError`.

#include "core/AtomicJson.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace asb {

enum class BridgeState { Stopped, Listening, Connected, Error };
QString enumName(BridgeState);
std::optional<BridgeState> bridgeStateFromName(const QString&);

struct PortStatus {
    QString name, comPort, bindAddress;
    int tcpPort = 0;
    BridgeState state = BridgeState::Stopped;
    std::optional<QString> client, lastError;
    qint64 bytesToSerial = 0, bytesToClient = 0;
    QDateTime since;
    bool operator==(const PortStatus&) const = default;
};

struct StatusReport {
    QDateTime updated;
    QDateTime startedAt;
    qint64 pid = 0;
    QByteArray instanceId;            // random per process (newInstanceId)
    QByteArray startToken;            // echoed from start.token (D52); the ACK value
    QString configSha256;             // sha256Hex of the exact config bytes the service read
    std::optional<QString> configError;
    QList<PortStatus> ports;
    bool operator==(const StatusReport&) const = default;
};

// Reads the published status. Implementations read through the data-folder trust layer in ReadOnly
// mode (never writing) and refuse more than kStatusMaxBytes.
class IStatusReader {
public:
    virtual ~IStatusReader() = default;
    virtual std::optional<StatusReport> read() = 0;
};

namespace StatusStore {

inline constexpr char kFileName[] = "status.json";
inline constexpr qint64 kStatusMaxBytes = 4 * 1024 * 1024;

QByteArray toJson(const StatusReport&);
// nullopt for anything that is not a well-formed report, or larger than kStatusMaxBytes.
std::optional<StatusReport> fromJson(const QByteArray&, QString* error = nullptr);
// Writes status.json in `dir` with OwnerPolicy::Runtime (the service's own file: no chown on POSIX,
// owner SYSTEM on Windows). INV-96.
WriteResult write(FileIo&, const DirHandle&, const StatusReport&, const NameGen& gen = {});
// 128 random bits from the system CSPRNG as 32 lower-case hex digits.
QByteArray newInstanceId();

}  // namespace StatusStore
}  // namespace asb
