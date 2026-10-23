#include "platform/win/InstallBinary.h"

#include "core/AtomicJson.h"

#include <QCryptographicHash>

namespace asb::win::InstallBinary {

QByteArray sha256Hex(const QByteArray& b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

Result install(FileIo& io, const DirHandle& dir, const QString& name, const QByteArray& image,
               const QByteArray& expectedSha256Hex, const std::function<void()>& afterWrite,
               const std::function<void()>& beforeCommit)
{
    Result r;
    if (image.isEmpty() || image.size() > kMaxImageBytes) {
        r.error = QStringLiteral("the program image to install is empty or too large");
        return r;
    }
    QString err;
    std::unique_ptr<TempFile> temp = io.createTemp(dir, OwnerPolicy::Config, defaultNameGen(name), &err);
    if (!temp) {
        r.error = QStringLiteral("could not create the install copy: %1").arg(err);
        return r;
    }
    if (!temp->write(image, &err) || !temp->flush(&err)) {
        r.error = QStringLiteral("could not write the install copy: %1").arg(err);
        return r;
    }
    if (afterWrite) afterWrite();
    // INV-41: the hash is of the COPY, read back through the temp's own handle — never of the source.
    const QByteArray copy = temp->readBack(kMaxImageBytes, &err);
    if (!err.isEmpty()) {
        r.error = QStringLiteral("could not read the install copy back: %1").arg(err);
        return r;
    }
    const QByteArray got = sha256Hex(copy);
    if (got != expectedSha256Hex.toLower()) {
        r.error = QStringLiteral("the install copy does not match this build (SHA-256 %1, expected %2); nothing was "
                                 "installed")
                      .arg(QString::fromLatin1(got), QString::fromLatin1(expectedSha256Hex));
        return r;
    }
    if (beforeCommit) beforeCommit();   // throws: the temp is deleted by its destructor, nothing is published
    const TempFile::Commit c = temp->commitAs(name);
    r.replaced = c.replaced;
    r.durable = c.durable;
    if (!c.replaced) {
        r.error = QStringLiteral("could not replace %1: %2").arg(name, c.error);
        return r;
    }
    r.ok = true;
    if (!c.durable) r.error = c.error;   // installed, but the directory flush failed: the caller decides (WindowsService stops)
    return r;
}

}  // namespace asb::win::InstallBinary
