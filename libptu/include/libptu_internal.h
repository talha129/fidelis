/*
 * libptu_internal.h — shared types, macros, and extern declarations
 * for the libptu LD_PRELOAD audit/replay library.
 *
 * Three modes (set via LIBPTU_MODE env var):
 *   log     — record file accesses to cde.manifest, no file copying
 *   capture — log + copy accessed files into cde-root (full materialization)
 *   replay  — redirect paths into cde-root, fall through to host if missing
 */

#ifndef LIBPTU_INTERNAL_H
#define LIBPTU_INTERNAL_H

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sendfile.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <libgen.h>

/* ── Session — shared across fork/exec chain via shared memory ──────── */
/*
 * All processes in a vine_worker session (vine_worker + all forked tasks)
 * share one libptu_session_t in a sysV shared memory segment.
 *
 * seen_hashes[]: cross-process hash-only dedup.  Slot stores djb2 hash of
 * the path; a match means "probably already recorded" — false-positive rate
 * is ~entries/SHMEM_DEDUP_SLOTS which is negligible for identical tasks.
 *
 * The per-process g_dedup[] (string-based) remains for within-process accuracy.
 * The shmem table is a fast cross-process pre-filter.
 */
#define LIBPTU_SESSION_MAGIC  0x4C505455U   /* "LPTU" */
#define SHMEM_DEDUP_SLOTS     1048576       /* 1M slots, 4 MB — collision rate <0.05% */

typedef struct {
    uint32_t                magic;
    _Atomic uint32_t        seen_hashes[SHMEM_DEDUP_SLOTS];
    char                    manifest_path[PATH_MAX];
} libptu_session_t;

/* ── Modes ──────────────────────────────────────────────────────────── */
typedef enum {
    MODE_DISABLED = 0,
    MODE_LOG,
    MODE_CAPTURE,
    MODE_REPLAY,
} libptu_mode_t;

/* ── Globals (defined in libptu.c) ─────────────────────────────────── */
extern libptu_mode_t      g_mode;
extern char               g_output_dir[PATH_MAX];
extern char               g_cde_root[PATH_MAX];
extern int                g_cde_root_len;

/* Manifest file + locking */
extern FILE              *g_manifest_fp;
extern int                g_manifest_fd;      /* raw fd for flock() cross-process */
extern pthread_mutex_t    g_manifest_mutex;

/* Shared memory session (cross-process dedup + manifest path) */
extern libptu_session_t  *g_session;
extern int                g_shmid;            /* -1 if not creator */

/* Dedup hash table */
#define DEDUP_SLOTS 16384
extern char *g_dedup[DEDUP_SLOTS];

/* fd → path table */
#define MAX_FD 65536
extern char *g_fd_table[MAX_FD];
extern pthread_mutex_t g_fd_mutex;

/* CWD (maintained by chdir/fchdir hooks; init from getcwd in constructor) */
extern char            g_cwd[PATH_MAX];
extern pthread_rwlock_t g_cwd_rwlock;

/* ── Per-thread recursion guard ─────────────────────────────────────── */
/*
 * Any hook that fires while we're already inside a hook (e.g., from our own
 * internal fopen / lstat calls) must skip auditing to avoid infinite recursion.
 * Using __thread gives zero-cost access after TLS init.
 */
extern __thread int tl_in_hook;

/*
 * libptu_lazy_init — ensure real_* function pointers are loaded.
 *
 * May be called before the constructor runs (e.g., when hooks fire during
 * early dynamic linking before __attribute__((constructor)) executes).
 * After the normal constructor runs this is a no-op (g_real_initialized=1).
 */
void libptu_lazy_init(void);

/*
 * HOOK_PREAMBLE — place at the top of every hook.
 *
 * Fast path (common case, no branch misprediction):
 *   tl_in_hook=0 and g_mode != DISABLED  →  set tl_in_hook=1, proceed.
 *
 * Slow path (rare: re-entry OR mode disabled/uninitialized):
 *   Call libptu_lazy_init() to ensure real_* pointers are valid, then
 *   fall through to the real function (fallback_expr).
 *
 * `fallback_expr` must be an expression that calls the real function and
 * produces the correct return value, e.g. real_open(path, flags, mode).
 */
#define HOOK_PREAMBLE(fallback_expr)                                    \
    do {                                                                \
        if (__builtin_expect(                                           \
                tl_in_hook | (g_mode == MODE_DISABLED), 0)) {          \
            libptu_lazy_init();                                         \
            return (fallback_expr);                                     \
        }                                                               \
        tl_in_hook = 1;                                                 \
    } while (0)

#define HOOK_RETURN(val)                                                \
    do { tl_in_hook = 0; return (val); } while (0)

/* ── Real function pointers (defined in libptu.c) ──────────────────── */
extern int   (*real_open)      (const char *, int, ...);
extern int   (*real_openat)    (int, const char *, int, ...);
extern int   (*real_creat)     (const char *, mode_t);
extern int   (*real_close)     (int);
extern int   (*real_stat)      (const char *, struct stat *);
extern int   (*real_lstat)     (const char *, struct stat *);
extern int   (*real_fstat)     (int, struct stat *);
extern int   (*real_fstatat)   (int, const char *, struct stat *, int);
extern int   (*real_access)    (const char *, int);
extern int   (*real_faccessat) (int, const char *, int, int);
extern ssize_t (*real_readlink)  (const char *, char *, size_t);
extern ssize_t (*real_readlinkat)(int, const char *, char *, size_t);
extern int   (*real_symlink)   (const char *, const char *);
extern int   (*real_symlinkat) (const char *, int, const char *);
extern int   (*real_link)      (const char *, const char *);
extern int   (*real_linkat)    (int, const char *, int, const char *, int);
extern int   (*real_unlink)    (const char *);
extern int   (*real_unlinkat)  (int, const char *, int);
extern int   (*real_rename)    (const char *, const char *);
extern int   (*real_renameat)  (int, const char *, int, const char *);
extern int   (*real_mkdir)     (const char *, mode_t);
extern int   (*real_mkdirat)   (int, const char *, mode_t);
extern int   (*real_rmdir)     (const char *);
extern int   (*real_chmod)     (const char *, mode_t);
extern int   (*real_fchmod)    (int, mode_t);
extern int   (*real_chmodat)   (int, const char *, mode_t, int);
extern int   (*real_chown)     (const char *, uid_t, gid_t);
extern int   (*real_lchown)    (const char *, uid_t, gid_t);
extern int   (*real_fchownat)  (int, const char *, uid_t, gid_t, int);
extern int   (*real_chdir)     (const char *);
extern int   (*real_fchdir)    (int);
extern int   (*real_dup)       (int);
extern int   (*real_dup2)      (int, int);
extern int   (*real_dup3)      (int, int, int);
extern int   (*real_fcntl)     (int, int, ...);
extern void *(*real_mmap)      (void *, size_t, int, int, int, off_t);
extern int   (*real_execve)    (const char *, char *const[], char *const[]);
extern int   (*real_execveat)  (int, const char *, char *const[], char *const[], int);
/* dlopen family — needed because Python's import uses dlopen internally and
 * ld-linux's private loader bypasses our open() hook for .so files */
extern void *(*real_dlopen)    (const char *, int);
extern int   (*real_dlclose)   (void *);
/* glibc <2.33 stat wrappers (non-LFS) */
extern int   (*real___xstat)     (int, const char *, struct stat *);
extern int   (*real___lxstat)    (int, const char *, struct stat *);
extern int   (*real___fxstat)    (int, int, struct stat *);
extern int   (*real___fxstatat)  (int, int, const char *, struct stat *, int);
/* glibc <2.33 stat wrappers (LFS/64-bit) — used by conda Python and other
 * binaries compiled with _FILE_OFFSET_BITS=64 against older glibc */
extern int   (*real___xstat64)   (int, const char *, struct stat64 *);
extern int   (*real___lxstat64)  (int, const char *, struct stat64 *);
extern int   (*real___fxstat64)  (int, int, struct stat64 *);
extern int   (*real___fxstatat64)(int, int, const char *, struct stat64 *, int);
/* statx — glibc 2.28+ / Linux 4.11+; NULL on older systems */
#include <sys/syscall.h>
#ifdef SYS_statx
extern int   (*real_statx)     (int, const char *, int, unsigned int, struct statx *);
#endif

/* ── Internal helpers (implemented in libptu.c) ─────────────────────── */

/* Resolve (dirfd, path) to absolute path in out_buf[PATH_MAX].
 * Returns out_buf on success, NULL on error. */
const char *resolve_at(int dirfd, const char *path, char *out_buf);

/* Record a file path into the manifest (log/capture modes). */
void handle_path(const char *abspath);

/* Redirect abspath into cde-root if present (replay mode).
 * Returns original path if not found in cde-root.
 * buf must be PATH_MAX bytes. */
const char *maybe_redirect(const char *abspath, char *buf);

/* fd_table helpers */
void fd_table_set(int fd, const char *path);
void fd_table_del(int fd);
void fd_table_dup(int oldfd, int newfd);

/* ── Public launcher API ─────────────────────────────────────────────
 * Called by libptu-launcher after dlopen("libptu.so").
 * libptu_scan_maps() re-runs the proc/self/maps scan to pick up any
 * files (locale, gconv) that were mmap'd since the constructor ran.
 * libptu_record() records an arbitrary path into the manifest.
 * ─────────────────────────────────────────────────────────────────── */
void libptu_scan_maps(void);
void libptu_record(const char *path);
void libptu_flush(void);   /* flush manifest buffer — call before execvp() */

#endif /* LIBPTU_INTERNAL_H */
