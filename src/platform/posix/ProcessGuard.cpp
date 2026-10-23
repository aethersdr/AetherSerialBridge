#include "platform/posix/ProcessGuard.h"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace asb::posix::ProcessGuard {
namespace {

#if defined(__x86_64__)
constexpr unsigned int kArch = AUDIT_ARCH_X86_64;
constexpr unsigned int kX32Bit = 0x40000000u;   // __X32_SYSCALL_BIT
#elif defined(__aarch64__)
constexpr unsigned int kArch = AUDIT_ARCH_AARCH64;
#else
#error "ProcessGuard: only x86-64 and AArch64 are supported"
#endif

constexpr unsigned int kNr = offsetof(struct seccomp_data, nr);
constexpr unsigned int kArchOff = offsetof(struct seccomp_data, arch);
// The low 32 bits of the first argument (little-endian on both ABIs): clone's flags.
constexpr unsigned int kArg0Lo = offsetof(struct seccomp_data, args);

#define ASB_STMT(code, k) sock_filter{static_cast<unsigned short>(code), 0, 0, static_cast<unsigned int>(k)}
#define ASB_JUMP(code, k, jt, jf) \
    sock_filter{static_cast<unsigned short>(code), static_cast<unsigned char>(jt), static_cast<unsigned char>(jf), static_cast<unsigned int>(k)}

constexpr unsigned int kAllow = SECCOMP_RET_ALLOW;
constexpr unsigned int kKill = SECCOMP_RET_KILL_PROCESS;
constexpr unsigned int kEperm = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
constexpr unsigned int kEnosys = SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA);

// Jump offsets count the instructions skipped: each denylist entry jumps to the matching RET at the end
// (tests/systemd/seccomp_probe.cpp exercises every one of them).
const sock_filter kProgram[] = {
    // 1. the ABI
    ASB_STMT(BPF_LD | BPF_W | BPF_ABS, kArchOff),
    ASB_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0),
    ASB_STMT(BPF_RET | BPF_K, kKill),
    ASB_STMT(BPF_LD | BPF_W | BPF_ABS, kNr),
#if defined(__x86_64__)
    // 2. x32 numbers, before anything is matched
    ASB_JUMP(BPF_JMP | BPF_JSET | BPF_K, kX32Bit, 0, 1),
    ASB_STMT(BPF_RET | BPF_K, kKill),
    // 3. the denylist
    ASB_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fork, 8, 0),
    ASB_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_vfork, 7, 0),
#endif
    ASB_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execve, 6, 0),
    ASB_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execveat, 5, 0),
    ASB_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 5, 0),
    ASB_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 1, 0),
    ASB_STMT(BPF_RET | BPF_K, kAllow),
    // clone: allowed only in the thread form
    ASB_STMT(BPF_LD | BPF_W | BPF_ABS, kArg0Lo),
    ASB_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 2, 0),
    // fork, vfork, execve, execveat, process-form clone
    ASB_STMT(BPF_RET | BPF_K, kEperm),
    // clone3
    ASB_STMT(BPF_RET | BPF_K, kEnosys),
    // thread-form clone
    ASB_STMT(BPF_RET | BPF_K, kAllow),
};

#undef ASB_STMT
#undef ASB_JUMP

}  // namespace

long realSeccomp(unsigned int op, unsigned int flags, void* args)
{
    return ::syscall(SYS_seccomp, op, flags, args);
}

bool install(QString* why, SeccompCall call)
{
    // Always, root included: the kernel installs a filter only for a thread with no_new_privs set OR
    // CAP_SYS_ADMIN in its user namespace, and uid 0 is not CAP_SYS_ADMIN (a container's root, a unit with a
    // reduced CapabilityBoundingSet, a user namespace). no_new_privs costs nothing here: the filter forbids
    // exec, the only thing it restricts.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        if (why) *why = QStringLiteral("prctl(PR_SET_NO_NEW_PRIVS): %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    sock_fprog prog{};
    prog.len = static_cast<unsigned short>(sizeof kProgram / sizeof kProgram[0]);
    prog.filter = const_cast<sock_filter*>(kProgram);
    errno = 0;
    const long r = call(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (r == 0) return true;
    if (why) {
        if (r > 0)
            *why = QStringLiteral("seccomp(TSYNC): thread %1 could not be synchronised").arg(r);
        else
            *why = QStringLiteral("seccomp: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
    }
    return false;
}

void installOrExit(SeccompCall call)
{
    QString why;
    if (install(&why, call)) return;
    const QByteArray line = QByteArrayLiteral("aether-serial-bridge: cannot install the process guard: ") + why.toLocal8Bit() + '\n';
    const ssize_t w = ::write(STDERR_FILENO, line.constData(), size_t(line.size()));
    (void)w;
    ::_exit(kExitGuardFailed);
}

}  // namespace asb::posix::ProcessGuard
