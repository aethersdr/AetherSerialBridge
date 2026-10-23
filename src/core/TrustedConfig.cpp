#include "core/TrustedConfig.h"

#include <exception>

namespace asb::TrustedConfig {
namespace {

QString untrustedText(const Inspection& i)
{
    if (i.action == DirAction::Refuse)
        return QStringLiteral("The data folder is not a plain folder (%1), so it cannot be trusted. "
                              "Remove it as an administrator and try again.")
            .arg(i.reason);
    return QStringLiteral("The data folder is not trusted (%1). Run Repair to move it aside and start "
                          "with an empty one; nothing is deleted.")
        .arg(i.reason);
}

// Re-inspects after a change and requires Trust: the change is verified, not assumed.
bool verifyTrusted(IDataFolder& folder, TrustMode mode, OpenedFolder& out)
{
    out.inspection = folder.inspect(mode);
    if (out.inspection.action == DirAction::Trust) return true;
    out.error = QStringLiteral("The data folder is still not trusted after securing it (%1).").arg(out.inspection.reason);
    return false;
}

// The two-argument open(): the folder's own mutators, ungated (tests and adapters' own checks).
class DirectMutations final : public FolderMutations {
public:
    explicit DirectMutations(IDataFolder& f) : m_f(f) {}
    void createLockedDown() override { m_f.createLockedDown(); }
    QStringList normalise(const Inspection& i) override { return m_f.normalise(i); }
    QString renameAside(const Inspection& i) override { return m_f.renameAside(i); }

private:
    IDataFolder& m_f;
};

OpenedFolder mutate(IDataFolder& folder, TrustMode mode, FolderMutations& change)
{
    OpenedFolder out;
    out.inspection = folder.inspect(mode);
    out.fileChanges += out.inspection.fileChanges;
    try {
        switch (out.inspection.action) {
        case DirAction::Trust:
            break;
        case DirAction::Create:
            change.createLockedDown();
            out.fileChanges << QStringLiteral("created the data folder");
            if (!verifyTrusted(folder, mode, out)) return out;
            break;
        case DirAction::Recreate:
            if (mode != TrustMode::Repair) {
                out.error = untrustedText(out.inspection);
                return out;
            }
            {
                // Rename aside FIRST; the notice is recorded only once the rename succeeded (INV-21).
                const QString aside = change.renameAside(out.inspection);
                out.fileChanges << QStringLiteral("moved the untrusted folder to %1 (nothing was deleted)").arg(aside);
            }
            change.createLockedDown();
            out.fileChanges << QStringLiteral("created a fresh data folder");
            if (!verifyTrusted(folder, mode, out)) return out;
            break;
        case DirAction::Refuse:
            out.error = untrustedText(out.inspection);
            return out;
        }
        out.fileChanges += change.normalise(out.inspection);
        if (!verifyTrusted(folder, mode, out)) return out;
    } catch (const PlatformErrorAfterChanges& e) {
        out.fileChanges += e.changes;   // made before the failure and still on disk: reported, not lost
        out.error = QStringLiteral("Could not secure the data folder: %1").arg(QString::fromLocal8Bit(e.what()));
        return out;
    } catch (const std::exception& e) {
        out.error = QStringLiteral("Could not secure the data folder: %1").arg(QString::fromLocal8Bit(e.what()));
        return out;
    } catch (const GateRefusal& r) {
        out.error = r.describe();
        out.refused = r;
        return out;
    }
    out.usable = true;
    return out;
}

ConfigFiles::Reader readerFor(IDataFolder& folder, const Inspection& insp)
{
    return [&folder, &insp](const QString& name, qint64 cap) {
        ConfigFiles::RawRead r;
        QString err;
        const std::optional<QByteArray> bytes = folder.readFile(insp, name, cap, &err);
        if (bytes) {
            r.status = ConfigFiles::RawRead::Status::Ok;
            r.bytes = *bytes;
        } else if (err == fileNotFoundError()) {
            r.status = ConfigFiles::RawRead::Status::Missing;
        } else {
            r.status = ConfigFiles::RawRead::Status::Failed;
            r.error = err;
        }
        return r;
    };
}

}  // namespace

OpenedFolder open(IDataFolder& folder, TrustMode mode)
{
    DirectMutations direct(folder);
    return open(folder, mode, direct);
}

OpenedFolder open(IDataFolder& folder, TrustMode mode, FolderMutations& mutations)
{
    switch (mode) {
    case TrustMode::Mutating:
    case TrustMode::Repair:
        return mutate(folder, mode, mutations);
    case TrustMode::ReadOnly:
    case TrustMode::Service:
    case TrustMode::Scratch:
        break;
    }
    // Read-only modes: inspect, never change anything (INV-22, INV-44).
    OpenedFolder out;
    out.inspection = folder.inspect(mode);
    switch (out.inspection.action) {
    case DirAction::Trust:
        out.usable = true;
        break;
    case DirAction::Create:
        out.missing = true;
        out.error = mode == TrustMode::ReadOnly
                        ? QStringLiteral("The data folder has not been created yet.")
                        : QStringLiteral("The data folder does not exist; it is created by --install or the "
                                         "configuration window. No ports are bridged.");
        break;
    case DirAction::Recreate:
    case DirAction::Refuse:
        out.error = untrustedText(out.inspection);
        if (mode != TrustMode::ReadOnly) out.error += QStringLiteral(" No ports are bridged.");
        break;
    }
    return out;
}

ConfigFiles::Loaded readCommitted(IDataFolder& folder, const Inspection& insp)
{
    return ConfigFiles::readCommitted(readerFor(folder, insp));
}

ConfigFiles::Loaded readPending(IDataFolder& folder, const Inspection& insp)
{
    return ConfigFiles::readPending(readerFor(folder, insp));
}

ConfigFiles::Loaded readScratch(IDataFolder& folder, const Inspection& insp, const QString& fileName)
{
    return ConfigFiles::readNamed(readerFor(folder, insp), fileName);
}

}  // namespace asb::TrustedConfig
