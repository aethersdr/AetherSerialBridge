#pragma once

// The TUI's fake service host (master §8.4 "ASB_TEST_SERVICE_HOST"): the WS-5 Rig behind a cli::Host, set up by a
// scenario string, for tst_Tui (in process) and asb_tui_testhost (tst_TuiProcess, under a pty). Test code only.
//
// Scenario: comma-separated words.
//   installed          (the default) a station running configA(): service Current, token, lock pair, rules
//   fresh              nothing installed, an empty data folder that does not exist yet
//   untrusted          installed, but the data folder is owned by "attacker" (Recreate: INV-33)
//   pending            installed, and config.pending.json holds configB() (saved, not applied)
//   readonly           not elevated
//   detect-fails       IServiceControl::detect throws (the service manager cannot be queried)
//   query-fails        detect answers, IServiceControl::query throws
//   token-corrupt      start.token holds bytes that are not a token
//   other-build        detect(): a complete installation of another build (IncompleteInstall, otherBuild)
//   unfinished-install detect(): IncompleteInstall for another reason (not otherBuild)
//   barrier-deadline   the engine outlives every stop request: the barrier's deadline passes before R4
//   block=<seam>:<m>   the first call of seam ("service" or "firewall") method <m> moves the fake clock past every
//                      phase deadline (so the interim fires) and then blocks until release() (or the file named by
//                      ASB_TEST_RELEASE exists)

#include "support/FakeCliHost.h"
#include "support/Fakes.h"

#include "core/ConfigFiles.h"

#include <QFile>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <chrono>

namespace asb::test {

class TuiScenario {
public:
    TuiScenario(Rig& rig, FakeCliHost& host) : m_rig(rig), m_host(host) {}

    void apply(const QString& scenario)
    {
        QStringList words = scenario.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const bool fresh = words.contains(QStringLiteral("fresh"));
        if (!fresh) m_rig.installedRunning(configA());
        else m_rig.folder.state = FakeFolder::DirState::Missing;
        for (const QString& w : words) {
            if (w == QLatin1String("untrusted")) {
                m_rig.folder.owner = QStringLiteral("attacker");
            } else if (w == QLatin1String("pending")) {
                m_rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
            } else if (w == QLatin1String("readonly")) {
                m_host.isElevated = false;
            } else if (w == QLatin1String("detect-fails")) {
                m_rig.service.fail[QStringLiteral("detect")] = QStringLiteral("the service manager did not answer");
            } else if (w == QLatin1String("query-fails")) {
                m_rig.service.fail[QStringLiteral("query")] = QStringLiteral("the service manager did not answer");
            } else if (w == QLatin1String("token-corrupt")) {
                m_rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kStartToken), QByteArrayLiteral("garbage"),
                                            QStringLiteral("root"));
            } else if (w == QLatin1String("other-build") || w == QLatin1String("unfinished-install")) {
                m_rig.service.installation.state = InstallState::IncompleteInstall;
                m_rig.service.installation.otherBuild = w == QLatin1String("other-build");
                m_rig.service.installation.installedSha256 = w == QLatin1String("other-build")
                                                                 ? QStringLiteral("abcdef0123456789abcdef0123456789")
                                                                 : QString();
            } else if (w == QLatin1String("barrier-deadline")) {
                m_rig.service.neverReleases = true;
            } else if (w.startsWith(QLatin1String("block="))) {
                const QStringList sm = w.mid(6).split(QLatin1Char(':'));
                if (sm.size() == 2) block(sm[0], sm[1]);
            }
        }
        m_rig.world.log.clear();
    }

    void release() { m_released = true; }
    bool blocked() const { return m_blocked.load(); }
    QString releaseFile;

private:
    void block(const QString& seam, const QString& method)
    {
        auto hook = [this, method](const QString& m) {
            if (m != method || m_fired.exchange(true)) return;
            // Every phase deadline is past while the call is still inside: the host posts the interim (D57).
            m_rig.world.clock.advance(std::chrono::minutes(5));
            m_blocked = true;
            for (int i = 0; i < 6000 && !m_released.load(); ++i) {
                if (!releaseFile.isEmpty() && QFile::exists(releaseFile)) break;
                QThread::msleep(10);
            }
            m_blocked = false;
        };
        if (seam == QLatin1String("service")) m_rig.service.onCall = hook;
        else if (seam == QLatin1String("firewall")) m_rig.firewall.onCall = hook;
    }

    Rig& m_rig;
    FakeCliHost& m_host;
    std::atomic<bool> m_fired{false}, m_released{false}, m_blocked{false};
};

}  // namespace asb::test
