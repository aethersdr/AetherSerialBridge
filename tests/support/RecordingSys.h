#pragma once

// A PosixSys over the REAL calls that records them (WS-8 native tests). Each fd an open/openat returns is
// remembered with the name it was opened by, so the log reads "fsync config.json.<hex>.asbtmp" rather than
// "fsync 7"; a directory the test opened itself can be labelled (label(fd, "DIR")). Hooks run after a call
// whose log entry starts with a given prefix (the swap of INV-65), and failures can be injected per entry
// prefix. fstatfs can report a chosen file-system magic (INV-116 rows on an ext4 build host).

#include "platform/posix/PosixSys.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace asb::test {

class RecordingSys final : public asb::posix::PosixSys {
public:
    RecordingSys() : m_real(asb::posix::realPosixSys()) {}

    QStringList log() const;
    void clearLog();
    void label(int fd, const QString& name);
    // Runs `f` after the first call whose log entry starts with `prefix` (once).
    void after(const QString& prefix, std::function<void()> f);
    // Makes calls whose log entry starts with `prefix` fail with `err` (every time).
    void failWith(const QString& prefix, int err);
    // fstatfs reports this f_type (on every fd) when set.
    std::optional<long> fsType;
    // Edits what a successful fstat reports, when set (a file's times pinned to one coarse tick, v1.1 INV-120).
    std::function<void(struct stat&)> editFstat;

    int open(const char* path, int flags, mode_t mode) override;
    int openat(int dirfd, const char* name, int flags, mode_t mode) override;
    int close(int fd) override;
    int fstat(int fd, struct stat* st) override;
    int fstatat(int dirfd, const char* name, struct stat* st, int flags) override;
    int fstatfs(int fd, struct statfs* st) override;
    int fchown(int fd, uid_t uid, gid_t gid) override;
    int fchmod(int fd, mode_t mode) override;
    ssize_t read(int fd, void* buf, size_t n) override;
    ssize_t pread(int fd, void* buf, size_t n, off_t off) override;
    ssize_t write(int fd, const void* buf, size_t n) override;
    int ftruncate(int fd, off_t len) override;
    int fsync(int fd) override;
    int renameat(int olddirfd, const char* oldname, int newdirfd, const char* newname) override;
    int renameNoReplace(int olddirfd, const char* oldname, int newdirfd, const char* newname) override;
    int unlinkat(int dirfd, const char* name, int flags) override;
    int mkdirat(int dirfd, const char* name, mode_t mode) override;
    int flock(int fd, int op) override;
    ssize_t readlinkat(int dirfd, const char* name, char* buf, size_t n) override;
    std::optional<QStringList> listDirectory(int dirfd) override;

private:
    QString nameOf(int fd) const;
    // Records `entry`; returns the injected errno for it (0 = run the real call).
    int before(const QString& entry);
    void afterCall(const QString& entry);

    asb::posix::PosixSys& m_real;
    mutable QMutex m_mutex;
    QStringList m_log;
    QHash<int, QString> m_names;
    std::vector<std::pair<QString, std::function<void()>>> m_after;
    std::vector<std::pair<QString, int>> m_fail;
};

}  // namespace asb::test
