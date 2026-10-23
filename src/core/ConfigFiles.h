#pragma once

// PRIVATE to the trust layer. Every read goes through TrustedConfig, a rule kept by linkage: only TrustedConfig.cpp (WS-5) may reference the
// read functions below, and WS-5's symbol-reference test (INV-20) fails the build otherwise. Everything
// else reads configuration through TrustedConfig / IDataFolder.
//
// The data-folder file names, the read cap, the raw reads (through a reader the trust layer supplies,
// so nothing here opens a path) and the pending write.

#include "core/AtomicJson.h"
#include "core/BridgeConfig.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>

namespace asb::ConfigFiles {

// Files in the data folder (master §4.6).
inline constexpr char kConfig[] = "config.json";              // committed; replaced only by the Applier's rename
inline constexpr char kPending[] = "config.pending.json";     // staged edits (D38, D48)
inline constexpr char kStartToken[] = "start.token";          // the activation fence (D52)
inline constexpr char kStatus[] = "status.json";              // service (Runtime policy)
inline constexpr char kLog[] = "bridge.log";                  // service (Runtime policy), and bridge.log.1

// The most a config file may hold. Generous (a 1,000-port config is ~400 KB) and bounded.
inline constexpr qint64 kConfigMaxBytes = 1024 * 1024;

// What the trust layer's read of one file produced.
struct RawRead {
    enum class Status { Ok, Missing, Failed } status = Status::Failed;
    QByteArray bytes;
    QString error;
};
// Reads `name` (a single component in the trusted folder) refusing more than `cap` bytes.
using Reader = std::function<RawRead(const QString& name, qint64 cap)>;

struct Loaded {
    bool present = false;                  // the file exists
    QByteArray bytes;                      // exactly what was read (the digest input)
    std::optional<BridgeConfig> config;    // parsed (not validated); nullopt on a read or parse failure
    QString error;                         // "Could not read config.json: <why>"
};

// config.json. Missing loads as an empty config (a fresh install has none).
Loaded readCommitted(const Reader&);
// config.pending.json. Missing → present = false, config = nullopt.
Loaded readPending(const Reader&);
// A scratch config (--config <file>, WS-9) by its own file name in its folder. Missing loads as an empty config,
// like config.json.
Loaded readNamed(const Reader&, const QString& name);

// Writes the exact bytes as config.pending.json (OwnerPolicy::Config; replacement and durability
// reported separately, R4).
WriteResult writePending(FileIo&, const DirHandle&, const QByteArray& bytes, const NameGen& gen = {});

}  // namespace asb::ConfigFiles
