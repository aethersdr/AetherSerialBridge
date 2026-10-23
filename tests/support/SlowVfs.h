#pragma once

/* A wrapper SQLite VFS that can stall xRead (tst_ThemeFollower, INV-53, INV-95).
 *
 * Registered under the name "asb-slow" (never as the default). Every call is passed through to the
 * default VFS; while armed, the next `stalls` xRead calls on a main database file sleep `delay_ms`
 * first. The follower is pointed at it through its open seam, so nothing else in the process is
 * slowed. */

#ifdef __cplusplus
extern "C" {
#endif

#define ASB_SLOW_VFS_NAME "asb-slow"

/* Registers the VFS once; returns an SQLite result code. */
int asb_slowvfs_register(void);
/* Arms the next `stalls` main-database xRead calls to sleep `delay_ms` each (0 disarms). */
void asb_slowvfs_arm(int delay_ms, int stalls);
/* Arms the next `stalls` main-database xRead calls to block until asb_slowvfs_release() (or
 * asb_slowvfs_arm(), which releases too), with a 30 s safety cap so a test that fails before it
 * releases cannot hang the process. A gated stall has no duration to race: a test that must show
 * "the UI did not wait for the read" holds the read for exactly as long as it needs. */
void asb_slowvfs_arm_gated(int stalls);
/* Opens the gate: every gated stall in progress, and any started later, returns at once. */
void asb_slowvfs_release(void);
/* Stalls entered since registration (counted when the read starts to wait, not when it ends). */
int asb_slowvfs_stalls_done(void);
/* While on, xOpen on this VFS fails with SQLITE_CANTOPEN without touching the filesystem or the
 * network: a test that only wants the recorded pathname (a UNC host would otherwise be resolved). */
void asb_slowvfs_set_record_only(int on);
/* Forgets the recorded pathname. */
void asb_slowvfs_clear_pathname(void);
/* Copies the name SQLite last passed to this VFS's xFullPathname into `out` (NUL-terminated,
 * truncated to `n`); returns 1 if there was one, else 0. That name is what SQLite's URI parser
 * produced for the database file, before any VFS normalisation. */
int asb_slowvfs_last_pathname(char* out, int n);

#ifdef __cplusplus
}
#endif
