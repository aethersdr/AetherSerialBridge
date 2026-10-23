// asb_seccomp_probe: installs the product's process guard (asb::posix::ProcessGuard, the same call every Linux
// mode entry makes) and then tries ONE forbidden or allowed thing, printing what happened (WS-8, INV-110,
// INV-112 runtime half). Test code only: the raw system calls here are the attacks, not product code.
//
//   asb_seccomp_probe <case>
//     fork-libc | fork-raw | vfork-raw | clone-process | clone3 | execve | execveat   -> "DENIED <errno name>"
//     int80 | x32                                       -> the process is killed by SIGSYS (prints nothing after)
//     threads | dbus                                    -> "OK ..."
//     tsync-fake | install-failure                      -> exit 5 (the guard's own refusal)
//     control-fork                                      -> no guard: fork works ("ALLOWED"), proving the probe
//
// Anything that unexpectedly succeeds prints "ALLOWED" and, for a child, exits at once.

#include "platform/posix/ProcessGuard.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QThread>

#include <fcntl.h>
#include <linux/sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

// Every case ends through _exit: under the guard, at-exit work that creates processes (LeakSanitizer's
// StopTheWorld clones without CLONE_THREAD) must not run, or a sanitizer build reports a false failure.
[[noreturn]] void done(int code)
{
    std::fflush(stdout);
    ::_exit(code);
}

int report(const char* what, long r)
{
    if (r == -1) {
        const char* n = strerrorname_np(errno);
        std::printf("DENIED %s %s\n", what, n ? n : "?");
        return 0;
    }
    std::printf("ALLOWED %s %ld\n", what, r);
    return 1;
}

long fakeTsync(unsigned int, unsigned int, void*)
{
    return 1234;   // the thread id TSYNC reports when a thread could not be synchronised
}

long fakeFailure(unsigned int, unsigned int, void*)
{
    errno = EINVAL;
    return -1;
}

int forkLike(long r, const char* what)
{
    if (r == 0) ::_exit(97);   // a child that should never have existed
    if (r > 0) ::waitpid(pid_t(r), nullptr, 0);
    return report(what, r);
}

}  // namespace

int probeMain(int argc, char** argv);

int main(int argc, char** argv)
{
    done(probeMain(argc, argv));
}

int probeMain(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("usage: asb_seccomp_probe <case>\n", stderr);
        return 2;
    }
    const std::string c = argv[1];
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (c == "tsync-fake") {
        asb::posix::ProcessGuard::installOrExit(&fakeTsync);
        std::puts("ALLOWED tsync-fake (continued after a positive TSYNC result)");
        return 1;
    }
    if (c == "install-failure") {
        asb::posix::ProcessGuard::installOrExit(&fakeFailure);
        std::puts("ALLOWED install-failure (continued after a failed install)");
        return 1;
    }
    if (c != "control-fork") asb::posix::ProcessGuard::installOrExit();

    if (c == "fork-libc" || c == "control-fork") return forkLike(long(::fork()), c.c_str());
#if defined(__x86_64__)
    if (c == "fork-raw") return forkLike(::syscall(SYS_fork), "fork-raw");
    if (c == "vfork-raw") return forkLike(::syscall(SYS_vfork), "vfork-raw");
#else
    if (c == "fork-raw" || c == "vfork-raw") {
        std::puts("SKIP no fork/vfork system call on this ABI");
        return 0;
    }
#endif
    if (c == "clone-process") return forkLike(::syscall(SYS_clone, long(SIGCHLD), 0L, 0L, 0L, 0L), "clone-process");
    if (c == "clone3") {
        struct clone_args a {};
        a.exit_signal = SIGCHLD;
        return forkLike(::syscall(SYS_clone3, &a, sizeof a), "clone3");
    }
    if (c == "execve") {
        char* const args[] = {const_cast<char*>("/bin/true"), nullptr};
        return report("execve", ::execve("/bin/true", args, environ));
    }
    if (c == "execveat") {
        char* const args[] = {const_cast<char*>("/bin/true"), nullptr};
        return report("execveat", ::syscall(SYS_execveat, AT_FDCWD, "/bin/true", args, environ, 0));
    }
#if defined(__x86_64__)
    if (c == "int80") {
        long r = 20;   // i386 getpid
        asm volatile("int $0x80" : "+a"(r) : : "memory");
        std::printf("ALLOWED int80 %ld\n", r);
        return 1;
    }
    if (c == "x32") {
        const long r = ::syscall(39 | 0x40000000L);   // x32 getpid
        std::printf("ALLOWED x32 %ld errno=%d\n", r, errno);
        return 1;
    }
#else
    if (c == "int80" || c == "x32") {
        std::puts("SKIP x86-64 only");
        return 0;
    }
#endif
    if (c == "threads") {
        int ran = 0;
        std::thread t([&] { ran += 1; });
        t.join();
        QThread* q = QThread::create([&] { ran += 10; });
        q->start();
        q->wait();
        delete q;
        std::printf("OK threads %d\n", ran);
        return ran == 11 ? 0 : 1;
    }
    if (c == "dbus") {
        QCoreApplication app(argc, argv);
        QDBusConnection bus = QDBusConnection::systemBus();
        if (!bus.isConnected()) {
            std::printf("FAILED dbus %s\n", qPrintable(bus.lastError().message()));
            return 1;
        }
        const bool registered = bus.interface()->isServiceRegistered(QStringLiteral("org.freedesktop.systemd1"));
        std::printf("OK dbus systemd1=%d\n", int(registered));
        return registered ? 0 : 1;
    }
    std::fprintf(stderr, "unknown case %s\n", c.c_str());
    return 2;
}
