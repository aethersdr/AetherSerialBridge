#pragma once

// Test builds only (ASB_TEST_SEAMS): the systemd suite's seams (WS-8), shared by `--test-op` and, from WS-9, by the
// CLI's own operations in a test build. Never in a release build.
//
// Failure points (ASB_TEST_FAIL, comma separated): firewall:sync throws (an apply that fails after the barrier).
// Pause points (ASB_TEST_PAUSE, comma separated), where the process prints "ASB-PAUSED <point>" on stderr and
// waits forever so the suite can kill it:
//   step:<name>                 SystemdService steps: after-mask, before-allow-starts, before-start
//   firewall:<method>           the (null) firewall's sync, sweep, ...
//   sys:<before|after>:<call>:<name>   a PosixSys call (openat, renameat, fsync, ...) whose name argument
//                               (for renameat: the new name) contains <name>
// ASB_TEST_RESULT_JSON=1: the CLI prints every final Result as one JSON line on stdout (as --test-op does).

#include "core/Interfaces.h"
#include "platform/posix/LinuxPlatform.h"
#include "platform/posix/PosixSys.h"

#include <QStringList>

#include <cerrno>

namespace asb::app::testseams {

QStringList pausePoints();
QStringList failPoints();   // ASB_TEST_FAIL, comma separated
[[noreturn]] void pauseHere(const QString& point);
void maybePause(const QString& point);
void maybeFail(const QString& point);

// A PosixSys that pauses at a configured call (ASB_TEST_PAUSE sys:<before|after>:<call>:<name>), or fails one
// (ASB_TEST_FAIL sys:<call>:<name>: the call returns -1 with EIO without reaching the kernel; WS-13's
// activation_matrix uses it for the rename that fails after the sync).
class PausingSys final : public asb::posix::PosixSys {
public:
    PausingSys() : m_real(asb::posix::realPosixSys()), m_points(pausePoints()), m_fails(failPoints()) {}

    int open(const char* p, int f, mode_t m) override { return wrap("open", p, [&] { return m_real.open(p, f, m); }); }
    int openat(int d, const char* n, int f, mode_t m) override
    {
        return wrap("openat", n, [&] { return m_real.openat(d, n, f, m); });
    }
    int close(int fd) override { return m_real.close(fd); }
    int fstat(int fd, struct stat* st) override { return m_real.fstat(fd, st); }
    int fstatat(int d, const char* n, struct stat* st, int f) override { return m_real.fstatat(d, n, st, f); }
    int fstatfs(int fd, struct statfs* st) override { return m_real.fstatfs(fd, st); }
    int fchown(int fd, uid_t u, gid_t g) override { return m_real.fchown(fd, u, g); }
    int fchmod(int fd, mode_t m) override { return m_real.fchmod(fd, m); }
    ssize_t read(int fd, void* b, size_t n) override { return m_real.read(fd, b, n); }
    ssize_t pread(int fd, void* b, size_t n, off_t o) override { return m_real.pread(fd, b, n, o); }
    ssize_t write(int fd, const void* b, size_t n) override { return m_real.write(fd, b, n); }
    int ftruncate(int fd, off_t l) override { return m_real.ftruncate(fd, l); }
    int fsync(int fd) override { return m_real.fsync(fd); }
    int renameat(int a, const char* an, int b, const char* bn) override
    {
        return wrap("renameat", bn, [&] { return m_real.renameat(a, an, b, bn); });
    }
    int renameNoReplace(int a, const char* an, int b, const char* bn) override
    {
        return wrap("renameNoReplace", bn, [&] { return m_real.renameNoReplace(a, an, b, bn); });
    }
    int unlinkat(int d, const char* n, int f) override { return wrap("unlinkat", n, [&] { return m_real.unlinkat(d, n, f); }); }
    int mkdirat(int d, const char* n, mode_t m) override { return wrap("mkdirat", n, [&] { return m_real.mkdirat(d, n, m); }); }
    int flock(int fd, int op) override { return m_real.flock(fd, op); }
    ssize_t readlinkat(int d, const char* n, char* b, size_t s) override { return m_real.readlinkat(d, n, b, s); }
    std::optional<QStringList> listDirectory(int d) override { return m_real.listDirectory(d); }

private:
    template <typename F>
    int wrap(const char* call, const char* name, F&& f)
    {
        check(QStringLiteral("before"), call, name);
        for (const QString& p : m_fails) {
            const QStringList x = p.split(QLatin1Char(':'));
            if (x.size() == 3 && x[0] == QLatin1String("sys") && x[1] == QLatin1String(call) && name &&
                QString::fromLocal8Bit(name).contains(x[2])) {
                errno = EIO;
                return -1;
            }
        }
        const int r = f();
        check(QStringLiteral("after"), call, name);
        return r;
    }
    void check(const QString& when, const char* call, const char* name)
    {
        for (const QString& p : m_points) {
            const QStringList f = p.split(QLatin1Char(':'));
            if (f.size() != 4 || f[0] != QLatin1String("sys") || f[1] != when || f[2] != QLatin1String(call)) continue;
            if (name && QString::fromLocal8Bit(name).contains(f[3])) pauseHere(p);
        }
    }
    asb::posix::PosixSys& m_real;
    QStringList m_points;
    QStringList m_fails;
};

class PausingFirewall final : public IFirewall {
public:
    explicit PausingFirewall(IFirewall& inner) : m_inner(inner) {}
    void connect(Deadline d) override { maybePause(QStringLiteral("firewall:connect")); m_inner.connect(d); }
    void sync(const BridgeConfig& c, const FirewallGeneration& g, Deadline d) override
    {
        maybePause(QStringLiteral("firewall:sync"));
        maybeFail(QStringLiteral("firewall:sync"));
        m_inner.sync(c, g, d);
    }
    void rollback(const FirewallGeneration& g, Deadline d) override { m_inner.rollback(g, d); }
    void sweep(const FirewallGeneration& g, Deadline d) override
    {
        maybePause(QStringLiteral("firewall:sweep"));
        m_inner.sweep(g, d);
    }
    void removeAll(Deadline d) override
    {
        maybeFail(QStringLiteral("firewall:removeAll"));
        m_inner.removeAll(d);
    }

private:
    IFirewall& m_inner;
};

// The JSON line the suite reads: the Result, plus what the adapters reported beside it.
QByteArray resultJson(const Result&, const QStringList& reports, asb::posix::LinuxPlatform&);

}  // namespace asb::app::testseams
