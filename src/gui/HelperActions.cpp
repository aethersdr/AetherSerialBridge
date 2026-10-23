#include "gui/HelperActions.h"

#include "cli/StagedHelper.h"
#include "core/ConfigJson.h"
#include "platform/posix/SystemdUnit.h"

#include <QMetaObject>

namespace asb::gui {

namespace {

Result nothingChanged(const QString& message)
{
    Result r;
    r.outcome = Outcome::NothingChanged;
    r.message = message;
    return r;
}

}  // namespace

HelperActions::HelperActions(cli::HelperClient& client, Staged staged, Facts facts)
    : m_client(client), m_staged(std::move(staged)), m_facts(std::move(facts))
{
    if (!m_staged.read || !m_staged.probe) return;
    // The window's program is hashed once, on a worker (WindowProgramProbe caches it for the staged reads that follow).
    m_probing = true;
    std::shared_ptr<Shared> shared = m_shared;
    QObject* context = &m_context;
    m_probe = std::thread([this, shared, context, probe = m_staged.probe] {
        QString problem;
        try {
            problem = probe();
        } catch (const std::exception& e) {
            problem = QString::fromLocal8Bit(e.what());
        } catch (...) {
            problem = QStringLiteral("it could not be read");
        }
        std::lock_guard lk(shared->m);
        if (shared->alive)
            QMetaObject::invokeMethod(context, [this, problem] {
                m_probing = false;
                m_probeProblem = problem;
                if (m_listener) m_listener();
            }, Qt::QueuedConnection);
    });
}

QString HelperActions::lifecycleUnavailableReason() const
{
    if (!m_staged.read) return QStringLiteral("This window has no program of its own to install.");
    if (m_probing) return QStringLiteral("Checking this window's program…");
    return m_probeProblem;
}

HelperActions::~HelperActions()
{
    {
        // A posted result to m_context is discarded with it; nothing is posted after this.
        std::lock_guard lk(m_shared->m);
        m_shared->alive = false;
    }
    if (m_worker.joinable()) m_worker.join();
    if (m_probe.joinable()) m_probe.join();
}

Result HelperActions::toResult(const cli::HelperClient::Outcome& o)
{
    Result r = o.result;
    if (!o.message.isEmpty()) r.message = o.message;
    return r;
}

cli::HelperClient::Done HelperActions::finishTo(IOperationObserver& observer)
{
    return [this, &observer](const cli::HelperClient::Outcome& o) {
        m_last = o;
        observer.finished(toResult(o));
    };
}

cli::HelperClient::Waiting HelperActions::interimTo(IOperationObserver& observer)
{
    return [&observer](const QString& message) { observer.interim(Interim{QStringLiteral("helper"), message}); };
}

cli::RouteFacts HelperActions::facts() const
{
    return m_facts ? m_facts() : cli::readInstalledFacts(QString::fromLatin1(posix::SystemdUnit::kInstalledBinary));
}

// The installed route: one request at a time (a staged read in progress counts), and v1.0's "Is the bridge installed?"
// hint only when the installed program could not be read.
bool HelperActions::prepareInstalled()
{
    if (busy()) return false;
    m_client.setNothingInstalled(!facts().installedVersion.has_value());
    return true;
}

bool HelperActions::run(const SaveRequest& r, bool install, IOperationObserver& o)
{
    if (install) return stagedInstall(r, o);
    if (!prepareInstalled()) return false;
    return m_client.saveAndApply(r.config, r.source, r.supersedePending, 0, finishTo(o), interimTo(o));
}

bool HelperActions::untick(const SaveRequest& r, IOperationObserver& o)
{
    if (!prepareInstalled()) return false;
    return m_client.deactivate(r.config, r.source, r.supersedePending, 0, finishTo(o), interimTo(o));
}

bool HelperActions::start(IOperationObserver& o)
{
    if (!prepareInstalled()) return false;
    return m_client.start(finishTo(o), interimTo(o));
}

bool HelperActions::stop(IOperationObserver& o)
{
    if (!prepareInstalled()) return false;
    return m_client.stop(finishTo(o), interimTo(o));
}

bool HelperActions::repair(IOperationObserver& o)
{
    if (!prepareInstalled()) return false;
    return m_client.repair(finishTo(o), interimTo(o));
}

bool HelperActions::discardPending(const QString& sha256, IOperationObserver& o)
{
    if (busy()) return false;
    const cli::RouteFacts f = facts();
    m_client.setNothingInstalled(!f.installedVersion.has_value());
    // INV-123: the digest-checked op only to a helper that has it (a stamped, v1.1+ program); v1.0 gets v1.0's op.
    if (cli::routeFor(cli::WindowAction::Discard, f).op == cli::helper::Op::DiscardPendingIf)
        return m_client.discardPendingIf(sha256, finishTo(o), interimTo(o));
    return m_client.discardPending(finishTo(o), interimTo(o));
}

bool HelperActions::install(const std::optional<SaveRequest>& r, IOperationObserver& o) { return stagedInstall(r, o); }
bool HelperActions::update(const std::optional<SaveRequest>& r, IOperationObserver& o) { return stagedInstall(r, o); }

bool HelperActions::stagedInstall(const std::optional<SaveRequest>& r, IOperationObserver& o)
{
    return staged(o, [this, r, &o](const cli::HelperClient::StagedProgram& p) {
        return m_client.install(r ? std::optional<QByteArray>(r->config) : std::nullopt, r ? r->source : Source{},
                                r && r->supersedePending, 0, p, finishTo(o), interimTo(o));
    });
}

bool HelperActions::uninstall(UninstallOptions options, IOperationObserver& o)
{
    return staged(o, [this, options, &o](const cli::HelperClient::StagedProgram& p) {
        return m_client.uninstall(options.removeSettings, p, finishTo(o), interimTo(o));
    });
}

// The staged route: the window's program (20-40 MB) is read AND hashed on a worker thread, never on the window's
// (hashing it there froze the window, most on a Pi). The digest is of that very buffer, which is what the client
// sends; the client launches it only when the digest is this window's build (D60), and the file is never read again.
bool HelperActions::staged(IOperationObserver& observer, StagedSubmit submit)
{
    if (busy()) return false;
    if (!m_staged.read) {
        // Posted like every other ending: never inside the call. Not busy by the time the result arrives.
        m_loading = true;
        QMetaObject::invokeMethod(&m_context, [this, &observer] {
            m_loading = false;
            observer.finished(nothingChanged(QStringLiteral("This window has no program of its own to install; nothing was changed.")));
        }, Qt::QueuedConnection);
        return true;
    }
    if (m_worker.joinable()) m_worker.join();   // the previous read ended (it posted before it returned)
    m_loading = true;
    std::shared_ptr<Shared> shared = m_shared;
    QObject* context = &m_context;
    auto read = m_staged.read;
    auto hash = m_staged.hash ? m_staged.hash : [](const QByteArray& b) { return sha256Hex(b).toLatin1(); };
    auto ready = [this, &observer, submit](const ProgramFile& file, const QByteArray& sha256) {
        m_loading = false;
        if (!file.problem.isEmpty()) {
            observer.finished(nothingChanged(
                QStringLiteral("This window's own program cannot be used (%1); nothing was changed.").arg(file.problem)));
            return;
        }
        const cli::HelperClient::StagedProgram p{file.bytes, sha256, m_staged.expectedSha256,
                                                 m_staged.appImage.isEmpty() ? file.path : m_staged.appImage};
        if (!submit(p)) observer.finished(nothingChanged(QStringLiteral("Another change is in progress; nothing was changed.")));
    };
    m_worker = std::thread([shared, context, read, hash, ready] {
        ProgramFile file;
        QByteArray sha256;
        try {
            file = read();
            if (file.problem.isEmpty()) sha256 = hash(file.bytes);   // the buffer `ready` hands to the client
        } catch (const std::exception& e) {
            file.problem = QString::fromLocal8Bit(e.what());
        } catch (...) {
            file.problem = QStringLiteral("it could not be read");
        }
        std::lock_guard lk(shared->m);
        if (shared->alive)
            QMetaObject::invokeMethod(context, [ready, file, sha256] { ready(file, sha256); }, Qt::QueuedConnection);
    });
    return true;
}

}  // namespace asb::gui
