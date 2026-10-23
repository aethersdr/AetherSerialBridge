#pragma once

// A cli::Host over the WS-5 fakes (Rig) for tst_Commands and tst_Cli: the Applier's Platform is the Rig's, the clock
// is the Rig's fake clock, and every Host call the commands make is recorded in order ("guard", "probe",
// "elevated", "platform", "runScratch ...") so a test can assert mode entry and that a refused command built nothing.
// Scratch folders are real directories (POSIX adapters) on Linux.

#include "cli/Commands.h"
#include "support/Fakes.h"

#ifndef _WIN32
#include "platform/posix/PosixDataFolder.h"
#include "platform/posix/PosixFileIo.h"

#include <unistd.h>
#endif

#include <QDir>
#include <QTemporaryDir>

#include <mutex>

namespace asb::test {

class CapturingConsole final : public cli::Console {
public:
    void out(const QString& line) override
    {
        std::lock_guard<std::mutex> g(m);
        outLines << line;
        events << QStringLiteral("out: ") + line;
    }
    void err(const QString& line) override
    {
        std::function<void(const QString&)> hook;
        {
            std::lock_guard<std::mutex> g(m);
            errLines << line;
            events << QStringLiteral("err: ") + line;
            hook = onErr;
        }
        if (hook) hook(line);
    }
    QString outText() const { return outLines.join(QLatin1Char('\n')); }
    QString errText() const { return errLines.join(QLatin1Char('\n')); }

    std::mutex m;
    QStringList outLines, errLines, events;
    std::function<void(const QString&)> onErr;
};

class FakeCliHost : public cli::Host {
public:
    explicit FakeCliHost(Rig& rig) : r(rig)
    {
        dataDir = tmp.filePath(QStringLiteral("data"));
        QDir().mkpath(dataDir);
    }

    // The build the build question names (empty: Host::thisBuild, the version).
    QString thisBuild() override { return buildName.isEmpty() ? cli::Host::thisBuild() : buildName; }
    QString buildName;
    bool windows() const override { return isWindows; }
    QString managedConfigFile() const override { return dataDir + QStringLiteral("/config.json"); }
    QString dataFolder() const override { return dataDir; }
    void enterGuardedMode() override { calls << QStringLiteral("guard"); }
    void enterStandardUserJob() override { calls << QStringLiteral("standardUserJob"); }
    void guardProbe() override { calls << QStringLiteral("probe"); }
    void blockStopSignals() override { calls << QStringLiteral("signals"); }
    bool elevated() override
    {
        calls << QStringLiteral("elevated");
        return isElevated;
    }
    Platform* platform(QString* error, int* code) override
    {
        calls << QStringLiteral("platform");
        if (platformError) {
            *error = *platformError;
            *code = 3;
            return nullptr;
        }
        return r.platform.get();
    }
    QStringList takeReports() override
    {
        QStringList out;
        out.swap(reports);
        return out;
    }
    IClock& clock() override { return r.world.clock; }
    std::chrono::milliseconds interimPoll() override { return std::chrono::milliseconds(10); }
    void resultHook(const Result& res, const QStringList&) override { results << res; }

    cli::ScratchFolder scratchFolder(const QString& directory) override
    {
        calls << QStringLiteral("scratchFolder");
        cli::ScratchFolder s;
#ifndef _WIN32
        const posix::PosixOwner me{::geteuid(), ::getegid()};
        posix::PosixDataFolder::Options o;   // as LinuxHost: Config owner root, the invoking user also trusted
        o.scratchUser = ::geteuid();
        s.folder = std::make_unique<posix::PosixDataFolder>(directory, o);
        s.io = std::make_unique<posix::PosixFileIo>(posix::realPosixSys(), me, 0644);
#else
        Q_UNUSED(directory);
#endif
        return s;
    }
    QString ownExecutable() override { return own; }
    QString installedExecutable() override { return installed; }
    int runScratch(const QString& file, const BridgeConfig& c, cli::Console&) override
    {
        calls << QStringLiteral("runScratch ") + file;
        scratchConfig = c;
        return 0;
    }
    int runService() override
    {
        calls << QStringLiteral("runService");
        return 2;
    }
    int listPorts(cli::Console& c) override
    {
        calls << QStringLiteral("listPorts");
        c.out(QStringLiteral("(ports)"));
        return 0;
    }
    std::optional<bool> confirm(const QString&) override
    {
        calls << QStringLiteral("confirm");
        return answer;
    }
    // --uninstall-from-apps (WS-16): the boxes it showed, its answers, and the relaunch it asked for.
    bool messageBox(const QString& text, Box kind) override
    {
        calls << QStringLiteral("messageBox");
        boxes << text;
        return kind == Box::Question && boxAnswer;
    }
    void detachConsole() override { calls << QStringLiteral("detachConsole"); }
    Relaunch relaunchElevated(const QStringList& arguments) override
    {
        calls << QStringLiteral("relaunch");
        relaunched << arguments;
        return relaunchResult;
    }

    Rig& r;
    QTemporaryDir tmp;
    QString dataDir;
    QString own, installed;
    bool isElevated = true;
    std::optional<QString> platformError;
    std::optional<bool> answer;
    QStringList calls, reports;
    QList<Result> results;
    std::optional<BridgeConfig> scratchConfig;
    bool isWindows = false;
    bool boxAnswer = false;               // the answer to a Question box
    QStringList boxes;                    // every message box's text, in order
    QList<QStringList> relaunched;        // every relaunchElevated request
    Relaunch relaunchResult{true, false, 0, {}};
};

}  // namespace asb::test
