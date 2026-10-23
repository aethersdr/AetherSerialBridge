#include "gui/PkexecClient.h"

#include "cli/StagedHelper.h"

#include <QProcess>
#include <QTimer>

#include <memory>

namespace asb::gui {

PkexecClient::PkexecClient(QString program, QStringList arguments)
    : m_program(std::move(program)), m_arguments(std::move(arguments))
{
}

void PkexecClient::run(const QByteArray& request, std::function<void(const Exit&)> done)
{
    launch(m_arguments, request, std::move(done));
}

void PkexecClient::runStaged(const QByteArray& expectedSha256, const QByteArray& program, const QByteArray& request,
                             std::function<void(const Exit&)> done)
{
    launch(cli::staged::pkexecArguments(expectedSha256, program.size(), request.size()), program + request, std::move(done));
}

void PkexecClient::launch(const QStringList& arguments, QByteArray input, std::function<void(const Exit&)> done)
{
    // One process per request. `done` runs once, later, on this thread: from finished() or from a failed start.
    auto* p = new QProcess;
    auto once = std::make_shared<std::function<void(const Exit&)>>(std::move(done));
    auto finish = [once](const Exit& e) {
        if (!*once) return;
        auto f = std::move(*once);
        *once = nullptr;
        f(e);
    };
    QObject::connect(p, &QProcess::finished, p, [p, finish](int code, QProcess::ExitStatus status) {
        Exit e;
        e.code = code;
        e.crashed = status == QProcess::CrashExit;
        e.out = p->readAllStandardOutput();
        e.err = p->readAllStandardError();
        p->deleteLater();
        finish(e);
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [p, finish](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        Exit e;
        e.started = false;
        e.err = p->errorString().toUtf8();
        p->deleteLater();
        // Posted: a start failure may be reported from inside start(), and `done` never runs inside run().
        QTimer::singleShot(0, [finish, e] { finish(e); });
    });
    // The input goes in once the process runs (a program that cannot start reports FailedToStart instead).
    QObject::connect(p, &QProcess::started, p, [p, input = std::move(input)] {
        p->write(input);
        p->closeWriteChannel();
    });
    p->start(m_program, arguments);
}

}  // namespace asb::gui
