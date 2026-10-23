#include "gui/ThemeDiscovery.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStringList>

#include "gui/ThemeValidator.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <QDir>
#else
#  include <cerrno>
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace asb::gui {

bool isBuiltInThemeName(const QString& name) {
    return name == QLatin1String("Default Dark") || name == QLatin1String("Default Light");
}

namespace {

// Up to kMaxThemeBytes + 1 bytes: one more than the cap, so "larger than the cap" is decided by
// what the handle actually returned, whatever the file's size said when it was opened.
constexpr qsizetype kReadLimit = kMaxThemeBytes + 1;

enum class ReadStatus { Ok, TooLarge, Error };

QString themeNameOf(const QByteArray& bytes) {
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    return doc.isObject() ? doc.object().value(QStringLiteral("name")).toString() : QString();
}

struct Candidate {
    QString path;
    QByteArray bytes;
};

// Whether a candidate that was not examined could still be a theme AetherSDR loads. AetherSDR
// lists its themes with QDir::entryList({"*.json"}, QDir::Files): that never yields a directory,
// FIFO, socket or device, but it follows links, has no size cap, and a file the bridge could not
// open or read is one it may still read. Such a candidate may carry the wanted name.
enum class Unexamined {
    NeverATheme,   // not a regular file on the handle: AetherSDR never loads it either
    MayClaimName,  // a link, too large, or unopenable/unreadable: its name is unknown
};

// Shared by both platforms once a candidate's bytes are in hand.
struct Matcher {
    const QString& wanted;
    QList<Candidate> matches;
    QStringList skipped;     // every candidate not examined, with why (detail / log)
    QStringList mayClaim;    // the ones among them that could be a second file of this name

    void skip(const QString& path, const QString& why, Unexamined kind) {
        skipped << path + QStringLiteral(": ") + why;
        if (kind == Unexamined::MayClaimName) {
            mayClaim << path + QStringLiteral(" (") + why + QLatin1Char(')');
        }
    }

    void consider(const QString& path, ReadStatus st, QByteArray bytes) {
        if (st == ReadStatus::TooLarge) {
            skip(path, QStringLiteral("larger than 256 KiB"), Unexamined::MayClaimName);
            return;
        }
        if (st == ReadStatus::Error) {
            skip(path, QStringLiteral("read failed"), Unexamined::MayClaimName);
            return;
        }
        const QString name = themeNameOf(bytes);
        if (name.isEmpty() || isBuiltInThemeName(name) || name != wanted) {
            return;
        }
        matches.append(Candidate{path, std::move(bytes)});
    }

    // Duplicates are ambiguous (WS-11): two examined files with the name, or one examined file
    // with the name next to a candidate that could not be examined and may carry the name too
    // (AetherSDR would load that one as well, and could be showing it).
    ThemeDiscovery finish() {
        ThemeDiscovery r;
        r.detail = skipped.join(QStringLiteral("; "));
        if (matches.isEmpty()) {
            r.status = ThemeDiscovery::Status::NotFound;
        } else if (matches.size() > 1) {
            r.status = ThemeDiscovery::Status::Ambiguous;
            QStringList files;
            for (const Candidate& c : matches) {
                files << c.path;
            }
            r.detail = QStringLiteral("more than one theme file has that name (%1)").arg(files.join(QStringLiteral(", ")));
        } else if (!mayClaim.isEmpty()) {
            r.status = ThemeDiscovery::Status::Ambiguous;
            r.detail = QStringLiteral("%1 has that name, but %2 could not be checked and may have it too")
                           .arg(matches.first().path, mayClaim.join(QStringLiteral(", ")));
        } else {
            r.status = ThemeDiscovery::Status::Found;
            r.path = matches.first().path;
            r.bytes = matches.first().bytes;
        }
        return r;
    }
};

ThemeDiscovery status(ThemeDiscovery::Status s, const QString& detail = {}) {
    ThemeDiscovery r;
    r.status = s;
    r.detail = detail;
    return r;
}

bool endsWithJson(const QString& name) { return name.endsWith(QLatin1String(".json"), Qt::CaseInsensitive); }

#ifndef Q_OS_WIN

struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

ReadStatus readCapped(int fd, QByteArray& out) {
    out.resize(kReadLimit);
    qsizetype got = 0;
    while (got < kReadLimit) {
        const ssize_t n = ::read(fd, out.data() + got, static_cast<size_t>(kReadLimit - got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            out.clear();
            return ReadStatus::Error;
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    out.resize(got);
    if (got > kMaxThemeBytes) {
        out.clear();
        return ReadStatus::TooLarge;
    }
    return ReadStatus::Ok;
}

ThemeDiscovery findPosix(const QString& themesDir, const QString& wanted, const ThemeDiscoveryHooks& hooks) {
    Fd dir;
    dir.fd = ::open(QFile::encodeName(themesDir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
    if (dir.fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return status(ThemeDiscovery::Status::NoDirectory);
        }
        return status(ThemeDiscovery::Status::Error, QStringLiteral("cannot open the theme folder"));
    }

    // Names only. readdir never opens or stats an entry.
    QList<QByteArray> jsonNames;
    {
        const int listFd = ::fcntl(dir.fd, F_DUPFD_CLOEXEC, 0);
        DIR* d = listFd >= 0 ? ::fdopendir(listFd) : nullptr;
        if (!d) {
            if (listFd >= 0) {
                ::close(listFd);
            }
            return status(ThemeDiscovery::Status::Error, QStringLiteral("cannot list the theme folder"));
        }
        int entries = 0;
        int listError = 0;
        for (;;) {
            const dirent* e = nullptr;
            if (hooks.failListing && hooks.failListing(static_cast<int>(jsonNames.size()))) {
                errno = EIO;  // what readdir reports when reading the directory fails
            } else {
                errno = 0;  // readdir leaves errno alone at the end and sets it on an error
                e = ::readdir(d);
            }
            if (!e) {
                listError = errno;  // read before anything else can change it
                break;
            }
            const QByteArray raw(e->d_name);
            if (raw == "." || raw == "..") {
                continue;
            }
            if (++entries > kMaxThemeDirEntries) {
                ::closedir(d);
                return status(ThemeDiscovery::Status::TooManyEntries);
            }
            if (endsWithJson(QFile::decodeName(raw))) {
                jsonNames.append(raw);
            }
        }
        ::closedir(d);
        // An error is not the end of the folder: entries after it were never seen, so neither a
        // duplicate nor the budgets were checked. Nothing listed so far may be followed.
        if (listError != 0) {
            return status(ThemeDiscovery::Status::Error,
                          QStringLiteral("listing the theme folder stopped early: %1").arg(qt_error_string(listError)));
        }
    }
    if (jsonNames.size() > kMaxThemeJsonEntries) {
        return status(ThemeDiscovery::Status::TooManyJson);
    }

    Matcher m{wanted, {}, {}, {}};
    for (const QByteArray& raw : jsonNames) {
        const QString path = themesDir + QLatin1Char('/') + QFile::decodeName(raw);
        // Hidden, as Qt decides it on Unix: the name starts with a dot. AetherSDR's listing has no
        // QDir::Hidden, so it never sees this entry; neither does the match. Not even opened.
        if (raw.startsWith('.')) {
            m.skip(path, QStringLiteral("hidden"), Unexamined::NeverATheme);
            continue;
        }
        Fd f;
        // Relative to the directory handle: the name is resolved in the directory listed, and a
        // FIFO cannot block the open, a symlink is not followed.
        f.fd = ::openat(dir.fd, raw.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
        if (f.fd < 0) {
            const int openError = errno;
            // The open failed, so the entry is classified by a no-follow stat of its name: a
            // socket (ENXIO) or a device without a driver fails the open but is still not a
            // regular file, and never a theme. Anything else, or a failed stat (ENOENT, ESTALE:
            // gone or replaced since the listing), may carry the name.
            struct stat ls {};
            const bool statted = ::fstatat(dir.fd, raw.constData(), &ls, AT_SYMLINK_NOFOLLOW) == 0;
            if (openError == ELOOP || (statted && S_ISLNK(ls.st_mode))) {
                // AetherSDR's listing resolves links (no QDir::NoSymLinks) and, without
                // QDir::System, drops a link whose target does not resolve; without QDir::Dirs it
                // drops one that resolves to a folder. Anything else it lists. The bridge still
                // never follows the link itself: a following stat reads only the target's type.
                struct stat ts {};
                if (::fstatat(dir.fd, raw.constData(), &ts, 0) != 0) {
                    m.skip(path, QStringLiteral("a broken link"), Unexamined::NeverATheme);
                } else if (S_ISDIR(ts.st_mode)) {
                    m.skip(path, QStringLiteral("a link to a folder"), Unexamined::NeverATheme);
                } else {
                    m.skip(path, QStringLiteral("a link"), Unexamined::MayClaimName);
                }
            } else if (statted && !S_ISREG(ls.st_mode)) {
                m.skip(path, QStringLiteral("not a regular file"), Unexamined::NeverATheme);
            } else {
                m.skip(path, QStringLiteral("cannot open"), Unexamined::MayClaimName);
            }
            continue;
        }
        struct stat st {};
        if (::fstat(f.fd, &st) != 0) {
            m.skip(path, QStringLiteral("cannot examine"), Unexamined::MayClaimName);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            m.skip(path, QStringLiteral("not a regular file"), Unexamined::NeverATheme);
            continue;
        }
        if (hooks.afterOpen) {
            hooks.afterOpen(path);
        }
        QByteArray bytes;
        const ReadStatus rs = readCapped(f.fd, bytes);
        m.consider(path, rs, std::move(bytes));
    }
    return m.finish();
}

#else  // Q_OS_WIN

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};

struct Find {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Find() {
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
        }
    }
};

ReadStatus readCapped(HANDLE h, QByteArray& out) {
    out.resize(kReadLimit);
    qsizetype got = 0;
    while (got < kReadLimit) {
        DWORD n = 0;
        if (!ReadFile(h, out.data() + got, static_cast<DWORD>(kReadLimit - got), &n, nullptr)) {
            out.clear();
            return ReadStatus::Error;
        }
        if (n == 0) {
            break;
        }
        got += static_cast<qsizetype>(n);
    }
    out.resize(got);
    if (got > kMaxThemeBytes) {
        out.clear();
        return ReadStatus::TooLarge;
    }
    return ReadStatus::Ok;
}

ThemeDiscovery findWindows(const QString& themesDir, const QString& wanted, const ThemeDiscoveryHooks& hooks) {
    const std::wstring pattern = QDir::toNativeSeparators(themesDir + QStringLiteral("/*")).toStdWString();
    struct Listed {
        QString name;
        bool hidden = false;  // FILE_ATTRIBUTE_HIDDEN in the listing, which is where Qt reads it
    };
    QList<Listed> jsonNames;
    {
        WIN32_FIND_DATAW fd{};
        Find find;
        // FindExInfoBasic: names only, no short names.
        find.h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                  FIND_FIRST_EX_LARGE_FETCH);
        if (find.h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND || err == ERROR_DIRECTORY) {
                return status(ThemeDiscovery::Status::NoDirectory);
            }
            return status(ThemeDiscovery::Status::Error, QStringLiteral("cannot list the theme folder"));
        }
        int entries = 0;
        DWORD listError = ERROR_SUCCESS;
        const auto next = [&]() {
            BOOL more = FALSE;
            if (hooks.failListing && hooks.failListing(static_cast<int>(jsonNames.size()))) {
                SetLastError(ERROR_IO_DEVICE);  // what FindNextFileW reports when reading the directory fails
            } else {
                more = FindNextFileW(find.h, &fd);
            }
            if (!more) {
                listError = GetLastError();  // read before anything else can change it
            }
            return more != FALSE;
        };
        do {
            const QString name = QString::fromWCharArray(fd.cFileName);
            if (name == QLatin1String(".") || name == QLatin1String("..")) {
                continue;
            }
            if (++entries > kMaxThemeDirEntries) {
                return status(ThemeDiscovery::Status::TooManyEntries);
            }
            if (endsWithJson(name)) {
                jsonNames.append(Listed{name, (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0});
            }
        } while (next());
        // Only ERROR_NO_MORE_FILES is the end of the folder; any other error leaves entries
        // unseen, so neither a duplicate nor the budgets were checked.
        if (listError != ERROR_NO_MORE_FILES) {
            return status(ThemeDiscovery::Status::Error,
                          QStringLiteral("listing the theme folder stopped early: %1")
                              .arg(qt_error_string(static_cast<int>(listError))));
        }
    }
    if (jsonNames.size() > kMaxThemeJsonEntries) {
        return status(ThemeDiscovery::Status::TooManyJson);
    }

    Matcher m{wanted, {}, {}, {}};
    for (const Listed& listed : jsonNames) {
        const QString path = themesDir + QLatin1Char('/') + listed.name;
        // Hidden, as Qt decides it on Windows: the hidden attribute, from the listing (a leading
        // dot means nothing here). AetherSDR's listing has no QDir::Hidden, so it never sees this
        // entry; neither does the match. Not even opened.
        if (listed.hidden) {
            m.skip(path, QStringLiteral("hidden"), Unexamined::NeverATheme);
            continue;
        }
        const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
        Handle f;
        // The reparse point itself, never its target; a directory cannot be opened without
        // FILE_FLAG_BACKUP_SEMANTICS, so it fails here.
        f.h = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f.h == INVALID_HANDLE_VALUE) {
            // Classified by what the name is now, not by the listing (which may be stale: a
            // directory listed, then replaced by a file that cannot be opened, must not pass as
            // "not a regular file"). GetFileAttributesW does not follow a final reparse point.
            const DWORD attributes = GetFileAttributesW(native.c_str());
            // A folder first, reparse point or not: a junction or folder link is a folder to
            // Qt, and QDir::Files without QDir::Dirs never lists it.
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                m.skip(path, QStringLiteral("cannot open"), Unexamined::MayClaimName);  // gone, or no access
            } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                m.skip(path, QStringLiteral("not a regular file"), Unexamined::NeverATheme);
            } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                m.skip(path, QStringLiteral("a link"), Unexamined::MayClaimName);
            } else {
                m.skip(path, QStringLiteral("cannot open"), Unexamined::MayClaimName);
            }
            continue;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(f.h, &info)) {
            m.skip(path, QStringLiteral("cannot examine"), Unexamined::MayClaimName);
            continue;
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            m.skip(path, QStringLiteral("not a regular file"), Unexamined::NeverATheme);
            continue;
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            m.skip(path, QStringLiteral("a link"), Unexamined::MayClaimName);
            continue;
        }
        if (GetFileType(f.h) != FILE_TYPE_DISK) {
            m.skip(path, QStringLiteral("not a regular file"), Unexamined::NeverATheme);
            continue;
        }
        if (hooks.afterOpen) {
            hooks.afterOpen(path);
        }
        QByteArray bytes;
        const ReadStatus rs = readCapped(f.h, bytes);
        m.consider(path, rs, std::move(bytes));
    }
    return m.finish();
}

#endif

}  // namespace

ThemeDiscovery findUserTheme(const QString& themesDir, const QString& name, const ThemeDiscoveryHooks& hooks) {
#ifdef Q_OS_WIN
    return findWindows(themesDir, name, hooks);
#else
    return findPosix(themesDir, name, hooks);
#endif
}

}  // namespace asb::gui
