#pragma once

// The Linux window's process boundary to the pkexec helper (master §5.7, D2; WS-9b HelperClient.h; INV-112).
// `pkexec /usr/local/bin/aether-serial-bridge --helper`, the request written to its stdin and stdin closed; the exit
// code and both outputs handed to the HelperClient, which makes every decision. This file is the only process creation
// in the product, allow-listed by object in process_creation_audit for the Linux GUI target.

#include "cli/HelperClient.h"

#include <QString>
#include <QStringList>

namespace asb::gui {

class PkexecClient final : public cli::IHelperProcess {
public:
    static constexpr char kPkexec[] = "/usr/bin/pkexec";
    static constexpr char kHelper[] = "/usr/local/bin/aether-serial-bridge";

    // `program` / `arguments`: the defaults above; test builds pass a stand-in.
    PkexecClient(QString program = QString::fromLatin1(kPkexec),
                 QStringList arguments = {QString::fromLatin1(kHelper), QStringLiteral("--helper")});
    void run(const QByteArray& request, std::function<void(const Exit&)> done) override;
    void runStaged(const QByteArray& expectedSha256, const QByteArray& program, const QByteArray& request,
                   std::function<void(const Exit&)> done) override;

private:
    void launch(const QStringList& arguments, QByteArray input, std::function<void(const Exit&)> done);
    QString m_program;
    QStringList m_arguments;
};

}  // namespace asb::gui
