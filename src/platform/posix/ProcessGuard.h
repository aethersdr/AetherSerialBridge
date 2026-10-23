#pragma once

// The Linux runtime guard against process creation (S17, D51; INV-110, INV-112 runtime half). Installed at
// mode entry — --service here, the mutating CLI commands and the pkexec helper in WS-9 — before any Applier
// or engine object exists and before Qt starts a thread:
//   PR_SET_NO_NEW_PRIVS (always, root included: uid 0 is not CAP_SYS_ADMIN), then a seccomp-BPF
//   filter installed with SECCOMP_FILTER_FLAG_TSYNC (every existing thread):
//     1. seccomp_data.arch must be this build's ABI (AUDIT_ARCH_X86_64 / AUDIT_ARCH_AARCH64); anything else
//        -> SECCOMP_RET_KILL_PROCESS (closes the i386 `int 0x80` entry on x86-64);
//     2. x86-64 only: any number with __X32_SYSCALL_BIT -> KILL_PROCESS, BEFORE the denylist (x32 shares
//        AUDIT_ARCH_X86_64);
//     3. fork, vfork (x86-64 only; AArch64 has neither), execve, execveat -> EPERM; clone -> EPERM unless its
//        flags contain CLONE_THREAD; clone3 -> ENOSYS (glibc then creates threads with clone).
//   Any non-zero seccomp() result — including the thread id TSYNC returns when a thread cannot be synchronised
//   — is a failure, and the caller exits 5 before anything else happens.
//
// The seccomp() call is the one raw system call in the product (glibc has no wrapper, and prctl cannot pass
// TSYNC); it lives only in ProcessGuard.cpp, which the WS-9 `syscall(` / `__NR_` audit must allow-list.

#include <QString>

namespace asb::posix::ProcessGuard {

inline constexpr int kExitGuardFailed = 5;

// The seccomp(2) entry point, injectable for the tsync/failure tests: (operation, flags, args) -> result.
using SeccompCall = long (*)(unsigned int op, unsigned int flags, void* args);
long realSeccomp(unsigned int op, unsigned int flags, void* args);

// Installs the guard. False (with *why) on any failure, including a positive TSYNC result.
bool install(QString* why, SeccompCall call = &realSeccomp);

// Mode entry: install(), and on failure write one line to stderr and _exit(5) (no other I/O, no objects).
void installOrExit(SeccompCall call = &realSeccomp);

}  // namespace asb::posix::ProcessGuard
