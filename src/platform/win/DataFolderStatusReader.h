#pragma once

// IStatusReader over a trusted data folder (WS-7): the Applier's R10 ACK and the viewers read status.json through
// their OWN folder object (never the Applier's), ReadOnly: inspect, then read the file through the inspected
// handle (owner and permissions re-checked on that handle), at most 4 MiB, parsed strictly. Anything else —
// an untrusted or missing folder, a missing or malformed file — is "no report". Portable.

#include "core/Interfaces.h"
#include "core/StatusStore.h"

namespace asb::win {

class DataFolderStatusReader final : public IStatusReader {
public:
    explicit DataFolderStatusReader(IDataFolder& ownFolder) : m_folder(ownFolder) {}
    std::optional<StatusReport> read() override
    {
        const Inspection i = m_folder.inspect(TrustMode::ReadOnly);
        std::optional<QByteArray> bytes;
        if (i.action == DirAction::Trust) {
            QString err;
            bytes = m_folder.readFile(i, QString::fromLatin1(StatusStore::kFileName), StatusStore::kStatusMaxBytes, &err);
        }
        m_folder.closeIdle();   // v1.1 INV-128: nothing held in the data folder between reads
        if (!bytes) return std::nullopt;
        return StatusStore::fromJson(*bytes);
    }

private:
    IDataFolder& m_folder;
};

}  // namespace asb::win
