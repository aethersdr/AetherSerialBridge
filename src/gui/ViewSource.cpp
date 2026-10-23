#include "gui/ViewSource.h"

#include "core/ConfigFiles.h"
#include "core/TrustedConfig.h"

#include <chrono>
#include <QScopeGuard>

namespace asb::gui {
namespace {

constexpr std::chrono::seconds kQueryBudget{15};
// bridge.log is bounded at 2 MB + 2 lines (master §4.7); the folder refuses anything larger than the cap.
constexpr qint64 kLogReadCap = 3 * 1024 * 1024;

ConfigView view(const ConfigFiles::Loaded& l)
{
    ConfigView v;
    v.present = l.present;
    v.bytes = l.bytes;
    v.config = l.config;
    v.error = l.error;
    return v;
}

}  // namespace

QString describeInstallation(const Installation& i)
{
    switch (i.state) {
    case InstallState::NotInstalled: return QStringLiteral("not installed");
    case InstallState::IncompleteInstall:
        if (i.otherBuild) return QStringLiteral("a different build of the program is installed; Save & apply asks before it updates it");
        return QStringLiteral("installed, but the install did not finish%1")
            .arg(i.detail.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(i.detail));
    case InstallState::Foreign:
        return QStringLiteral("a service this program did not install is in the way%1")
            .arg(i.detail.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(i.detail));
    case InstallState::Current: return QStringLiteral("installed");
    }
    return {};
}

PlatformViewSource::PlatformViewSource(Platform& p, Hooks hooks, IClock& clock)
    : m_p(p), m_hooks(std::move(hooks)), m_clock(clock)
{
}

ServiceView PlatformViewSource::service()
{
    ServiceView v;
    const Deadline deadline = m_clock.now() + kQueryBudget;
    try {
        const Installation inst = m_p.service.detect(deadline);
        v.installation = inst.state;
        v.otherBuild = inst.otherBuild;
        v.installedSha256 = inst.installedSha256;
        v.installedVersion = inst.installedVersion;
        v.programRemains = inst.state == InstallState::NotInstalled && inst.programRemains;
        if (inst.state != InstallState::Current && inst.state != InstallState::NotInstalled)
            v.installationDetail = describeInstallation(inst);
        v.queried = true;
    } catch (const std::exception& e) {
        v.error = QString::fromLocal8Bit(e.what());
        return v;   // installation unknown: no state, no start type (never read as "not installed")
    }
    // The state is a read of its own: a failure leaves it unknown and the installation known.
    if (v.installation == InstallState::Current || v.installation == InstallState::IncompleteInstall) {
        try {
            v.state = m_p.service.query(deadline);
        } catch (const std::exception& e) {
            v.stateError = QString::fromLocal8Bit(e.what());
        }
    }
    if (m_hooks.autoStart && v.queried && v.installation != InstallState::NotInstalled) v.autoStart = m_hooks.autoStart();
    if (v.queried && v.installation == InstallState::NotInstalled) v.autoStart = false;
    return v;
}

ServiceView PlatformViewSource::serviceState(const ServiceView& last, bool allowDetect)
{
    const bool installed = last.queried && (last.installation == InstallState::Current ||
                                            last.installation == InstallState::IncompleteInstall);
    if (!installed) return allowDetect ? service() : last;
    ServiceView v = last;
    try {
        v.state = m_p.service.query(m_clock.now() + kQueryBudget);
        v.stateError.clear();
    } catch (const std::exception& e) {
        v.state.reset();
        v.stateError = QString::fromLocal8Bit(e.what());
    }
    return v;
}

std::optional<StatusReport> PlatformViewSource::status()
{
    return m_p.status.read();
}

Snapshot PlatformViewSource::load()
{
    // v1.1 INV-128: the folder's handle is closed again when the snapshot is read (the window holds nothing in the data
    // folder between loads).
    const auto closeFolder = qScopeGuard([this] { m_p.folder.closeIdle(); });
    Snapshot s;
    const OpenedFolder o = TrustedConfig::open(m_p.folder, TrustMode::ReadOnly);
    if (o.missing) {
        s.folder = FolderView::Missing;
        s.committed.present = false;
        s.committed.config = BridgeConfig{};   // a fresh install has none: an empty config
    } else if (!o.usable) {
        s.folder = FolderView::Untrusted;
        s.folderReason = o.error;
    } else {
        s.folder = FolderView::Trusted;
        s.committed = view(TrustedConfig::readCommitted(m_p.folder, o.inspection));
        s.pending = view(TrustedConfig::readPending(m_p.folder, o.inspection));
        QString why;
        const std::optional<IStartToken::Token> token = m_p.token.read(&why);
        s.tokenPresent = token.has_value();
        s.tokenWhy = why;
        if (token) s.tokenGeneration = token->generation;
    }
    s.service = service();
    s.status = status();
    return s;
}

QStringList PlatformViewSource::serialPorts()
{
    return m_hooks.serialPorts ? m_hooks.serialPorts() : QStringList{};
}

std::optional<QString> PlatformViewSource::logTail(qint64 cap, QString* why)
{
    if (m_hooks.journalLog) {
        if (why) *why = QStringLiteral("The bridge logs to the system journal: journalctl -u aether-serial-bridge");
        return std::nullopt;
    }
    const auto closeFolder = qScopeGuard([this] { m_p.folder.closeIdle(); });   // v1.1 INV-128
    const OpenedFolder o = TrustedConfig::open(m_p.folder, TrustMode::ReadOnly);
    if (o.missing) {
        if (why) *why = QStringLiteral("No log yet: the data folder has not been created.");
        return std::nullopt;
    }
    if (!o.usable) {
        if (why) *why = QStringLiteral("The data folder is not trusted; nothing in it is shown.");
        return std::nullopt;
    }
    QString err;
    const std::optional<QByteArray> bytes =
        m_p.folder.readFile(o.inspection, QString::fromLatin1(ConfigFiles::kLog), kLogReadCap, &err);
    if (!bytes) {
        if (why) *why = err == fileNotFoundError() ? QStringLiteral("No log yet.") : QStringLiteral("The log could not be read: %1").arg(err);
        return std::nullopt;
    }
    QByteArray tail = *bytes;
    if (cap > 0 && tail.size() > cap) {
        tail = tail.right(cap);
        const qsizetype nl = tail.indexOf('\n');
        if (nl >= 0) tail = tail.mid(nl + 1);   // start at a whole line
    }
    return QString::fromUtf8(tail);
}

Snapshot EmptyViewSource::load()
{
    Snapshot s;
    s.folder = FolderView::Missing;
    s.committed.config = BridgeConfig{};
    s.service = service();
    return s;
}

ServiceView EmptyViewSource::service()
{
    ServiceView v;
    v.queried = m_why.isEmpty();
    v.error = m_why;
    v.autoStart = false;
    return v;
}

std::optional<QString> EmptyViewSource::logTail(qint64, QString* why)
{
    if (why) *why = m_why.isEmpty() ? QStringLiteral("No log.") : m_why;
    return std::nullopt;
}

}  // namespace asb::gui
