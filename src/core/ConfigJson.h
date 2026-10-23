#pragma once

// config.json reader and writer (master §4.2, INV-30).
//
// Writer: 2-space indent, camelCase names, enums as their names, no `displayName` (a computed property:
// neither written nor read).
// Reader: property names exact camelCase; enum names case-insensitive; integer enum values accepted;
// unknown properties ignored; missing properties keep their defaults; a leading UTF-8 BOM is skipped.

#include "core/BridgeConfig.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace asb {

QByteArray toJson(const BridgeConfig&);
// nullopt on anything that is not a valid BridgeConfig document; `error` (if given)
// then says why, in one line.
std::optional<BridgeConfig> fromJson(const QByteArray&, QString* error);

// Lower-case hex SHA-256 of exact bytes: the digest convention for configSha256, persistedSha256 and
// request source digests.
QString sha256Hex(const QByteArray&);

}  // namespace asb
