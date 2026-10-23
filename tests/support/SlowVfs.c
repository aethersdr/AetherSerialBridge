/* A wrapper SQLite VFS that can stall xRead: see SlowVfs.h. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "SlowVfs.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

#include <sqlite3.h>

typedef struct SlowFile {
    sqlite3_file base; /* must be first */
    int is_main_db;
    sqlite3_file* real; /* the default VFS's file, allocated right after this struct */
} SlowFile;

static sqlite3_vfs* g_real_vfs = 0;
static sqlite3_vfs g_slow_vfs;
static sqlite3_io_methods g_slow_io;
static int g_delay_ms = 0;
static int g_stalls_left = 0;
static int g_stalls_done = 0;
static int g_gated = 0;     /* the armed stalls wait for the gate instead of a delay */
static int g_gate_open = 1; /* guarded by the SQLite app mutex */
static char g_last_pathname[4096];
static int g_have_pathname = 0;
static int g_record_only = 0;

/* Copies at most n-1 bytes of src into dst and NUL-terminates (strncpy is deprecated under MSVC). */
static void copy_name(char* dst, size_t n, const char* src) {
    size_t i = 0;
    while (i + 1 < n && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static sqlite3_mutex* slow_mutex(void) { return sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP1); }

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

static sqlite3_file* real_of(sqlite3_file* f) { return ((SlowFile*)f)->real; }

static int slow_close(sqlite3_file* f) {
    sqlite3_file* r = real_of(f);
    return r->pMethods ? r->pMethods->xClose(r) : SQLITE_OK;
}

static int slow_read(sqlite3_file* f, void* buf, int amt, sqlite3_int64 off) {
    int delay = 0;
    int gated = 0;
    if (((SlowFile*)f)->is_main_db) {
        sqlite3_mutex_enter(slow_mutex());
        if (g_stalls_left > 0 && (g_gated || g_delay_ms > 0)) {
            --g_stalls_left;
            ++g_stalls_done;
            gated = g_gated;
            delay = g_delay_ms;
        }
        sqlite3_mutex_leave(slow_mutex());
    }
    if (gated) {
        int waited_ms = 0;
        for (;;) {
            int open;
            sqlite3_mutex_enter(slow_mutex());
            open = g_gate_open;
            sqlite3_mutex_leave(slow_mutex());
            if (open || waited_ms >= 30000) {
                break;
            }
            sleep_ms(2);
            waited_ms += 2;
        }
    } else if (delay > 0) {
        sleep_ms(delay);
    }
    return real_of(f)->pMethods->xRead(real_of(f), buf, amt, off);
}

static int slow_write(sqlite3_file* f, const void* buf, int amt, sqlite3_int64 off) {
    return real_of(f)->pMethods->xWrite(real_of(f), buf, amt, off);
}
static int slow_truncate(sqlite3_file* f, sqlite3_int64 size) {
    return real_of(f)->pMethods->xTruncate(real_of(f), size);
}
static int slow_sync(sqlite3_file* f, int flags) { return real_of(f)->pMethods->xSync(real_of(f), flags); }
static int slow_file_size(sqlite3_file* f, sqlite3_int64* size) {
    return real_of(f)->pMethods->xFileSize(real_of(f), size);
}
static int slow_lock(sqlite3_file* f, int lock) { return real_of(f)->pMethods->xLock(real_of(f), lock); }
static int slow_unlock(sqlite3_file* f, int lock) { return real_of(f)->pMethods->xUnlock(real_of(f), lock); }
static int slow_check_reserved(sqlite3_file* f, int* out) {
    return real_of(f)->pMethods->xCheckReservedLock(real_of(f), out);
}
static int slow_file_control(sqlite3_file* f, int op, void* arg) {
    return real_of(f)->pMethods->xFileControl(real_of(f), op, arg);
}
static int slow_sector_size(sqlite3_file* f) { return real_of(f)->pMethods->xSectorSize(real_of(f)); }
static int slow_device_characteristics(sqlite3_file* f) {
    return real_of(f)->pMethods->xDeviceCharacteristics(real_of(f));
}
static int slow_shm_map(sqlite3_file* f, int pg, int pgsz, int extend, void volatile** pp) {
    return real_of(f)->pMethods->xShmMap(real_of(f), pg, pgsz, extend, pp);
}
static int slow_shm_lock(sqlite3_file* f, int offset, int n, int flags) {
    return real_of(f)->pMethods->xShmLock(real_of(f), offset, n, flags);
}
static void slow_shm_barrier(sqlite3_file* f) { real_of(f)->pMethods->xShmBarrier(real_of(f)); }
static int slow_shm_unmap(sqlite3_file* f, int del) { return real_of(f)->pMethods->xShmUnmap(real_of(f), del); }
static int slow_fetch(sqlite3_file* f, sqlite3_int64 off, int amt, void** pp) {
    return real_of(f)->pMethods->xFetch(real_of(f), off, amt, pp);
}
static int slow_unfetch(sqlite3_file* f, sqlite3_int64 off, void* p) {
    return real_of(f)->pMethods->xUnfetch(real_of(f), off, p);
}

static int slow_open(sqlite3_vfs* vfs, const char* name, sqlite3_file* f, int flags, int* out_flags) {
    SlowFile* sf = (SlowFile*)f;
    int rc;
    (void)vfs;
    memset(sf, 0, sizeof(*sf));
    if (g_record_only) {
        return SQLITE_CANTOPEN;
    }
    sf->real = (sqlite3_file*)(sf + 1);
    sf->is_main_db = (flags & SQLITE_OPEN_MAIN_DB) != 0;
    rc = g_real_vfs->xOpen(g_real_vfs, name, sf->real, flags, out_flags);
    /* Our methods only once the real file is open: SQLite calls xClose iff pMethods is set. */
    sf->base.pMethods = sf->real->pMethods ? &g_slow_io : 0;
    return rc;
}

static int slow_delete(sqlite3_vfs* vfs, const char* name, int sync_dir) {
    (void)vfs;
    return g_real_vfs->xDelete(g_real_vfs, name, sync_dir);
}
static int slow_access(sqlite3_vfs* vfs, const char* name, int flags, int* out) {
    (void)vfs;
    return g_real_vfs->xAccess(g_real_vfs, name, flags, out);
}
static int slow_full_pathname(sqlite3_vfs* vfs, const char* name, int n, char* out) {
    (void)vfs;
    sqlite3_mutex_enter(slow_mutex());
    copy_name(g_last_pathname, sizeof(g_last_pathname), name);
    g_have_pathname = 1;
    sqlite3_mutex_leave(slow_mutex());
    return g_real_vfs->xFullPathname(g_real_vfs, name, n, out);
}
static void* slow_dlopen(sqlite3_vfs* vfs, const char* name) {
    (void)vfs;
    return g_real_vfs->xDlOpen(g_real_vfs, name);
}
static void slow_dlerror(sqlite3_vfs* vfs, int n, char* out) {
    (void)vfs;
    g_real_vfs->xDlError(g_real_vfs, n, out);
}
static void (*slow_dlsym(sqlite3_vfs* vfs, void* h, const char* sym))(void) {
    (void)vfs;
    return g_real_vfs->xDlSym(g_real_vfs, h, sym);
}
static void slow_dlclose(sqlite3_vfs* vfs, void* h) {
    (void)vfs;
    g_real_vfs->xDlClose(g_real_vfs, h);
}
static int slow_randomness(sqlite3_vfs* vfs, int n, char* out) {
    (void)vfs;
    return g_real_vfs->xRandomness(g_real_vfs, n, out);
}
static int slow_sleep(sqlite3_vfs* vfs, int us) {
    (void)vfs;
    return g_real_vfs->xSleep(g_real_vfs, us);
}
static int slow_current_time(sqlite3_vfs* vfs, double* out) {
    (void)vfs;
    return g_real_vfs->xCurrentTime(g_real_vfs, out);
}
static int slow_get_last_error(sqlite3_vfs* vfs, int n, char* out) {
    (void)vfs;
    return g_real_vfs->xGetLastError ? g_real_vfs->xGetLastError(g_real_vfs, n, out) : 0;
}
static int slow_current_time_int64(sqlite3_vfs* vfs, sqlite3_int64* out) {
    (void)vfs;
    return g_real_vfs->xCurrentTimeInt64(g_real_vfs, out);
}

int asb_slowvfs_register(void) {
    if (g_real_vfs) {
        return SQLITE_OK;
    }
    g_real_vfs = sqlite3_vfs_find(0);
    if (!g_real_vfs) {
        return SQLITE_ERROR;
    }
    memset(&g_slow_io, 0, sizeof(g_slow_io));
    g_slow_io.iVersion = 3;
    g_slow_io.xClose = slow_close;
    g_slow_io.xRead = slow_read;
    g_slow_io.xWrite = slow_write;
    g_slow_io.xTruncate = slow_truncate;
    g_slow_io.xSync = slow_sync;
    g_slow_io.xFileSize = slow_file_size;
    g_slow_io.xLock = slow_lock;
    g_slow_io.xUnlock = slow_unlock;
    g_slow_io.xCheckReservedLock = slow_check_reserved;
    g_slow_io.xFileControl = slow_file_control;
    g_slow_io.xSectorSize = slow_sector_size;
    g_slow_io.xDeviceCharacteristics = slow_device_characteristics;
    g_slow_io.xShmMap = slow_shm_map;
    g_slow_io.xShmLock = slow_shm_lock;
    g_slow_io.xShmBarrier = slow_shm_barrier;
    g_slow_io.xShmUnmap = slow_shm_unmap;
    g_slow_io.xFetch = slow_fetch;
    g_slow_io.xUnfetch = slow_unfetch;

    memset(&g_slow_vfs, 0, sizeof(g_slow_vfs));
    g_slow_vfs.iVersion = 2;
    g_slow_vfs.szOsFile = (int)sizeof(SlowFile) + g_real_vfs->szOsFile;
    g_slow_vfs.mxPathname = g_real_vfs->mxPathname;
    g_slow_vfs.zName = ASB_SLOW_VFS_NAME;
    g_slow_vfs.xOpen = slow_open;
    g_slow_vfs.xDelete = slow_delete;
    g_slow_vfs.xAccess = slow_access;
    g_slow_vfs.xFullPathname = slow_full_pathname;
    g_slow_vfs.xDlOpen = slow_dlopen;
    g_slow_vfs.xDlError = slow_dlerror;
    g_slow_vfs.xDlSym = slow_dlsym;
    g_slow_vfs.xDlClose = slow_dlclose;
    g_slow_vfs.xRandomness = slow_randomness;
    g_slow_vfs.xSleep = slow_sleep;
    g_slow_vfs.xCurrentTime = slow_current_time;
    g_slow_vfs.xGetLastError = slow_get_last_error;
    g_slow_vfs.xCurrentTimeInt64 = slow_current_time_int64;
    return sqlite3_vfs_register(&g_slow_vfs, 0);
}

void asb_slowvfs_arm(int delay_ms, int stalls) {
    sqlite3_mutex_enter(slow_mutex());
    g_delay_ms = delay_ms;
    g_stalls_left = stalls;
    g_gated = 0;
    g_gate_open = 1; /* a gated stall still in progress is released */
    sqlite3_mutex_leave(slow_mutex());
}

void asb_slowvfs_arm_gated(int stalls) {
    sqlite3_mutex_enter(slow_mutex());
    g_delay_ms = 0;
    g_stalls_left = stalls;
    g_gated = 1;
    g_gate_open = 0;
    sqlite3_mutex_leave(slow_mutex());
}

void asb_slowvfs_release(void) {
    sqlite3_mutex_enter(slow_mutex());
    g_gate_open = 1;
    sqlite3_mutex_leave(slow_mutex());
}

int asb_slowvfs_stalls_done(void) {
    int n;
    sqlite3_mutex_enter(slow_mutex());
    n = g_stalls_done;
    sqlite3_mutex_leave(slow_mutex());
    return n;
}

void asb_slowvfs_set_record_only(int on) {
    sqlite3_mutex_enter(slow_mutex());
    g_record_only = on;
    sqlite3_mutex_leave(slow_mutex());
}

void asb_slowvfs_clear_pathname(void) {
    sqlite3_mutex_enter(slow_mutex());
    g_have_pathname = 0;
    g_last_pathname[0] = '\0';
    sqlite3_mutex_leave(slow_mutex());
}

int asb_slowvfs_last_pathname(char* out, int n) {
    int have;
    if (n <= 0) {
        return 0;
    }
    sqlite3_mutex_enter(slow_mutex());
    have = g_have_pathname;
    copy_name(out, (size_t)n, g_last_pathname);
    sqlite3_mutex_leave(slow_mutex());
    return have;
}
