#include "cli/StagedHelper.h"

#include "cli/StageScript.h"
#include "core/BuildStamp.h"

#include <QFile>
#include <QRegularExpression>

namespace asb::cli {
namespace staged {

QByteArray script()
{
    return QByteArray(kStageScript);
}

QStringList pkexecArguments(const QByteArray& expectedSha256, qint64 programLength, qint64 requestLength)
{
    return {QString::fromLatin1(kShell),
            QStringLiteral("-c"),
            QString::fromUtf8(script()),
            QString::fromLatin1(kScriptName),
            QString::fromLatin1(expectedSha256),
            QString::number(programLength),
            QString::number(requestLength)};
}

bool isStagedProgramPath(const QString& path)
{
    static const QRegularExpression re(QStringLiteral("\\A%1/staging\\.[A-Za-z0-9]{8}/aether-serial-bridge\\z")
                                           .arg(QRegularExpression::escape(QString::fromLatin1(kStagingRoot))));
    return re.match(path).hasMatch();
}

QString stageLine(const QByteArray& err)
{
    // The last one: the script's own line is the last thing it prints before it exits.
    const QLatin1String prefix(kLinePrefix);
    QString found;
    for (const QByteArray& raw : err.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        if (line.startsWith(prefix)) found = line.mid(prefix.size()).trimmed();
    }
    return found;
}

QString unsafeDirectory(const QString& line)
{
    static const QLatin1String tail(" is not a directory only root can write");
    return line.endsWith(tail) ? line.chopped(tail.size()) : QString();
}

QString terminalCommand(const QString& program, helper::Op op, bool removeSettings)
{
    // Double quotes, with the four characters that are special inside them escaped: the command runs as shown.
    QString quoted;
    for (const QChar c : program) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\') || c == QLatin1Char('$') || c == QLatin1Char('`'))
            quoted += QLatin1Char('\\');
        quoted += c;
    }
    QString cmd = QStringLiteral("sudo \"%1\"").arg(quoted);
    if (op == helper::Op::Uninstall) {
        cmd += QStringLiteral(" --uninstall");
        if (removeSettings) cmd += QStringLiteral(" --remove-settings");
    } else {
        cmd += QStringLiteral(" --install");
    }
    return cmd;
}

}  // namespace staged

bool helperHasV11Ops(const std::optional<QString>& installedVersion)
{
    return installedVersion.has_value() && !installedVersion->isEmpty();
}

RoutePlan routeFor(WindowAction a, const RouteFacts& f)
{
    using helper::Op;
    switch (a) {
    case WindowAction::Install:
    case WindowAction::Update: return {Op::Install, Route::Staged};
    case WindowAction::Uninstall: return {Op::Uninstall, Route::Staged};
    case WindowAction::Apply: return {Op::SaveAndApply, Route::Installed};
    case WindowAction::Untick: return {Op::Deactivate, Route::Installed};
    case WindowAction::Start: return {Op::Start, Route::Installed};
    case WindowAction::Stop: return {Op::Stop, Route::Installed};
    case WindowAction::Repair: return {Op::Repair, Route::Installed};
    case WindowAction::Discard:
        // INV-123: a v1.1 op only to a program whose stamp says it has it; a v1.0 helper refuses unknown ops.
        return {helperHasV11Ops(f.installedVersion) ? Op::DiscardPendingIf : Op::DiscardPending, Route::Installed};
    }
    return {};
}

RouteFacts readInstalledFacts(const QString& installedProgram)
{
    RouteFacts f;
    QFile file(installedProgram);
    if (!file.open(QIODevice::ReadOnly)) return f;
    const std::optional<core::BuildStamp> stamp = core::parseBuildStamp(file.size(), [&file](qint64 offset, qint64 length) {
        if (!file.seek(offset)) return QByteArray();
        return file.read(length);
    });
    f.installedVersion = stamp ? stamp->version : QString();
    return f;
}

}  // namespace asb::cli
