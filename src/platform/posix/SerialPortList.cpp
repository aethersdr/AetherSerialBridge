#include "platform/posix/SerialPortList.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>

namespace asb::posix::SerialPortList {
namespace {

// Entry names of a directory, without opening anything inside it.
std::optional<QStringList> names(const QString& dir)
{
    const QByteArray d = QFile::encodeName(dir);
    DIR* h = ::opendir(d.constData());
    if (!h) return std::nullopt;
    QStringList out;
    while (struct dirent* e = ::readdir(h)) {
        const QByteArray n(e->d_name);
        if (n == "." || n == "..") continue;
        out << QFile::decodeName(n);
    }
    ::closedir(h);
    return out;
}

// The final target of a chain of symbolic links, resolved with readlink and lstat only: the path the last link names, whether or not it exists.
// nullopt when the entry is not a link at all, or the chain loops.
std::optional<QString> finalTarget(const QString& link)
{
    QString cur = link;
    for (int hops = 0; hops < 40; ++hops) {
        const QByteArray c = QFile::encodeName(cur);
        struct stat st {};
        if (::lstat(c.constData(), &st) != 0) {
            if (hops > 0 && errno == ENOENT) return QDir::cleanPath(cur);   // dangling: the target is named anyway
            return std::nullopt;
        }
        if (!S_ISLNK(st.st_mode)) {
            if (hops == 0) return std::nullopt;   // not a link
            return QDir::cleanPath(cur);
        }
        char buf[PATH_MAX];
        const ssize_t n = ::readlink(c.constData(), buf, sizeof buf - 1);
        if (n < 0) return std::nullopt;
        const QString t = QFile::decodeName(QByteArray(buf, int(n)));
        cur = t.startsWith(QLatin1Char('/')) ? t : QDir::cleanPath(cur.left(cur.lastIndexOf(QLatin1Char('/')) + 1) + t);
    }
    return std::nullopt;
}

}  // namespace

QList<Entry> byId(const QString& dir)
{
    QList<Entry> out;
    const std::optional<QStringList> n = names(dir);
    if (!n) return out;
    QStringList sorted = *n;
    std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) {
        return QString::compare(a, b, Qt::CaseSensitive) < 0;   // ordinal (UTF-16 code units)
    });
    for (const QString& name : sorted) {
        const QString p = dir + QLatin1Char('/') + name;
        out << Entry{p, finalTarget(p)};
    }
    return out;
}

QStringList kernelNames(const QString& devDir)
{
    static const QRegularExpression re(QStringLiteral("^tty(USB|ACM)(\\d+)$"));
    struct K {
        QString kind;
        qulonglong number;
        QString path;
    };
    QList<K> ks;
    const std::optional<QStringList> n = names(devDir);
    if (!n) return {};
    for (const QString& name : *n) {
        const QRegularExpressionMatch m = re.match(name);
        if (!m.hasMatch()) continue;
        bool ok = false;
        const qulonglong num = m.captured(2).toULongLong(&ok);
        if (!ok) continue;
        ks << K{m.captured(1), num, devDir + QLatin1Char('/') + name};
    }
    std::sort(ks.begin(), ks.end(), [](const K& a, const K& b) {
        const int c = QString::compare(a.kind, b.kind, Qt::CaseSensitive);
        return c != 0 ? c < 0 : a.number < b.number;
    });
    QStringList out;
    for (const K& k : ks) out << k.path;
    return out;
}

void print(QTextStream& o, const QString& byIdDir, const QString& devDir)
{
    const QList<Entry> ids = byId(byIdDir);
    const QStringList kernel = kernelNames(devDir);
    if (ids.isEmpty() && kernel.isEmpty()) {
        o << "No USB serial adapters found. Plug one in and run --list-ports again.\n";
        return;
    }
    o << "Stable names (use these in --add-port):\n";
    if (ids.isEmpty()) o << "  none\n";
    for (const Entry& e : ids) o << "  " << e.path << " -> " << e.target.value_or(QStringLiteral("(dangling)")) << "\n";
    o << "Kernel names (can change after a reboot or replug):\n";
    if (kernel.isEmpty()) o << "  none\n";
    for (const QString& k : kernel) o << "  " << k << "\n";
}

}  // namespace asb::posix::SerialPortList
