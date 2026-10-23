#include "core/InstallationEvidence.h"

namespace asb::InstallationEvidence {
namespace {

Installation make(InstallState s, Installation::Flavor f, const QString& detail = {})
{
    Installation i;
    i.state = s;
    i.flavor = f;
    i.detail = detail;
    return i;
}

// Splits a binPath into the program and its arguments (quoted or not).
void splitBinPath(const QString& binPath, QString* exe, QString* args)
{
    const QString s = binPath.trimmed();
    if (s.startsWith(QLatin1Char('"'))) {
        const qsizetype close = s.indexOf(QLatin1Char('"'), 1);
        if (close < 0) {
            *exe = s.mid(1);
            args->clear();
            return;
        }
        *exe = s.mid(1, close - 1);
        *args = s.mid(close + 1).trimmed();
        return;
    }
    const qsizetype dotExe = s.indexOf(QStringLiteral(".exe"), 0, Qt::CaseInsensitive);
    const qsizetype end = dotExe >= 0 ? dotExe + 4 : s.indexOf(QLatin1Char(' '));
    *exe = end >= 0 ? s.left(end) : s;
    *args = end >= 0 ? s.mid(end).trimmed() : QString();
}

QString fileName(const QString& path)
{
    const qsizetype slash = qMax(path.lastIndexOf(QLatin1Char('\\')), path.lastIndexOf(QLatin1Char('/')));
    return path.mid(slash + 1);
}

QString normalisedPath(QString p)
{
    p.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return p.toLower();
}

}  // namespace

// WindowsEvidence::appsEntryPresent is deliberately not read here (D63, INV-126; tst_InstallationStates::entryNeverClassified).
Installation classifyWindows(const WindowsEvidence& ev)
{
    const auto W = Installation::Flavor::Windows;
    if (!ev.serviceExists) {
        // An uninstall that removed the service and kept the program: still NotInstalled, with the
        // program noted so that the window offers the Uninstall that finishes it. The Apps & features entry is not
        // read here either (D63).
        Installation i = make(InstallState::NotInstalled, W);
        i.programRemains = ev.qtExePresent;
        return i;
    }
    QString exe, args;
    splitBinPath(ev.binPath, &exe, &args);
    const QString name = fileName(exe);
    if (name.compare(QStringLiteral("aether-serial-bridge.exe"), Qt::CaseInsensitive) == 0 && args == QStringLiteral("--service") &&
        normalisedPath(exe) == normalisedPath(ev.expectedQtExe)) {
        if (ev.qtExeDigestOk && ev.lockFilesPresent && ev.scmConfigProblem.isEmpty()) return make(InstallState::Current, W);
        // Complete except for the build: the installed program is there and whole, but it is another build.
        const bool otherBuild = !ev.qtExeDigestOk && !ev.installedSha256.isEmpty() && ev.lockFilesPresent &&
                                ev.scmConfigProblem.isEmpty();
        Installation i = make(InstallState::IncompleteInstall, W,
                              otherBuild            ? QStringLiteral("the installed program is a different build")
                              : !ev.qtExeDigestOk    ? QStringLiteral("the installed program is missing or not this build")
                              : !ev.lockFilesPresent ? QStringLiteral("a lock file is missing")
                                                     : QStringLiteral("the service is not fully configured (%1)").arg(ev.scmConfigProblem));
        i.otherBuild = otherBuild;
        i.installedSha256 = ev.installedSha256;
        i.installedVersion = ev.installedVersion;
        return i;
    }
    return make(InstallState::Foreign, W, QStringLiteral("the service runs %1").arg(ev.binPath));
}

Installation classifyLinux(const LinuxEvidence& ev)
{
    const auto L = Installation::Flavor::Linux;
    using E = LinuxEvidence::EtcUnit;
    if (ev.etcUnit == E::RegularFile || ev.etcUnit == E::Other)
        return make(InstallState::Foreign, L, QStringLiteral("an unrecognised unit in /etc/systemd/system"));
    if (ev.qtUnitPresent) {
        if (!ev.qtUnitContent.startsWith(kQtUnitHeader))
            return make(InstallState::Foreign, L, QStringLiteral("an unrecognised unit in /usr/local/lib/systemd/system"));
        // Build identity (v1.1 D61, INV-120), the same predicate shape as Windows: Current needs this build's program;
        // a complete installation of another build (the program read whole, its digest not this build's) is
        // IncompleteInstall with otherBuild, which Save & apply asks about. A program that could not be read, or a
        // caller that does not know its own build, is neither: incomplete, never Current and never "other build".
        const bool digestOk = ev.thisBuildKnown && ev.programDigestOk;
        if (ev.binaryPresent && ev.lockFilesPresent && digestOk) {
            Installation i = make(InstallState::Current, L);
            i.installedVersion = ev.installedVersion;
            return i;
        }
        const bool otherBuild = ev.binaryPresent && ev.thisBuildKnown && !digestOk && !ev.programUnreadable &&
                                !ev.installedSha256.isEmpty() && ev.lockFilesPresent;
        Installation i = make(InstallState::IncompleteInstall, L,
                              !ev.binaryPresent     ? QStringLiteral("the program is missing")
                              : ev.programUnreadable ? QStringLiteral("the installed program could not be read")
                              : otherBuild           ? QStringLiteral("the installed program is a different build")
                              : !ev.lockFilesPresent ? QStringLiteral("a lock file is missing")
                              : !ev.thisBuildKnown   ? QStringLiteral("this build's program digest is not known")
                                                     : QStringLiteral("the installed program is not this build"));
        i.otherBuild = otherBuild;
        i.installedSha256 = ev.installedSha256;
        i.installedVersion = ev.installedVersion;
        return i;
    }
    if (ev.etcUnit == E::MaskLink) return make(InstallState::IncompleteInstall, L, QStringLiteral("masked, but no unit file"));
    // No unit, but the program at its installed path: an uninstall removed the unit and then failed on the program
    // (SystemdService::uninstall removes them in that order). Still NotInstalled.
    Installation i = make(InstallState::NotInstalled, L);
    i.programRemains = ev.binaryPresent;
    return i;
}

}  // namespace asb::InstallationEvidence
