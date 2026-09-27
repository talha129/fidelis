/*
 * libptu.c — LD_PRELOAD audit/replay library
 *
 * *** THIS IS THE "libptu-sync" VARIANT ***
 * Forked from libptu/ to implement MODE_CAPTURE's synchronous file copying
 * (copy each file into cde-root the moment it is first touched, instead of
 * only recording a manifest for a separate async materialize pass). See the
 * "MODE_CAPTURE — synchronous file copying into cde-root" section below.
 * The original libptu/ is untouched — MODE_CAPTURE there is still a no-op
 * stub that behaves identically to MODE_LOG.
 *
 * Intercepts glibc file-access wrappers to:
 *   MODE_LOG     — record accessed paths to cde.manifest (near-zero overhead)
 *   MODE_CAPTURE — log + copy files into cde-root synchronously, as each is
 *                  first touched (full package creation, no async
 *                  materialize step needed afterward)
 *   MODE_REPLAY  — redirect paths into cde-root, fall through to host
 *
 * Overhead target: <1% on Python scientific workflows (TaskVine tasks) in
 * MODE_LOG. MODE_CAPTURE is inherently slower (it does real file I/O inline
 * on every first-touch) — that added, synchronous cost is the whole point:
 * it trades runtime overhead for not needing a separate materialize phase.
 *
 * Key optimizations:
 *   1. __thread tl_in_hook: zero-overhead recursion guard
 *   2. __thread tl_last[]: per-thread last-seen cache, no lock for 94% of calls
 *   3. Lockless primary-slot read in global dedup table (atomic relaxed load)
 *   4. 256 KB stdio buffer on manifest file (one fwrite per entry)
 *   5. __builtin_expect on all fast-path branches
 *   6. resolve_at uses rwlock (readers concurrent, chdir writer rare)
 *
 * Environment variables:
 *   LIBPTU_MODE    = log | capture | replay
 *   LIBPTU_OUTPUT  = /path/to/output-pkg        (log / capture)
 *   LIBPTU_CDE_ROOT = /path/to/pkg/cde-root     (replay; or use LIBPTU_OUTPUT/cde-root)
 */

#include "../include/libptu_internal.h"
#include <time.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <dirent.h>
#include <elf.h>
#include <utime.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Global state definitions
 * ───────────────────────────────────────────────────────────────────────── */

libptu_mode_t  g_mode          = MODE_DISABLED;
char           g_output_dir[PATH_MAX] = "";
char           g_cde_root[PATH_MAX]   = "";
int            g_cde_root_len  = 0;

FILE          *g_manifest_fp   = NULL;
int            g_manifest_fd   = -1;
static char    g_manifest_path[PATH_MAX] = "";
pthread_mutex_t g_manifest_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Shared memory session */
libptu_session_t *g_session = NULL;
int               g_shmid   = -1;

char          *g_dedup[DEDUP_SLOTS]; /* zero-initialized by BSS */

char          *g_fd_table[MAX_FD];   /* zero-initialized by BSS */
pthread_mutex_t g_fd_mutex = PTHREAD_MUTEX_INITIALIZER;

char           g_cwd[PATH_MAX]  = "";
pthread_rwlock_t g_cwd_rwlock   = PTHREAD_RWLOCK_INITIALIZER;

/* Per-thread recursion guard */
__thread int tl_in_hook = 0;

/* Per-thread last-seen path cache (file-scope for dlopen TLS portability) */
__thread char tl_last[PATH_MAX];

/* ── Skip-pattern table ─────────────────────────────────────────────────── */
/* Paths matching any pattern are silently ignored at capture time.
 * Defaults cover TaskVine worker scratch dirs and run-info logs.
 * Additional patterns loaded from LIBPTU_SKIP_FILE (one glob per line). */
#define MAX_SKIP_PATTERNS 128
static char *g_skip_patterns[MAX_SKIP_PATTERNS];
static int   g_skip_count = 0;

/* ─────────────────────────────────────────────────────────────────────────
 * Real function pointers
 * ───────────────────────────────────────────────────────────────────────── */

int    (*real_open)      (const char *, int, ...)          = NULL;
int    (*real_openat)    (int, const char *, int, ...)     = NULL;
int    (*real_creat)     (const char *, mode_t)            = NULL;
int    (*real_close)     (int)                             = NULL;
int    (*real_stat)      (const char *, struct stat *)     = NULL;
int    (*real_lstat)     (const char *, struct stat *)     = NULL;
int    (*real_fstat)     (int, struct stat *)              = NULL;
int    (*real_fstatat)   (int, const char *, struct stat *, int) = NULL;
int    (*real_access)    (const char *, int)               = NULL;
int    (*real_faccessat) (int, const char *, int, int)     = NULL;
ssize_t (*real_readlink) (const char *, char *, size_t)    = NULL;
ssize_t (*real_readlinkat)(int, const char *, char *, size_t) = NULL;
int    (*real_symlink)   (const char *, const char *)      = NULL;
int    (*real_symlinkat) (const char *, int, const char *) = NULL;
int    (*real_link)      (const char *, const char *)      = NULL;
int    (*real_linkat)    (int, const char *, int, const char *, int) = NULL;
int    (*real_unlink)    (const char *)                    = NULL;
int    (*real_unlinkat)  (int, const char *, int)          = NULL;
int    (*real_rename)    (const char *, const char *)      = NULL;
int    (*real_renameat)  (int, const char *, int, const char *) = NULL;
int    (*real_mkdir)     (const char *, mode_t)            = NULL;
int    (*real_mkdirat)   (int, const char *, mode_t)       = NULL;
int    (*real_rmdir)     (const char *)                    = NULL;
int    (*real_chmod)     (const char *, mode_t)            = NULL;
int    (*real_fchmod)    (int, mode_t)                     = NULL;
int    (*real_chmodat)   (int, const char *, mode_t, int)  = NULL;
int    (*real_chown)     (const char *, uid_t, gid_t)      = NULL;
int    (*real_lchown)    (const char *, uid_t, gid_t)      = NULL;
int    (*real_fchownat)  (int, const char *, uid_t, gid_t, int) = NULL;
int    (*real_chdir)     (const char *)                    = NULL;
int    (*real_fchdir)    (int)                             = NULL;
int    (*real_dup)       (int)                             = NULL;
int    (*real_dup2)      (int, int)                        = NULL;
int    (*real_dup3)      (int, int, int)                   = NULL;
int    (*real_fcntl)     (int, int, ...)                   = NULL;
void  *(*real_mmap)      (void *, size_t, int, int, int, off_t) = NULL;
int    (*real_execve)    (const char *, char *const[], char *const[]) = NULL;
int    (*real_execveat)  (int, const char *, char *const[], char *const[], int) = NULL;
void  *(*real_dlopen)    (const char *, int)                = NULL;
int    (*real_dlclose)   (void *)                           = NULL;
pid_t  (*real_fork)      (void)                             = NULL;
int    (*real___xstat)     (int, const char *, struct stat *)            = NULL;
int    (*real___lxstat)    (int, const char *, struct stat *)            = NULL;
int    (*real___fxstat)    (int, int, struct stat *)                     = NULL;
int    (*real___fxstatat)  (int, int, const char *, struct stat *, int)  = NULL;
/* 64-bit LFS wrappers — conda Python 3.9 and other LFS-compiled binaries
 * call __xstat64 / __lxstat64 / __fxstat64 / __fxstatat64 instead of the
 * non-64 variants above. On x86_64 struct stat64 == struct stat (same layout),
 * so we use the same interception logic. */
int    (*real___xstat64)   (int, const char *, struct stat64 *)          = NULL;
int    (*real___lxstat64)  (int, const char *, struct stat64 *)          = NULL;
int    (*real___fxstat64)  (int, int, struct stat64 *)                   = NULL;
int    (*real___fxstatat64)(int, int, const char *, struct stat64 *, int)= NULL;
#ifdef SYS_statx
int    (*real_statx)     (int, const char *, int, unsigned int, struct statx *) = NULL;
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * Helper: djb2 hash
 * ───────────────────────────────────────────────────────────────────────── */
static __attribute__((pure)) uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

/* ─────────────────────────────────────────────────────────────────────────
 * fd_table helpers
 * ───────────────────────────────────────────────────────────────────────── */
void fd_table_set(int fd, const char *path)
{
    if ((unsigned)fd >= MAX_FD || !path) return;
    char *dup = strdup(path);
    pthread_mutex_lock(&g_fd_mutex);
    free(g_fd_table[fd]);
    g_fd_table[fd] = dup;
    pthread_mutex_unlock(&g_fd_mutex);
}

void fd_table_del(int fd)
{
    if ((unsigned)fd >= MAX_FD) return;
    pthread_mutex_lock(&g_fd_mutex);
    free(g_fd_table[fd]);
    g_fd_table[fd] = NULL;
    pthread_mutex_unlock(&g_fd_mutex);
}

void fd_table_dup(int oldfd, int newfd)
{
    if ((unsigned)oldfd >= MAX_FD || (unsigned)newfd >= MAX_FD) return;
    pthread_mutex_lock(&g_fd_mutex);
    free(g_fd_table[newfd]);
    g_fd_table[newfd] = g_fd_table[oldfd] ? strdup(g_fd_table[oldfd]) : NULL;
    pthread_mutex_unlock(&g_fd_mutex);
}

/* Atomic read — safe on LP64 for pointer-sized reads */
static inline char *fd_get(int fd)
{
    if ((unsigned)fd >= MAX_FD) return NULL;
    return __atomic_load_n(&g_fd_table[fd], __ATOMIC_RELAXED);
}

/* ─────────────────────────────────────────────────────────────────────────
 * resolve_at — (dirfd, relpath) → absolute path
 * ───────────────────────────────────────────────────────────────────────── */
const char *resolve_at(int dirfd, const char *path, char *out_buf)
{
    if (!path) return NULL;

    /* Already absolute — fast path (most common for Python imports) */
    if (path[0] == '/') {
        /* Avoid unnecessary copy when caller can use path directly */
        strncpy(out_buf, path, PATH_MAX - 1);
        out_buf[PATH_MAX - 1] = '\0';
        return out_buf;
    }

    const char *base;
    char proc_target[PATH_MAX];

    if (dirfd == AT_FDCWD || dirfd < 0) {
        /* Relative to CWD */
        pthread_rwlock_rdlock(&g_cwd_rwlock);
        snprintf(out_buf, PATH_MAX, "%s/%s", g_cwd, path);
        pthread_rwlock_unlock(&g_cwd_rwlock);
        return out_buf;
    }

    /* Relative to an open directory fd */
    base = fd_get(dirfd);
    if (!base) {
        /* fallback: /proc/self/fd/<dirfd> */
        char proc[32];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", dirfd);
        ssize_t len = readlink(proc, proc_target, PATH_MAX - 1);
        if (len <= 0) return NULL;
        proc_target[len] = '\0';
        base = proc_target;
    }
    snprintf(out_buf, PATH_MAX, "%s/%s", base, path);
    return out_buf;
}

/* Forward declaration — defined in skip-pattern section below */
static int is_skipped(const char *abspath);

/* ─────────────────────────────────────────────────────────────────────────
 * MODE_CAPTURE — synchronous file copying into cde-root
 *
 * Unlike MODE_LOG (manifest only, materialize later via libptu-materialize),
 * MODE_CAPTURE copies each file into cde-root the moment it is first
 * recorded — i.e. inline in the same once-per-unique-path slow path that
 * already writes the manifest entry, under g_manifest_mutex. No separate
 * async materialize pass is needed afterward.
 *
 * All helpers below are only ever called from within manifest_record_abs()
 * or record_ancestors_locked() while g_manifest_mutex is already held, so
 * none of them do their own locking.
 * ───────────────────────────────────────────────────────────────────────── */

/* Build the cde-root-relative destination path for an absolute source path.
 * SAFETY: if g_cde_root isn't configured yet (e.g. called too early — this
 * has happened once during development, see git history), produces an empty
 * string rather than a bare copy of abspath. Every capture_* mutator below
 * refuses to touch the filesystem when given an empty dst — this is a hard
 * guard against ever unlinking/overwriting a real host file at its own path,
 * which is what an empty g_cde_root prefix would otherwise cause. */
static void capture_dest_path(const char *abspath, char *out)
{
    if (g_cde_root_len == 0) { out[0] = '\0'; return; }
    snprintf(out, PATH_MAX, "%s%s", g_cde_root, abspath);
}

/* mkdir -p equivalent. Ignores EEXIST (mirrors libptu-materialize's mkdirs_p). */
static void capture_mkdirs_p(const char *path, mode_t mode)
{
    if (path[0] == '\0') return;
    char tmp[PATH_MAX];
    strncpy(tmp, path, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

/* Create the parent directory (under cde-root) of a cde-root-relative path. */
static void capture_mkdirs_for_parent(const char *path)
{
    if (path[0] == '\0') return;
    char tmp[PATH_MAX];
    strncpy(tmp, path, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';
    char *sl = strrchr(tmp, '/');
    if (sl && sl != tmp) {
        *sl = '\0';
        capture_mkdirs_p(tmp, 0755);
    }
}

/* Copy src -> dst via sendfile (kernel zero-copy), preserving mode + mtime.
 * Mirrors libptu-materialize's copy_file() so sync and async capture produce
 * identical cde-root contents. */
static void capture_copy_file(const char *src, const char *dst, mode_t mode)
{
    if (dst[0] == '\0' || strcmp(src, dst) == 0) return; /* see safety note above */
    capture_mkdirs_for_parent(dst);

    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return;

    struct stat st;
    if (fstat(sfd, &st) != 0) { close(sfd); return; }

    unlink(dst); /* in case a stale entry exists */
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dfd < 0) { close(sfd); return; }

    off_t off = 0;
    off_t rem = st.st_size;
    while (rem > 0) {
        ssize_t n = sendfile(dfd, sfd, &off, (size_t)rem);
        if (n <= 0) break;
        rem -= n;
    }
    close(sfd);
    close(dfd);
    chmod(dst, mode);

    struct utimbuf ut;
    ut.actime  = st.st_atime;
    ut.modtime = st.st_mtime;
    utime(dst, &ut);
}

/* Plain recursive rm -rf — no fork/exec. This code runs inside whatever
 * arbitrary target process libptu.so is preloaded into, already holding
 * g_manifest_mutex when called from capture_place_symlink; forking here
 * previously caused the calling process to hang (see git history) — the
 * exact mechanism wasn't pinned down, but a preloaded interposition library
 * forking out of an arbitrary host program while holding one of its own
 * locks is inherently fragile, so it's avoided entirely instead. Only ever
 * applied to directories libptu itself created (see capture_place_symlink),
 * so contents are simple and shallow. opendir/readdir/unlink/rmdir are not
 * libptu hooks (or are safe PASSTHROUGH), so none of this re-enters our own
 * interposition logic. */
static void capture_rm_rf(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { rmdir(dir); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) capture_rm_rf(child);
        else unlink(child);
    }
    closedir(d);
    rmdir(dir);
}

/* Place a symlink at dst (under cde-root) pointing at target.
 *
 * Ordering race: because captures happen live as paths are first touched (not
 * from a complete, pre-sorted manifest like libptu-materialize's offline
 * pass), a file nested under an OS-level symlink ancestor (e.g.
 * /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2, under the /lib -> usr/lib
 * symlink) can get its parent directories auto-created as REAL directories
 * before the /lib symlink entry itself is ever processed. Mirrors
 * libptu-materialize's place_symlink: if dst is already a non-symlink
 * directory, remove it (rmdir if empty, else rm -rf) before placing the
 * symlink. */
static void capture_place_symlink(const char *dst, const char *target)
{
    if (dst[0] == '\0') return; /* see safety note on capture_dest_path */
    capture_mkdirs_for_parent(dst);

    struct stat st;
    if (lstat(dst, &st) == 0) {
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            if (rmdir(dst) != 0) capture_rm_rf(dst);
        } else {
            unlink(dst);
        }
    }
    symlink(target, dst);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Ancestor directory recording
 *
 * Called from manifest_record_abs() while g_manifest_mutex is already held.
 * Walks each parent path component of abspath; for any component not yet in
 * the dedup table, stats it and writes a D entry (or L entry for OS-level
 * symlinks like /bin → usr/bin).
 *
 * Does NOT call manifest_record_abs() — that would deadlock on the mutex.
 * Performs its own dedup check + write inline.
 * ───────────────────────────────────────────────────────────────────────── */
static void record_ancestors_locked(const char *abspath)
{
    char buf[PATH_MAX];
    strncpy(buf, abspath, PATH_MAX - 1);
    buf[PATH_MAX - 1] = '\0';

    char *p = strrchr(buf, '/');
    while (p && p != buf) {
        *p = '\0';
        if (buf[0] == '\0') break;

        /* Dedup check — linear probe (mutex already held) */
        uint32_t slot = djb2(buf) & (DEDUP_SLOTS - 1);
        int found = 0, empty_slot = -1;
        for (int i = 0; i < DEDUP_SLOTS; i++) {
            uint32_t s = (slot + i) & (DEDUP_SLOTS - 1);
            if (!g_dedup[s]) {
                if (empty_slot < 0) empty_slot = (int)s;
                break;
            }
            if (strcmp(g_dedup[s], buf) == 0) { found = 1; break; }
        }

        if (!found) {
            /* Also check cross-process shmem dedup before lstat + write */
            uint32_t buf_hash = djb2(buf);
            if (g_session) {
                uint32_t sslt = buf_hash & (SHMEM_DEDUP_SLOTS - 1);
                uint32_t ex = atomic_load_explicit(
                    &g_session->seen_hashes[sslt], memory_order_relaxed);
                if (ex == buf_hash && ex != 0) {
                    p = strrchr(buf, '/');
                    continue; /* already recorded by another process */
                }
            }

            if (empty_slot >= 0) {
                struct stat st;
                if (syscall(SYS_newfstatat, AT_FDCWD, buf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                    g_dedup[empty_slot] = strdup(buf);
                    /* Update shmem dedup for this ancestor */
                    if (g_session) {
                        uint32_t sslt = buf_hash & (SHMEM_DEDUP_SLOTS - 1);
                        atomic_store_explicit(&g_session->seen_hashes[sslt],
                                              buf_hash, memory_order_relaxed);
                    }
                    if (g_manifest_fp && !is_skipped(buf)) {
                        if (S_ISDIR(st.st_mode)) {
                            fprintf(g_manifest_fp, "D %06o %s\n",
                                    (unsigned)(st.st_mode & 07777), buf);
                            if (g_mode == MODE_CAPTURE) {
                                char dst[PATH_MAX];
                                capture_dest_path(buf, dst);
                                capture_mkdirs_p(dst, (mode_t)(st.st_mode & 07777));
                            }
                        } else if (S_ISLNK(st.st_mode)) {
                            char target[PATH_MAX];
                            ssize_t n = real_readlink(buf, target, PATH_MAX - 1);
                            if (n > 0) {
                                target[n] = '\0';
                                fprintf(g_manifest_fp, "L %06o %s %s\n",
                                        (unsigned)(st.st_mode & 07777), buf, target);
                                if (g_mode == MODE_CAPTURE) {
                                    char dst[PATH_MAX];
                                    capture_dest_path(buf, dst);
                                    capture_place_symlink(dst, target);
                                }
                            }
                        }
                    }
                }
            }
        }

        p = strrchr(buf, '/');
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Skip-pattern loading and matching
 * ───────────────────────────────────────────────────────────────────────── */
#include <fnmatch.h>
#include <dlfcn.h>

static void _add_skip_pattern(const char *pat)
{
    if (g_skip_count >= MAX_SKIP_PATTERNS) return;
    g_skip_patterns[g_skip_count++] = strdup(pat);
}

/*
 * load_skip_patterns — read glob patterns from a config file.
 *
 * Search order:
 *   1. $LIBPTU_SKIP_FILE — explicit override
 *   2. <dir-of-libptu.so>/skip-patterns.conf — auto-discovered via dladdr
 *
 * File format: one fnmatch(3) glob per line; '#' lines and blank lines ignored.
 * Paths matched against the absolute path before recording to the manifest.
 */
static void load_skip_patterns(void)
{
    char path[PATH_MAX] = "";

    const char *env_path = getenv("LIBPTU_SKIP_FILE");
    if (env_path) {
        strncpy(path, env_path, PATH_MAX - 1);
    } else {
        /* Discover libptu.so's own directory via dladdr */
        Dl_info info;
        if (dladdr((void *)load_skip_patterns, &info) && info.dli_fname) {
            strncpy(path, info.dli_fname, PATH_MAX - 1);
            char *sl = strrchr(path, '/');
            if (sl) {
                snprintf(sl + 1, PATH_MAX - (sl + 1 - path), "skip-patterns.conf");
            } else {
                strncpy(path, "skip-patterns.conf", PATH_MAX - 1);
            }
        }
    }

    if (!path[0]) return;
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\n') line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        _add_skip_pattern(line);
    }
    fclose(f);
}

static int is_skipped(const char *abspath)
{
    /* No FNM_PATHNAME: '*' matches '/' so "/tmp/worker-*" catches all paths
     * under any worker scratch dir, not just the top-level name. */
    for (int i = 0; i < g_skip_count; i++) {
        if (fnmatch(g_skip_patterns[i], abspath, 0) == 0)
            return 1;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Manifest writing with two-level dedup
 * ───────────────────────────────────────────────────────────────────────── */

/*
 * manifest_record_abs — called with an ABSOLUTE path.
 *
 * Fast path (no lock, no lstat, no write) for 94% of calls:
 *   thread-local last-seen cache check → hit → return immediately.
 *
 * Slow path (lock + lstat + write) for new unique paths only:
 *   atomic read of primary hash slot → miss → acquire mutex →
 *   linear probe dedup → write → release.
 */
static __attribute__((hot)) void manifest_record_abs(const char *abspath)
{
    /* ── Level 1: thread-local last-seen (no lock, no hash) ── */
    if (__builtin_expect(tl_last[0] != '\0' &&
                         strcmp(abspath, tl_last) == 0, 1))
        return;
    strncpy(tl_last, abspath, PATH_MAX - 1);
    tl_last[PATH_MAX - 1] = '\0';

    /* ── MODE_CAPTURE self-reference guard ──
     * g_cde_root lives INSIDE g_output_dir (e.g.
     * /tmp/libptu-capture-<pid>/cde-root). If anything ever touches a path
     * under g_output_dir — including g_cde_root itself, or a file this
     * process already copied there — recording+copying it would mirror that
     * path back into cde-root AGAIN, one level deeper
     * (.../cde-root/tmp/libptu-capture-N/cde-root/tmp/libptu-capture-N/...),
     * repeating until PATH_MAX. Observed in practice during real MapReduce
     * runs — bounded (PATH_MAX truncates it) but leaves real, un-rm-able
     * leftover directories every time. This must live here rather than only
     * in handle_path(), since manifest_record_abs() is also called directly
     * (symlink-target recursion, scan_loaded_libs, the OS-symlink bootstrap
     * in libptu_init) bypassing handle_path entirely. Anything under our own
     * output dir is our own bookkeeping, never something a replay needs. */
    if (g_mode == MODE_CAPTURE && g_output_dir[0] != '\0') {
        size_t out_len = strlen(g_output_dir);
        if (strncmp(abspath, g_output_dir, out_len) == 0 &&
            (abspath[out_len] == '/' || abspath[out_len] == '\0'))
            return;
    }

    /* ── Skip-pattern filter ── */
    if (__builtin_expect(g_skip_count > 0 && is_skipped(abspath), 0))
        return;

    /* ── Level 2: lockless primary-slot check (per-process) ── */
    uint32_t slot = djb2(abspath) & (DEDUP_SLOTS - 1);
    char *primary = __atomic_load_n(&g_dedup[slot], __ATOMIC_RELAXED);
    if (__builtin_expect(primary != NULL &&
                         strcmp(primary, abspath) == 0, 0))
        return; /* already recorded (primary slot hit) */

    /* ── Level 2.5: cross-process shmem hash check ── */
    uint32_t path_hash = djb2(abspath);
    if (g_session) {
        uint32_t sslot = path_hash & (SHMEM_DEDUP_SLOTS - 1);
        uint32_t existing = atomic_load_explicit(
            &g_session->seen_hashes[sslot], memory_order_relaxed);
        if (existing == path_hash && existing != 0) return;
    }

    /* ── Level 3: lstat + full dedup under mutex ── */
    struct stat st;
    /* Use raw syscall — avoids rehooking AND works when real_lstat is NULL
     * (conda envs where dlsym(RTLD_NEXT,"lstat") returns NULL) */
    if (syscall(SYS_newfstatat, AT_FDCWD, abspath, &st, AT_SYMLINK_NOFOLLOW) != 0) return;

    pthread_mutex_lock(&g_manifest_mutex);

    /* Full linear-probe search */
    int empty_slot = -1;
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        uint32_t s = (slot + i) & (DEDUP_SLOTS - 1);
        if (!g_dedup[s]) {
            if (empty_slot < 0) empty_slot = s;
            break; /* first empty — key not present */
        }
        if (strcmp(g_dedup[s], abspath) == 0) {
            pthread_mutex_unlock(&g_manifest_mutex);
            return; /* already seen */
        }
    }

    if (empty_slot < 0) {
        /* Table full — log without dedup (rare) */
        pthread_mutex_unlock(&g_manifest_mutex);
        return;
    }

    /* Insert into per-process dedup table */
    g_dedup[empty_slot] = strdup(abspath);

    /* Update cross-process shmem dedup */
    if (g_session) {
        uint32_t sslot = path_hash & (SHMEM_DEDUP_SLOTS - 1);
        atomic_store_explicit(&g_session->seen_hashes[sslot],
                              path_hash, memory_order_relaxed);
    }

    /* Write manifest entry — flock for cross-process serialization */
    if (g_manifest_fd >= 0) flock(g_manifest_fd, LOCK_EX);
    char symlink_target_abs[PATH_MAX] = ""; /* non-empty if this is a symlink */
    if (g_manifest_fp) {
        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t len = real_readlink(abspath, target, PATH_MAX - 1);
            if (len > 0) {
                target[len] = '\0';
                fprintf(g_manifest_fp, "L %06o %s %s\n",
                        (unsigned)(st.st_mode & 07777), abspath, target);
                if (g_mode == MODE_CAPTURE) {
                    char dst[PATH_MAX];
                    capture_dest_path(abspath, dst);
                    capture_place_symlink(dst, target);
                }
                /* Resolve target to absolute so we can record the real file too.
                 * Relative targets are resolved relative to dir of the symlink. */
                if (target[0] == '/') {
                    strncpy(symlink_target_abs, target, PATH_MAX - 1);
                } else {
                    char dir[PATH_MAX];
                    strncpy(dir, abspath, PATH_MAX - 1);
                    char *sl = strrchr(dir, '/');
                    if (sl) { *sl = '\0'; snprintf(symlink_target_abs, PATH_MAX, "%s/%s", dir, target); }
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            fprintf(g_manifest_fp, "F %06o %s\n",
                    (unsigned)(st.st_mode & 07777), abspath);
            if (g_mode == MODE_CAPTURE) {
                char dst[PATH_MAX];
                capture_dest_path(abspath, dst);
                capture_copy_file(abspath, dst, (mode_t)(st.st_mode & 07777));
            }
        } else if (S_ISDIR(st.st_mode)) {
            fprintf(g_manifest_fp, "D %06o %s\n",
                    (unsigned)(st.st_mode & 07777), abspath);
            if (g_mode == MODE_CAPTURE) {
                char dst[PATH_MAX];
                capture_dest_path(abspath, dst);
                capture_mkdirs_p(dst, (mode_t)(st.st_mode & 07777));
            }
        }
    }

    /* Record all parent directory (and OS-symlink) components */
    record_ancestors_locked(abspath);

    if (g_manifest_fd >= 0) flock(g_manifest_fd, LOCK_UN);
    pthread_mutex_unlock(&g_manifest_mutex);

    /* Record symlink target after releasing mutex — avoids deadlock, and
     * handles chained symlinks (manifest_record_abs recurses if target is also
     * a symlink; dedup table prevents infinite loops). */
    if (symlink_target_abs[0])
        manifest_record_abs(symlink_target_abs);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Central path dispatch — called by all hooks
 * ───────────────────────────────────────────────────────────────────────── */
void __attribute__((hot)) handle_path(const char *abspath)
{
    if (!abspath || abspath[0] != '/') return;
    if (g_mode == MODE_LOG || g_mode == MODE_CAPTURE)
        manifest_record_abs(abspath);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Replay path redirect
 * ───────────────────────────────────────────────────────────────────────── */
const char *maybe_redirect(const char *abspath, char *buf)
{
    if (!abspath || abspath[0] != '/') return abspath;
    snprintf(buf, PATH_MAX, "%s%s", g_cde_root, abspath);
    struct stat st;
    if (syscall(SYS_newfstatat, AT_FDCWD, buf, &st, AT_SYMLINK_NOFOLLOW) == 0) return buf;
    return abspath; /* fallthrough to host */
}

/* ─────────────────────────────────────────────────────────────────────────
 * /proc/self/maps scan — capture all already-loaded shared libs
 * ───────────────────────────────────────────────────────────────────────── */
/*
 * phdr_callback / scan_loaded_libs:
 *
 * dl_iterate_phdr walks the in-memory link map (a doubly-linked list
 * maintained by ld-linux) — no procfs, no syscall, O(loaded libs).
 * Replaces the slower scan_proc_maps() which read /proc/self/maps and
 * caused the kernel to regenerate VMA lists on every dlopen hook call.
 */
#include <link.h>

/*
 * normalize_path — resolve '..' in the directory component via realpath(),
 * but leave the basename as-is so symlink names are preserved.
 *
 * dl_iterate_phdr returns RPATH-relative paths like "../../lib/libopenblas.so.0".
 * realpath() on the full path follows the final .so.0 symlink → wrong target.
 * Normalizing only the directory part gives the canonical absolute symlink path,
 * which manifest_record_abs() then records as an L entry + follows to the F entry.
 */
static const char *normalize_path(const char *raw, char *out)
{
    char dir_buf[PATH_MAX];
    strncpy(dir_buf, raw, PATH_MAX - 1);
    dir_buf[PATH_MAX - 1] = '\0';

    char *slash = strrchr(dir_buf, '/');
    if (!slash) { strncpy(out, raw, PATH_MAX - 1); out[PATH_MAX-1] = '\0'; return out; }

    char base[NAME_MAX + 1];
    strncpy(base, slash + 1, NAME_MAX);
    base[NAME_MAX] = '\0';
    *slash = '\0'; /* null-terminate dir part */

    char dir_resolved[PATH_MAX];
    if (!realpath(dir_buf, dir_resolved)) {
        strncpy(out, raw, PATH_MAX - 1); out[PATH_MAX-1] = '\0'; return out;
    }
    snprintf(out, PATH_MAX, "%s/%s", dir_resolved, base);
    return out;
}

static int phdr_callback(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size; (void)data;
    if (!info->dlpi_name || info->dlpi_name[0] != '/') return 0;

    /* libptu.so itself appears in the link map but is not needed for replay */
    if (strstr(info->dlpi_name, "libptu.so")) return 0;

    /* Normalize: resolve '..' in directory part without following the final
     * symlink, so libopenblas.so.0 stays as libopenblas.so.0 (not the target). */
    char resolved[PATH_MAX];
    manifest_record_abs(normalize_path(info->dlpi_name, resolved));
    return 0;
}

static void scan_loaded_libs(void)
{
    dl_iterate_phdr(phdr_callback, NULL);

    /* Also capture the main executable via /proc/self/exe */
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, PATH_MAX - 1);
    if (n > 0) { exe[n] = '\0'; manifest_record_abs(exe); }
}

/*
 * scan_proc_maps_init — called exactly once at constructor time.
 *
 * Reads /proc/self/maps to catch file-backed mappings created by the dynamic
 * linker or glibc during their own initialization, before our hooks fire.
 * Key targets: /etc/ld.so.cache (kept mmap'd by ld-linux) and
 * /usr/lib/locale/locale-archive (kept mmap'd by glibc locale code).
 *
 * At constructor time the process has few VMAs (Python+numpy not yet loaded),
 * so this single read is cheap (<1ms). NOT called on subsequent dlopen() calls
 * — those use dl_iterate_phdr() which needs no procfs.
 */
static void scan_proc_maps_init(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        /* Format: addr-addr perms offset dev ino [path] */
        char *path = strchr(line, '/');
        if (!path) continue;
        size_t l = strlen(path);
        if (l > 0 && path[l - 1] == '\n') path[l - 1] = '\0';
        /* .so files already captured by scan_loaded_libs() via dl_iterate_phdr */
        if (strstr(path, ".so")) continue;
        manifest_record_abs(path);
    }
    fclose(f);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Environment capture
 * ───────────────────────────────────────────────────────────────────────── */
static void capture_environment(void)
{
    extern char **environ;
    char env_path[PATH_MAX];
    snprintf(env_path, sizeof(env_path),
             "%s/cde.full-environment.cde-root", g_output_dir);
    FILE *f = fopen(env_path, "wb");
    if (!f) return;
    for (char **e = environ; *e; e++) {
        size_t len = strlen(*e);
        fwrite(*e, 1, len + 1, f); /* null-separated, PTU format */
    }
    fclose(f);
}

/* ─────────────────────────────────────────────────────────────────────────
 * On-exit build: materialize cde-root + mksquashfs
 *
 * Triggered when LIBPTU_BUILD_ON_EXIT=1 is set.
 * Only the session root process (g_shmid >= 0) builds.
 * Uses real_* function pointers directly to avoid hook re-entry.
 * ───────────────────────────────────────────────────────────────────────── */

static int _is_virtual(const char *path)
{
    static const char *vfs[] = {"/proc", "/sys", "/dev", NULL};
    for (int i = 0; vfs[i]; i++) {
        size_t l = strlen(vfs[i]);
        if (strcmp(path, vfs[i]) == 0 ||
            (strncmp(path, vfs[i], l) == 0 && path[l] == '/'))
            return 1;
    }
    return 0;
}

/* mkdir -p: create all intermediate components */
static void _mkdirs(const char *path)
{
    char tmp[PATH_MAX * 2];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            real_mkdir(tmp, 0755);
            *p = '/';
        }
    }
    real_mkdir(tmp, 0755);
}

/* Portable lstat: glibc < 2.33 exports __lxstat instead of lstat directly */
#ifndef _STAT_VER_LINUX
#define _STAT_VER_LINUX 1
#endif
static inline int _do_lstat(const char *path, struct stat *st)
{
    if (real_lstat)    return real_lstat(path, st);
    if (real___lxstat) return real___lxstat(_STAT_VER_LINUX, path, st);
    return -1;
}

/* Copy regular file src → dst using real open/read/write */
static void _copy_file(const char *src, const char *dst, mode_t mode)
{
    int sfd = real_open(src, O_RDONLY, 0);
    if (sfd < 0) return;
    int dfd = real_open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
    if (dfd < 0) { real_close(sfd); return; }
    char buf[65536];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0)
        if (write(dfd, buf, (size_t)n) < 0) break;
    real_close(sfd);
    real_close(dfd);
}

/* Read one '\n'-terminated line from fd into buf[bufsz].
 * carry[]/carry_len hold bytes buffered from the previous read() call.
 * Returns line length (>0) or -1 on EOF/error with nothing read. */
static int _read_line(int fd, char *buf, int bufsz,
                      char *carry, int *carry_len)
{
    int pos = 0;

    /* Drain carry: scan for newline or fill buf */
    for (int i = 0; i < *carry_len; i++) {
        buf[pos++] = carry[i];
        if (carry[i] == '\n' || pos == bufsz - 1) {
            int rem = *carry_len - i - 1;
            if (rem > 0) memmove(carry, carry + i + 1, (size_t)rem);
            *carry_len = rem;
            buf[pos] = '\0';
            return pos;
        }
    }
    *carry_len = 0;

    /* Read more from fd */
    while (pos < bufsz - 1) {
        char tmp[256];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) {
            buf[pos] = '\0';
            return (pos > 0) ? pos : -1;
        }
        for (int i = 0; i < (int)n; i++) {
            buf[pos++] = tmp[i];
            if (tmp[i] == '\n' || pos == bufsz - 1) {
                buf[pos] = '\0';
                int rem = (int)n - i - 1;
                if (rem > 0) memcpy(carry, tmp + i + 1, (size_t)rem);
                *carry_len = rem;
                return pos;
            }
        }
    }
    buf[pos] = '\0';
    return pos;
}

static void _rm_rf(const char *dir); /* forward decl — defined below */
static int  _place_symlink(const char *target, const char *dst); /* forward decl */

/* Count '/' characters in path — used to sort symlinks shallow-first. */
static int _path_depth(const char *p)
{
    int n = 0;
    for (; *p; p++) if (*p == '/') n++;
    return n;
}

/* Place symlink target→dst; return real_symlink result.
 * If dst is an existing real directory, remove it first (rmdir then _rm_rf)
 * so the symlink wins — mirrors Python's rmdir/rmtree fallback. */
static int _place_symlink(const char *target, const char *dst)
{
    if (real_unlink(dst) < 0 && errno == EISDIR) {
        if (real_rmdir(dst) < 0 && errno == ENOTEMPTY)
            _rm_rf(dst);
    }
    return real_symlink(target, dst);
}

/* Symlink entry buffered for depth-sorted materialization. */
typedef struct { char path[PATH_MAX]; char target[PATH_MAX]; } _LinkEnt;

/* Read manifest and materialize cde-root.
 *
 * Mirrors materialize_libptu.py:
 *   Phase 0 — symlinks, shallowest path first (so /lib64 exists before
 *              /lib64/ld-linux*.so.2; _mkdirs then follows the symlink).
 *   Phase 1 — directories
 *   Phase 2 — regular files
 *
 * Uses real_open/read/real_close — safe to call from destructor.
 */
static void _materialize(const char *pkg_dir, const char *cde_root)
{
    char mf_path[PATH_MAX];
    snprintf(mf_path, sizeof(mf_path), "%s/cde.manifest", pkg_dir);

    int mfd = real_open(mf_path, O_RDONLY, 0);
    if (mfd < 0) {
        dprintf(STDERR_FILENO, "[libptu] cannot open manifest: %s\n", mf_path);
        return;
    }

    /* ── Phase 0: collect + depth-sort symlinks ── */

    _LinkEnt *links  = NULL;
    int       nl     = 0, cap = 0;

    {
        char line[PATH_MAX * 2 + 16], carry[PATH_MAX * 2 + 16];
        int carry_len = 0;
        while (_read_line(mfd, line, (int)sizeof(line), carry, &carry_len) >= 0) {
            size_t ll = strlen(line);
            if (ll > 0 && line[ll-1] == '\n') line[ll-1] = '\0';
            if (!line[0]) continue;
            char kind[4], path[PATH_MAX], tgt[PATH_MAX];
            unsigned int mv = 0644;
            int n = sscanf(line, "%3s %o %4095s %4095s", kind, &mv, path, tgt);
            if (strstr(line, "lib64"))
                dprintf(STDERR_FILENO, "[libptu] DBG lib64 line n=%d kind=%s: %s\n", n, n>=1?kind:"?", line);
            if (n < 4 || kind[0] != 'L' || _is_virtual(path)) continue;
            if (nl == cap) {
                cap = cap ? cap * 2 : 64;
                _LinkEnt *tmp = realloc(links, (size_t)cap * sizeof(_LinkEnt));
                if (!tmp) { free(links); real_close(mfd); return; }
                links = tmp;
            }
            strncpy(links[nl].path,   path, PATH_MAX - 1);
            strncpy(links[nl].target, tgt,  PATH_MAX - 1);
            nl++;
        }
    }

    /* Insertion-sort by path depth (stable, small n in practice) */
    for (int i = 1; i < nl; i++) {
        _LinkEnt key = links[i];
        int d = _path_depth(key.path), j = i - 1;
        while (j >= 0 && _path_depth(links[j].path) > d) {
            links[j+1] = links[j]; j--;
        }
        links[j+1] = key;
    }

    {
        struct stat mst; fstat(mfd, &mst);
        dprintf(STDERR_FILENO, "[libptu] _materialize: manifest=%s size=%lld %d symlinks collected\n",
                mf_path, (long long)mst.st_size, nl);
    }

    /* Place symlinks shallow-first */
    for (int i = 0; i < nl; i++) {
        char dst[PATH_MAX * 2];
        snprintf(dst, sizeof(dst), "%s%s", cde_root, links[i].path);
        char parent[PATH_MAX * 2];
        strncpy(parent, dst, sizeof(parent) - 1);
        char *sl = strrchr(parent, '/');
        if (sl) { *sl = '\0'; _mkdirs(parent); }
        int sr = _place_symlink(links[i].target, dst);
        dprintf(STDERR_FILENO, "[libptu] symlink %s -> %s : %d\n",
                links[i].path, links[i].target, sr);
    }
    free(links);

    /* ── Phases 1 (dirs) and 2 (files) ── */

    int ok = 0, miss = 0;
    char line[PATH_MAX * 2 + 16], carry[PATH_MAX * 2 + 16];
    int carry_len = 0;

    for (int pass = 0; pass < 2; pass++) {
        lseek(mfd, 0, SEEK_SET);
        carry_len = 0;
        int r;
        while ((r = _read_line(mfd, line, (int)sizeof(line), carry, &carry_len)) >= 0) {
            size_t ll = strlen(line);
            if (ll > 0 && line[ll - 1] == '\n') line[ll - 1] = '\0';
            if (!line[0]) continue;

            char kind[4], path[PATH_MAX], target[PATH_MAX];
            unsigned int mode_val = 0644;
            int n = sscanf(line, "%3s %o %4095s %4095s", kind, &mode_val, path, target);
            if (n < 3) continue;
            if (_is_virtual(path)) continue;
            if (kind[0] == 'L') continue; /* symlinks done in phase 0 */

            char dst[PATH_MAX * 2];
            snprintf(dst, sizeof(dst), "%s%s", cde_root, path);

            char parent[PATH_MAX * 2];
            strncpy(parent, dst, sizeof(parent) - 1);
            char *sl = strrchr(parent, '/');
            if (sl) { *sl = '\0'; _mkdirs(parent); }

            if (pass == 0) {
                if (kind[0] == 'D') _mkdirs(dst);
            } else {
                if (kind[0] != 'F') continue;
                struct stat st;
                if (_do_lstat(path, &st) != 0) { miss++; continue; }
                if (S_ISLNK(st.st_mode)) {
                    char ltgt[PATH_MAX];
                    ssize_t ln = real_readlink(path, ltgt, PATH_MAX - 1);
                    if (ln > 0) { ltgt[ln] = '\0'; real_unlink(dst); real_symlink(ltgt, dst); }
                } else if (S_ISREG(st.st_mode)) {
                    _copy_file(path, dst, (mode_t)mode_val);
                    ok++;
                }
            }
        }
    }
    real_close(mfd);
    dprintf(STDERR_FILENO, "[libptu] materialized %d files (%d missing)\n", ok, miss);
}

/* Write env vars as KEY=VALUE lines, skipping libptu internals */
static void _write_env_txt(void)
{
    extern char **environ;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/env.txt", g_output_dir);
    int fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, "LIBPTU_",      7)  == 0) continue;
        if (strncmp(*e, "LD_PRELOAD=", 11)  == 0) continue;
        /* Captured values wrong at replay — overridden explicitly at def-write time */
        if (strncmp(*e, "VINE_AUDIT_MODE",   15) == 0) continue;
        if (strncmp(*e, "VINE_REPLAY_MODE",  16) == 0) continue;
        if (strncmp(*e, "TASKVINE_WARM_POOL", 18) == 0) continue;
        size_t l = strlen(*e);
        if (write(fd, *e, l) < 0 || write(fd, "\n", 1) < 0) break;
    }
    real_close(fd);
}

/* Fork + exec mksquashfs directly (bypasses all libptu hooks via _exit in child) */
static void _build_squashfs(const char *cde_root, const char *sqsh_out)
{
    if (!real_fork) return;
    pid_t pid = real_fork();
    if (pid == 0) {
        /* Child: strip libptu env vars, exec mksquashfs */
        unsetenv("LD_PRELOAD");
        unsetenv("LIBPTU_MODE");
        unsetenv("LIBPTU_OUTPUT");
        unsetenv("LIBPTU_SHM_ID");
        unsetenv("LIBPTU_MANIFEST");
        unsetenv("LIBPTU_BUILD_ON_EXIT");
        execlp("mksquashfs", "mksquashfs",
               cde_root, sqsh_out, "-noappend", "-no-progress", (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            fprintf(stderr, "[libptu] squashfs built: %s\n", sqsh_out);
        else
            fprintf(stderr, "[libptu] mksquashfs failed (status=%d)\n", status);
    }
}

static int g_build_done = 0; /* prevent double build */

/* Recursively delete a directory tree (rm -rf via shell child process).
 * Only called on a private /tmp dir we created — safe to blast. */
static void _rm_rf(const char *dir)
{
    if (!real_fork) return;
    pid_t pid = real_fork();
    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        execlp("rm", "rm", "-rf", "--", dir, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        int st; waitpid(pid, &st, 0);
    }
}

/* Copy a single file src → dst using fork+cp */
static void _cp_file(const char *src, const char *dst)
{
    if (!real_fork) return;
    pid_t pid = real_fork();
    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        execlp("cp", "cp", "--", src, dst, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        int st; waitpid(pid, &st, 0);
    }
}

/* Write apptainer def file embedding env vars from env.txt.
 * Values are single-quote escaped for shell safety. */
static void _write_apptainer_def(const char *def_path, const char *cde_root)
{
    int fd = real_open(def_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    char hdr[PATH_MAX + 64];
    int n = snprintf(hdr, sizeof(hdr),
        "Bootstrap: localimage\nFrom: %s\n\n%%environment\n", cde_root);
    write(fd, hdr, (size_t)n);

    /* Read env.txt, emit export KEY='VALUE' skipping session/BASH_FUNC vars */
    char env_path[PATH_MAX];
    snprintf(env_path, sizeof(env_path), "%s/env.txt", g_output_dir);
    int efd = real_open(env_path, O_RDONLY, 0);
    if (efd >= 0) {
        char line[4096], carry[4096];
        int carry_len = 0, r;
        while ((r = _read_line(efd, line, (int)sizeof(line), carry, &carry_len)) >= 0) {
            size_t ll = strlen(line);
            if (ll > 0 && line[ll - 1] == '\n') line[--ll] = '\0';
            if (ll == 0 || line[0] == '}') continue; /* skip BASH_FUNC closing braces */
            if (strncmp(line, "BASH_FUNC", 9) == 0) continue;
            if (strncmp(line, "SSH_",     4) == 0) continue;
            if (strncmp(line, "DBUS_",    5) == 0) continue;
            if (strncmp(line, "_=",       2) == 0) continue;
            if (strncmp(line, "SHLVL=",   6) == 0) continue;
            if (strncmp(line, "PWD=",     4) == 0) continue;
            if (strncmp(line, "MOTD_",    5) == 0) continue;
            const char *eq = strchr(line, '=');
            if (!eq) continue;
            size_t klen = (size_t)(eq - line);
            const char *val = eq + 1;

            write(fd, "    export ", 11);
            write(fd, line, klen);
            write(fd, "='", 2);
            /* Single-quote escape: replace ' with '\'' */
            const char *p = val;
            while (*p) {
                const char *q = strchr(p, '\'');
                if (q) {
                    write(fd, p, (size_t)(q - p));
                    write(fd, "'\\''", 4);
                    p = q + 1;
                } else {
                    write(fd, p, strlen(p));
                    break;
                }
            }
            write(fd, "'\n", 2);
        }
        real_close(efd);
    }

    const char *footer = "\n%runscript\n    exec \"$@\"\n";
    write(fd, footer, strlen(footer));
    real_close(fd);
}

/* Build apptainer SIF from a def file via fork+exec. */
static void _build_apptainer_sif(const char *def_path, const char *sif_out)
{
    if (!real_fork) return;
    pid_t pid = real_fork();
    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        unsetenv("LIBPTU_MODE");
        unsetenv("LIBPTU_OUTPUT");
        unsetenv("LIBPTU_SHM_ID");
        unsetenv("LIBPTU_MANIFEST");
        unsetenv("LIBPTU_BUILD_ON_EXIT");
        execlp("apptainer", "apptainer", "build", "--fakeroot",
               sif_out, def_path, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            dprintf(STDERR_FILENO, "[libptu] SIF built: %s\n", sif_out);
        else
            dprintf(STDERR_FILENO, "[libptu] apptainer build failed (status=%d)\n", status);
    }
}

/* Public API: called by the launcher from its main thread (full stack).
 * Materializes to local /tmp (fast), builds apptainer SIF, copies to NFS output. */
void libptu_build_env(void)
{
    if (g_build_done) return;
    if (!g_output_dir[0]) return;

    g_build_done = 1;

    char tmp_base[PATH_MAX];
    snprintf(tmp_base, sizeof(tmp_base), "/tmp/libptu-build-%d", (int)getpid());
    char tmp_root[PATH_MAX], tmp_sif[PATH_MAX], def_path[PATH_MAX];
    snprintf(tmp_root,  sizeof(tmp_root),  "%s/cde-root",       tmp_base);
    snprintf(tmp_sif,   sizeof(tmp_sif),   "%s/vine-worker.sif", tmp_base);
    snprintf(def_path,  sizeof(def_path),  "%s/vine-worker.def", tmp_base);

    char sif_out[PATH_MAX];
    snprintf(sif_out, sizeof(sif_out), "%s/vine-worker.sif", g_output_dir);

    real_mkdir(tmp_base, 0755);
    real_mkdir(tmp_root, 0755);

#define _TS(v) struct timespec v; clock_gettime(CLOCK_MONOTONIC, &v)
#define _ELAPSED(a, b) ((double)((b).tv_sec - (a).tv_sec) + 1e-9 * ((b).tv_nsec - (a).tv_nsec))

    _TS(t0); _TS(ta);
    dprintf(STDERR_FILENO, "[libptu] materializing to %s ...\n", tmp_root);
    /* _materialize(g_output_dir, tmp_root); */
    _TS(tb);
    dprintf(STDERR_FILENO, "[libptu] materialization: %.2f s\n", _ELAPSED(ta, tb));

    _write_env_txt();

    /* _TS(tc);
    dprintf(STDERR_FILENO, "[libptu] writing apptainer def ...\n");
    _write_apptainer_def(def_path, tmp_root);
    _TS(td);
    dprintf(STDERR_FILENO, "[libptu] def write: %.2f s\n", _ELAPSED(tc, td));

    _TS(te);
    dprintf(STDERR_FILENO, "[libptu] building apptainer SIF ...\n");
    _build_apptainer_sif(def_path, tmp_sif);
    _TS(tf);
    dprintf(STDERR_FILENO, "[libptu] SIF build: %.2f s\n", _ELAPSED(te, tf));

    if (rename(tmp_sif, sif_out) != 0)
        _cp_file(tmp_sif, sif_out); */

    _TS(tg);
    dprintf(STDERR_FILENO, "[libptu] build_env total: %.2f s  (materialize=%.2f)\n",
            _ELAPSED(t0, tg), _ELAPSED(ta, tb));

#undef _TS
#undef _ELAPSED

    const char *keep = getenv("LIBPTU_KEEP_CDEROOT");
    if (keep && keep[0] == '1') {
        dprintf(STDERR_FILENO, "[libptu] LIBPTU_KEEP_CDEROOT=1: retaining %s\n", tmp_base);
    } else {
        _rm_rf(tmp_base);
    }
}

static void _build_on_exit(void)
{
    if (g_shmid < 0 || !g_output_dir[0]) return;           /* child process */
    if (g_mode != MODE_LOG && g_mode != MODE_CAPTURE) return;

    const char *flag = getenv("LIBPTU_BUILD_ON_EXIT");
    if (!flag || flag[0] != '1') return;

    libptu_build_env();
}

/* ─────────────────────────────────────────────────────────────────────────
 * Constructor — runs before main()
 * ───────────────────────────────────────────────────────────────────────── */

/* ─────────────────────────────────────────────────────────────────────────
 * libptu_lazy_init — resolve real_* pointers on first hook invocation.
 *
 * Called from HOOK_PREAMBLE's early-exit path so hooks that fire before
 * the constructor (during early dynamic linking) don't crash on NULL ptrs.
 * After the constructor runs g_real_initialized=1 and this is a no-op.
 * ───────────────────────────────────────────────────────────────────────── */
static int g_real_initialized = 0;

void libptu_lazy_init(void)
{
    if (__builtin_expect(g_real_initialized, 1)) return;

#define LOAD(name) do { if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name); } while(0)
    LOAD(open); LOAD(openat); LOAD(creat); LOAD(close);
    LOAD(stat); LOAD(lstat); LOAD(fstat); LOAD(fstatat);
    LOAD(access); LOAD(faccessat);
    LOAD(readlink); LOAD(readlinkat);
    LOAD(symlink); LOAD(symlinkat);
    LOAD(link); LOAD(linkat);
    LOAD(unlink); LOAD(unlinkat);
    LOAD(rename); LOAD(renameat);
    LOAD(mkdir); LOAD(mkdirat); LOAD(rmdir);
    LOAD(chmod); LOAD(fchmod); LOAD(chown); LOAD(lchown); LOAD(fchownat);
    LOAD(chdir); LOAD(fchdir);
    LOAD(dup); LOAD(dup2); LOAD(dup3); LOAD(fcntl);
    LOAD(mmap); LOAD(execve); LOAD(execveat);
    LOAD(dlopen); LOAD(dlclose); LOAD(fork);
    if (!real___xstat)      real___xstat      = dlsym(RTLD_NEXT, "__xstat");
    if (!real___lxstat)     real___lxstat     = dlsym(RTLD_NEXT, "__lxstat");
    if (!real___fxstat)     real___fxstat     = dlsym(RTLD_NEXT, "__fxstat");
    if (!real___fxstatat)   real___fxstatat   = dlsym(RTLD_NEXT, "__fxstatat");
    if (!real___xstat64)    real___xstat64    = dlsym(RTLD_NEXT, "__xstat64");
    if (!real___lxstat64)   real___lxstat64   = dlsym(RTLD_NEXT, "__lxstat64");
    if (!real___fxstat64)   real___fxstat64   = dlsym(RTLD_NEXT, "__fxstat64");
    if (!real___fxstatat64) real___fxstatat64 = dlsym(RTLD_NEXT, "__fxstatat64");
    if (!real_chmodat)      real_chmodat      = dlsym(RTLD_NEXT, "fchmodat");
#ifdef SYS_statx
    /* statx() is a glibc 2.28+ symbol; may be NULL on older glibc */
    if (!real_statx)      real_statx      = dlsym(RTLD_NEXT, "statx");
#endif
#undef LOAD

    g_real_initialized = 1;
}

/* Flush manifest on SIGTERM/SIGINT so long-lived processes (warm-pool library)
 * don't lose buffered entries when killed by vine_worker. */
static void _libptu_sig_flush(int signum)
{
    if (g_manifest_fp) {
        fflush(g_manifest_fp);
    }
    signal(signum, SIG_DFL);
    raise(signum);
}

__attribute__((constructor))
static void libptu_init(void)
{
    /* Block hooks from firing during our own init */
    tl_in_hook = 1;

    /* Resolve all real function pointers (marks g_real_initialized=1) */
    libptu_lazy_init();

    /* ── Parse mode from env ── */
    const char *mode_str = getenv("LIBPTU_MODE");
    if (!mode_str) {
        tl_in_hook = 0;
        return; /* MODE_DISABLED — zero overhead passthrough */
    }
    if (strcmp(mode_str, "log") == 0)         g_mode = MODE_LOG;
    else if (strcmp(mode_str, "capture") == 0) g_mode = MODE_CAPTURE;
    else if (strcmp(mode_str, "replay") == 0)  g_mode = MODE_REPLAY;
    else {
        fprintf(stderr, "[libptu] unknown LIBPTU_MODE=%s, disabling\n", mode_str);
        tl_in_hook = 0;
        return;
    }

    /* ── Init CWD ── */
    if (!getcwd(g_cwd, PATH_MAX)) g_cwd[0] = '\0';

    /* ── Load skip patterns (config file next to libptu.so or LIBPTU_SKIP_FILE) ── */
    load_skip_patterns();

    /* ── Mode-specific init ── */
    if (g_mode == MODE_REPLAY) {
        const char *cr = getenv("LIBPTU_CDE_ROOT");
        if (cr) {
            strncpy(g_cde_root, cr, PATH_MAX - 1);
        } else {
            /* Try LIBPTU_OUTPUT/cde-root */
            const char *out = getenv("LIBPTU_OUTPUT");
            if (out) snprintf(g_cde_root, PATH_MAX, "%s/cde-root", out);
        }
        g_cde_root_len = (int)strlen(g_cde_root);

    } else {
        /* log or capture mode */

        /* ── Session attach: child process joining existing session ── */
        const char *shm_id_str  = getenv("LIBPTU_SHM_ID");
        const char *mf_path_env = getenv("LIBPTU_MANIFEST");

        if (shm_id_str && mf_path_env) {
            /* Join existing session — attach to shared dedup table */
            int shmid = atoi(shm_id_str);
            g_session = (libptu_session_t *)shmat(shmid, NULL, 0);
            if (g_session == (libptu_session_t *)-1) g_session = NULL;
            g_shmid = -1; /* attachers never call IPC_RMID */

            strncpy(g_manifest_path, mf_path_env, PATH_MAX - 1);
            strncpy(g_output_dir, g_manifest_path, PATH_MAX - 1);
            /* strip /cde.manifest suffix to get output dir */
            char *slash = strrchr(g_output_dir, '/');
            if (slash) *slash = '\0';

            /* MUST be set before any manifest_record_abs() call below (e.g.
             * scan_loaded_libs()) — those calls do capture-copies gated on
             * g_cde_root being non-empty (see capture_copy_file's guard). */
            if (g_mode == MODE_CAPTURE) {
                snprintf(g_cde_root, PATH_MAX, "%s/cde-root", g_output_dir);
                g_cde_root_len = (int)strlen(g_cde_root);
            }

            /* Open manifest in APPEND mode — never truncate */
            g_manifest_fd = open(g_manifest_path,
                                 O_WRONLY | O_APPEND | O_CREAT, 0644);
            if (g_manifest_fd >= 0) {
                g_manifest_fp = fdopen(g_manifest_fd, "a");
                /* No stdio buffering: attached processes (e.g. warm-pool library)
                 * may be killed with SIGKILL — every write must reach the kernel
                 * immediately so no entries are lost. */
                if (g_manifest_fp) setvbuf(g_manifest_fp, NULL, _IONBF, 0);
            }

            /* Scan maps to catch any new mmap'd files in this process */
            scan_loaded_libs();
            scan_proc_maps_init();

        } else {
            /* ── Session create: root process (launcher or direct use) ── */
            const char *out = getenv("LIBPTU_OUTPUT");
            if (!out) {
                fprintf(stderr, "[libptu] LIBPTU_OUTPUT not set, disabling\n");
                g_mode = MODE_DISABLED;
                tl_in_hook = 0;
                return;
            }
            strncpy(g_output_dir, out, PATH_MAX - 1);

            /* Create output dirs */
            char cde_root_dir[PATH_MAX];
            snprintf(cde_root_dir, sizeof(cde_root_dir),
                     "%s/cde-root", g_output_dir);
            mkdir(g_output_dir, 0755);
            mkdir(cde_root_dir, 0755);

            /* MUST be set before any manifest_record_abs() call below (e.g.
             * scan_loaded_libs(), capture_environment()) — those calls do
             * capture-copies gated on g_cde_root being non-empty. */
            if (g_mode == MODE_CAPTURE) {
                strncpy(g_cde_root, cde_root_dir, PATH_MAX - 1);
                g_cde_root_len = (int)strlen(g_cde_root);
            }

            /* Open manifest file (truncate — first time) */
            snprintf(g_manifest_path, PATH_MAX,
                     "%s/cde.manifest", g_output_dir);
            g_manifest_fd = open(g_manifest_path,
                                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (g_manifest_fd >= 0) {
                g_manifest_fp = fdopen(g_manifest_fd, "w");
                if (g_manifest_fp) {
                    static char mf_buf[262144];
                    setvbuf(g_manifest_fp, mf_buf, _IOFBF, sizeof(mf_buf));
                }
            }

            /* MODE_CAPTURE only: pre-place the well-known Ubuntu multiarch
             * compat symlinks (/bin, /lib, /lib32, /lib64, /libx32, /sbin ->
             * usr/...) in cde-root BEFORE any other file discovery happens.
             * Without this, a file nested under one of these (e.g.
             * /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2) can get its parent
             * directories auto-created as REAL directories in cde-root before
             * the symlink entry for /lib itself is ever recorded — ancestor
             * symlinks are only discovered by record_ancestors_locked(),
             * which runs AFTER the triggering file is already copied. Once
             * that race is lost, recovering it (see capture_place_symlink's
             * rmdir/rm_rf fallback) has to destroy the already-copied
             * subtree, and since libptu only ever records each real
             * underlying path once (dedup), that file is never re-copied
             * under its post-symlink-resolved path — it's just gone. Doing
             * this up front for the known common cases avoids hitting that
             * fallback at all for the paths most likely to matter. */
            if (g_mode == MODE_CAPTURE) {
                static const char *os_symlinks[] = {
                    "/bin", "/lib", "/lib32", "/lib64", "/libx32", "/sbin", NULL
                };
                for (int i = 0; os_symlinks[i]; i++)
                    manifest_record_abs(os_symlinks[i]);
            }

            /* Create shared memory session for cross-process dedup */
            int shmid = shmget(IPC_PRIVATE,
                               sizeof(libptu_session_t),
                               IPC_CREAT | 0666);
            if (shmid >= 0) {
                g_session = (libptu_session_t *)shmat(shmid, NULL, 0);
                if (g_session == (libptu_session_t *)-1) {
                    g_session = NULL;
                } else {
                    g_session->magic = LIBPTU_SESSION_MAGIC;
                    memset((void *)g_session->seen_hashes, 0,
                           sizeof(g_session->seen_hashes));
                    strncpy(g_session->manifest_path,
                            g_manifest_path, PATH_MAX - 1);
                    g_shmid = shmid; /* creator — shmem NOT yet IPC_RMID'd */
                }
            }

            /* Publish session info for children via environment */
            if (g_shmid >= 0) {
                char shmid_str[32];
                snprintf(shmid_str, sizeof(shmid_str), "%d", g_shmid);
                setenv("LIBPTU_SHM_ID",   shmid_str,       1);
                setenv("LIBPTU_MANIFEST",  g_manifest_path, 1);
            }

            /* Capture environment variables (same format as PTU) */
            capture_environment();

            /* Capture all shared libs loaded at startup + main executable */
            scan_loaded_libs();

            /* Catch file-backed mappings created before our hooks fired */
            scan_proc_maps_init();
        }
    }

    /* Install signal handlers to flush manifest if killed (e.g. warm-pool library) */
    if (g_mode == MODE_LOG || g_mode == MODE_CAPTURE) {
        signal(SIGTERM, _libptu_sig_flush);
        signal(SIGINT,  _libptu_sig_flush);
    }

    /* ── Enable hooks ── */
    tl_in_hook = 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Destructor — flush and close manifest
 * ───────────────────────────────────────────────────────────────────────── */
__attribute__((destructor))
static void libptu_fini(void)
{
    tl_in_hook = 1;

    /* Final scan — catches locale/gconv files mmap'd after constructor */
    if (g_mode == MODE_LOG || g_mode == MODE_CAPTURE) {
        /* Walk the dynamic linker's link map to catch .so files loaded AFTER
         * the constructor — specifically glibc's lazily-loaded NSS modules
         * (libnss_dns.so.2, libnss_files.so.2) which are opened via
         * __libc_dlopen_mode() inside ld-linux.so, bypassing our dlopen() hook.
         * dl_iterate_phdr sees everything in the link map regardless of how it
         * was loaded, so this is generic: no hardcoded library names. */
        scan_loaded_libs();
        scan_proc_maps_init();
    }

    pthread_mutex_lock(&g_manifest_mutex);
    if (g_manifest_fd >= 0) flock(g_manifest_fd, LOCK_EX);
    if (g_manifest_fp) {
        fflush(g_manifest_fp);
        fclose(g_manifest_fp);
        g_manifest_fp = NULL;
        g_manifest_fd = -1;
    }
    pthread_mutex_unlock(&g_manifest_mutex);

    /* Build cde-root + squashfs overlay if requested */
    _build_on_exit();

    /* Detach shared memory — IPC_RMID is NOT called automatically.
     * The segment persists until the session is explicitly cleaned up
     * (vine_worker exit or manual: ipcrm -m <LIBPTU_SHM_ID>). */
    if (g_session) {
        shmdt(g_session);
        g_session = NULL;
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 *  H O O K S
 * ═════════════════════════════════════════════════════════════════════════ */

/* ── Shared internal open/openat implementations ──────────────────────── */
/*
 * _open_impl / _openat_impl: called with tl_in_hook already set to 1.
 * open64 / openat64 call these directly to avoid re-entering our hook.
 */
static int _open_impl(const char *path, int flags, mode_t mode)
{
    const char *abs;
    char abs_buf[PATH_MAX];
    if (!path) {
        return real_open(path, flags, mode); /* let kernel set EFAULT */
    }
    if (path[0] == '/') {
        abs = path;
    } else {
        abs = resolve_at(AT_FDCWD, path, abs_buf);
    }

    const char *call_path = path;
    char redir_buf[PATH_MAX];
    if (g_mode == MODE_REPLAY && abs)
        call_path = maybe_redirect(abs, redir_buf);

    int fd = real_open(call_path, flags, mode);
    if (fd >= 0 && abs) {
        fd_table_set(fd, abs);
        if (g_mode != MODE_REPLAY) handle_path(abs);
    }
    return fd;
}

static int _openat_impl(int dirfd, const char *path, int flags, mode_t mode)
{
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);

    const char *call_path = path;
    int call_dirfd = dirfd;
    char redir_buf[PATH_MAX];
    if (g_mode == MODE_REPLAY && abs) {
        const char *rp = maybe_redirect(abs, redir_buf);
        if (rp != abs) { call_path = rp; call_dirfd = AT_FDCWD; }
    }

    int fd = real_openat(call_dirfd, call_path, flags, mode);
    if (fd >= 0 && abs) {
        fd_table_set(fd, abs);
        if (g_mode != MODE_REPLAY) handle_path(abs);
    }
    return fd;
}

/* ── open / open64 / creat ─────────────────────────────────────────────── */

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    }
    HOOK_PREAMBLE(real_open(path, flags, mode));
    int fd = _open_impl(path, flags, mode);
    HOOK_RETURN(fd);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    }
    /* Call _open_impl directly — avoids recursive hook entry (tl_in_hook would
     * already be 1 if we called open() here, turning it into a passthrough). */
    HOOK_PREAMBLE(real_open(path, flags | O_LARGEFILE, mode));
    int fd = _open_impl(path, flags | O_LARGEFILE, mode);
    HOOK_RETURN(fd);
}

int creat(const char *path, mode_t mode)
{
    HOOK_PREAMBLE(real_creat(path, mode));
    /* creat(p, m) == open(p, O_WRONLY|O_CREAT|O_TRUNC, m) */
    int fd = _open_impl(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    HOOK_RETURN(fd);
}

/* ── openat / openat64 ─────────────────────────────────────────────────── */

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    }
    HOOK_PREAMBLE(real_openat(dirfd, path, flags, mode));
    int fd = _openat_impl(dirfd, path, flags, mode);
    HOOK_RETURN(fd);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    }
    HOOK_PREAMBLE(real_openat(dirfd, path, flags | O_LARGEFILE, mode));
    int fd = _openat_impl(dirfd, path, flags | O_LARGEFILE, mode);
    HOOK_RETURN(fd);
}

/* ── close / dup / dup2 / dup3 / fcntl ───────────────────────────────── */

int close(int fd)
{
    libptu_lazy_init();
    if (!real_close) return 0;
    if (g_mode != MODE_DISABLED && !tl_in_hook)
        fd_table_del(fd);
    return real_close(fd);
}

int dup(int oldfd)
{
    libptu_lazy_init();
    if (!real_dup) return -1;
    int newfd = real_dup(oldfd);
    if (newfd >= 0 && g_mode != MODE_DISABLED && !tl_in_hook)
        fd_table_dup(oldfd, newfd);
    return newfd;
}

int dup2(int oldfd, int newfd)
{
    libptu_lazy_init();
    if (!real_dup2) return -1;
    int r = real_dup2(oldfd, newfd);
    if (r >= 0 && g_mode != MODE_DISABLED && !tl_in_hook)
        fd_table_dup(oldfd, newfd);
    return r;
}

int dup3(int oldfd, int newfd, int flags)
{
    libptu_lazy_init();
    if (!real_dup3) return -1;
    int r = real_dup3(oldfd, newfd, flags);
    if (r >= 0 && g_mode != MODE_DISABLED && !tl_in_hook)
        fd_table_dup(oldfd, newfd);
    return r;
}

int fcntl(int fd, int cmd, ...)
{
    libptu_lazy_init();
    if (!real_fcntl) return -1;
    va_list ap; va_start(ap, cmd);
    unsigned long arg = va_arg(ap, unsigned long);
    va_end(ap);
    int r = real_fcntl(fd, cmd, arg);
    if (r >= 0 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
            && g_mode != MODE_DISABLED && !tl_in_hook)
        fd_table_dup(fd, r);
    return r;
}

/* ── Shared path resolution helper for non-dirfd hooks ─────────────────── */
/*
 * Resolves path → absolute path in abs_buf, returns pointer to the
 * canonical string (either path itself if already absolute, or abs_buf).
 * Returns NULL if path is NULL.
 */
static inline const char *resolve_abs(const char *path, char *abs_buf)
{
    if (!path) return NULL;
    if (path[0] == '/') return path;
    return resolve_at(AT_FDCWD, path, abs_buf);
}

/* ── fopen / fopen64 / freopen ─────────────────────────────────────────── */
/*
 * glibc's fopen() uses its own private __open64_nocancel() internally, which
 * bypasses the exported open()/openat() PLT entries and thus our hooks.
 * Non-glibc libraries (e.g. OpenSSL, libyaml) call fopen() via PLT, so
 * hooking fopen() at the symbol level captures their file accesses.
 */
static FILE *(*real_fopen)  (const char *, const char *)         = NULL;
static FILE *(*real_fopen64)(const char *, const char *)         = NULL;
static FILE *(*real_freopen)(const char *, const char *, FILE *) = NULL;

FILE *fopen(const char *path, const char *mode)
{
    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen");
    HOOK_PREAMBLE(real_fopen ? real_fopen(path, mode) : NULL);
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    FILE *f = real_fopen(cp, mode);
    if (f && abs) {
        fd_table_set(fileno(f), abs);
        if (g_mode != MODE_REPLAY) handle_path(abs);
    }
    HOOK_RETURN(f);
}

FILE *fopen64(const char *path, const char *mode)
{
    if (!real_fopen64) real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    HOOK_PREAMBLE(real_fopen64 ? real_fopen64(path, mode) : NULL);
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    FILE *f = real_fopen64 ? real_fopen64(cp, mode) : NULL;
    if (f && abs) {
        fd_table_set(fileno(f), abs);
        if (g_mode != MODE_REPLAY) handle_path(abs);
    }
    HOOK_RETURN(f);
}

FILE *freopen(const char *path, const char *mode, FILE *stream)
{
    if (!real_freopen) real_freopen = dlsym(RTLD_NEXT, "freopen");
    HOOK_PREAMBLE(real_freopen ? real_freopen(path, mode, stream) : NULL);
    if (!path) HOOK_RETURN(real_freopen(path, mode, stream));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    FILE *f = real_freopen(cp, mode, stream);
    if (f && abs) {
        fd_table_set(fileno(f), abs);
        if (g_mode != MODE_REPLAY) handle_path(abs);
    }
    HOOK_RETURN(f);
}

/* ── stat family ───────────────────────────────────────────────────────── */

int stat(const char *path, struct stat *buf)
{
    HOOK_PREAMBLE(real_stat(path, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    int r = real_stat(cp, buf);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int lstat(const char *path, struct stat *buf)
{
    HOOK_PREAMBLE(real_lstat(path, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    int r = real_lstat(cp, buf);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int fstat(int fd, struct stat *buf)
{
    libptu_lazy_init();
    if (!real_fstat) { errno = ENOSYS; return -1; }
    int r = real_fstat(fd, buf);
    if (r == 0 && g_mode != MODE_DISABLED && !tl_in_hook) {
        char *p = fd_get(fd);
        if (p) {
            tl_in_hook = 1; handle_path(p); tl_in_hook = 0;
        }
    }
    return r;
}

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    HOOK_PREAMBLE(real_fstatat(dirfd, path, buf, flags));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = path;
    int call_dirfd = dirfd;
    if (g_mode == MODE_REPLAY && abs) {
        const char *rp = maybe_redirect(abs, redir_buf);
        if (rp != abs) { cp = rp; call_dirfd = AT_FDCWD; }
    }
    int r = real_fstatat(call_dirfd, cp, buf, flags);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

/* glibc <2.33 versioned wrappers — only compiled when symbols exist */
int __xstat(int ver, const char *path, struct stat *buf)
{
    if (!real___xstat) return -1;
    HOOK_PREAMBLE(real___xstat(ver, path, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    int r = real___xstat(ver, path, buf);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int __lxstat(int ver, const char *path, struct stat *buf)
{
    if (!real___lxstat) return -1;
    HOOK_PREAMBLE(real___lxstat(ver, path, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    int r = real___lxstat(ver, path, buf);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int __fxstat(int ver, int fd, struct stat *buf)
{
    if (!real___fxstat) return -1;
    int r = real___fxstat(ver, fd, buf);
    if (r == 0 && g_mode != MODE_DISABLED && !tl_in_hook) {
        char *p = fd_get(fd);
        if (p) { tl_in_hook = 1; handle_path(p); tl_in_hook = 0; }
    }
    return r;
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *buf, int flags)
{
    if (!real___fxstatat) return -1;
    HOOK_PREAMBLE(real___fxstatat(ver, dirfd, path, buf, flags));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    int r = real___fxstatat(ver, dirfd, path, buf, flags);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

/* ── __xstat64 / __lxstat64 / __fxstat64 / __fxstatat64 ────────────────── */
/* 64-bit LFS stat wrappers — identical logic to the non-64 variants above.
 * struct stat64 == struct stat on x86_64 so casts are safe. */

int __xstat64(int ver, const char *path, struct stat64 *buf)
{
    if (!real___xstat64) return -1;
    HOOK_PREAMBLE(real___xstat64(ver, path, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    int r = real___xstat64(ver, path, buf);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int __lxstat64(int ver, const char *path, struct stat64 *buf)
{
    if (!real___lxstat64) return -1;
    HOOK_PREAMBLE(real___lxstat64(ver, path, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    int r = real___lxstat64(ver, path, buf);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int __fxstat64(int ver, int fd, struct stat64 *buf)
{
    if (!real___fxstat64) return -1;
    int r = real___fxstat64(ver, fd, buf);
    if (r == 0 && g_mode != MODE_DISABLED && !tl_in_hook) {
        char *p = fd_get(fd);
        if (p) { tl_in_hook = 1; handle_path(p); tl_in_hook = 0; }
    }
    return r;
}

int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *buf, int flags)
{
    if (!real___fxstatat64) return -1;
    HOOK_PREAMBLE(real___fxstatat64(ver, dirfd, path, buf, flags));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    int r = real___fxstatat64(ver, dirfd, path, buf, flags);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

/* ── statx ─────────────────────────────────────────────────────────────── */
/*
 * Python 3.9+ on Linux 4.11+ calls statx() to check whether a .py source
 * file is newer than its .pyc cache. Our stat()/lstat() hooks never see
 * these calls. Hooking statx() fills Gap 4 from MANIFEST_GAPS.md.
 *
 * real_statx = dlsym(RTLD_NEXT, "statx") — valid on glibc 2.28+.
 * Falls back to syscall(SYS_statx) if real_statx is NULL (older glibc).
 * Entire hook compiled away with #ifdef SYS_statx on kernels/arches without it.
 */
#ifdef SYS_statx
int statx(int dirfd, const char *path, int flags,
          unsigned int mask, struct statx *buf)
{
    /* Fallback: real_statx if available, otherwise raw syscall */
#define REAL_STATX(d,p,f,m,b) \
    (real_statx ? real_statx((d),(p),(f),(m),(b)) \
                : (int)syscall(SYS_statx,(d),(p),(f),(m),(b)))

    HOOK_PREAMBLE(REAL_STATX(dirfd, path, flags, mask, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);

    const char *cp = path;
    int call_dirfd = dirfd;
    char redir_buf[PATH_MAX];
    if (g_mode == MODE_REPLAY && abs) {
        const char *rp = maybe_redirect(abs, redir_buf);
        if (rp != abs) { cp = rp; call_dirfd = AT_FDCWD; }
    }

    int r = REAL_STATX(call_dirfd, cp, flags, mask, buf);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
#undef REAL_STATX
}
#endif /* SYS_statx */

/* ── access / faccessat ────────────────────────────────────────────────── */

int access(const char *path, int mode)
{
    HOOK_PREAMBLE(real_access(path, mode));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    int r = real_access(cp, mode);
    if (abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int faccessat(int dirfd, const char *path, int amode, int flags)
{
    HOOK_PREAMBLE(real_faccessat(dirfd, path, amode, flags));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    int call_dirfd = (cp == path) ? dirfd : AT_FDCWD;
    int r = real_faccessat(call_dirfd, cp, amode, flags);
    if (abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

/* ── readlink / readlinkat ─────────────────────────────────────────────── */

ssize_t readlink(const char *path, char *buf, size_t bufsz)
{
    HOOK_PREAMBLE(real_readlink(path, buf, bufsz));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    ssize_t r = real_readlink(cp, buf, bufsz);
    if (r >= 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsz)
{
    HOOK_PREAMBLE(real_readlinkat(dirfd, path, buf, bufsz));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    char redir_buf[PATH_MAX];
    const char *cp = (g_mode == MODE_REPLAY && abs)
                     ? maybe_redirect(abs, redir_buf) : path;
    int call_dirfd = (cp == path) ? dirfd : AT_FDCWD;
    ssize_t r = real_readlinkat(call_dirfd, cp, buf, bufsz);
    if (r >= 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

/* ── symlink / link / unlink / rename ─────────────────────────────────── */

/* These mutating operations are pass-through in log mode (we don't audit writes).
 * Guard with lazy_init so real_* ptrs are valid even on early calls. */
#define PASSTHROUGH(real_fn, ...) \
    do { libptu_lazy_init(); \
         return real_fn ? real_fn(__VA_ARGS__) : (errno = ENOSYS, -1); } while(0)

int symlink(const char *target, const char *linkpath)
    { PASSTHROUGH(real_symlink, target, linkpath); }

int symlinkat(const char *target, int newdirfd, const char *linkpath)
    { PASSTHROUGH(real_symlinkat, target, newdirfd, linkpath); }

int link(const char *oldpath, const char *newpath)
    { PASSTHROUGH(real_link, oldpath, newpath); }

int linkat(int olddirfd, const char *oldpath, int newdirfd,
           const char *newpath, int flags)
    { PASSTHROUGH(real_linkat, olddirfd, oldpath, newdirfd, newpath, flags); }

int unlink(const char *path)
    { PASSTHROUGH(real_unlink, path); }

int unlinkat(int dirfd, const char *path, int flags)
    { PASSTHROUGH(real_unlinkat, dirfd, path, flags); }

int rename(const char *oldpath, const char *newpath)
    { PASSTHROUGH(real_rename, oldpath, newpath); }

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
    { PASSTHROUGH(real_renameat, olddirfd, oldpath, newdirfd, newpath); }

/* ── mkdir / rmdir ─────────────────────────────────────────────────────── */

int mkdir(const char *path, mode_t mode)
    { PASSTHROUGH(real_mkdir, path, mode); }

int mkdirat(int dirfd, const char *path, mode_t mode)
{
    HOOK_PREAMBLE(real_mkdirat(dirfd, path, mode));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    int r = real_mkdirat(dirfd, path, mode);
    if (r == 0 && abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int rmdir(const char *path)
    { PASSTHROUGH(real_rmdir, path); }

/* ── chmod / chown ─────────────────────────────────────────────────────── */

int chmod(const char *path, mode_t mode)
{
    HOOK_PREAMBLE(real_chmod(path, mode));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);
    int r = real_chmod(path, mode);
    if (abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int fchmod(int fd, mode_t mode)
{
    libptu_lazy_init();
    if (!real_fchmod) return -1;
    return real_fchmod(fd, mode);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    if (!real_chmodat) real_chmodat = dlsym(RTLD_NEXT, "fchmodat");
    if (!real_chmodat) return -1;
    HOOK_PREAMBLE(real_chmodat(dirfd, path, mode, flags));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    int r = real_chmodat(dirfd, path, mode, flags);
    if (abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}

int chown(const char *path, uid_t owner, gid_t group)
{
    libptu_lazy_init();
    return real_chown ? real_chown(path, owner, group) : (errno = ENOSYS, -1);
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    libptu_lazy_init();
    return real_lchown ? real_lchown(path, owner, group) : (errno = ENOSYS, -1);
}

int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags)
{
    libptu_lazy_init();
    return real_fchownat ? real_fchownat(dirfd, path, owner, group, flags)
                         : (errno = ENOSYS, -1);
}

/* ── chdir / fchdir ───────────────────────────────────────────────────── */

int chdir(const char *path)
{
    libptu_lazy_init();
    if (!real_chdir) return (errno = ENOSYS, -1);
    int r = real_chdir(path);
    if (r == 0) {
        char abs_buf[PATH_MAX];
        const char *new_cwd;
        if (path[0] == '/') {
            new_cwd = path;
        } else {
            pthread_rwlock_rdlock(&g_cwd_rwlock);
            snprintf(abs_buf, PATH_MAX, "%s/%s", g_cwd, path);
            pthread_rwlock_unlock(&g_cwd_rwlock);
            new_cwd = abs_buf;
        }
        pthread_rwlock_wrlock(&g_cwd_rwlock);
        strncpy(g_cwd, new_cwd, PATH_MAX - 1);
        g_cwd[PATH_MAX - 1] = '\0';
        pthread_rwlock_unlock(&g_cwd_rwlock);
    }
    return r;
}

int fchdir(int fd)
{
    libptu_lazy_init();
    if (!real_fchdir) return (errno = ENOSYS, -1);
    int r = real_fchdir(fd);
    if (r == 0) {
        char *p = fd_get(fd);
        if (p) {
            pthread_rwlock_wrlock(&g_cwd_rwlock);
            strncpy(g_cwd, p, PATH_MAX - 1);
            g_cwd[PATH_MAX - 1] = '\0';
            pthread_rwlock_unlock(&g_cwd_rwlock);
        }
    }
    return r;
}

/* ── mmap ──────────────────────────────────────────────────────────────── */

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    libptu_lazy_init();
    if (!real_mmap) return MAP_FAILED;
    void *r = real_mmap(addr, length, prot, flags, fd, offset);
    if (r != MAP_FAILED && !(flags & MAP_ANONYMOUS) && fd >= 0
            && g_mode != MODE_DISABLED && !tl_in_hook) {
        char *p = fd_get(fd);
        if (p) {
            tl_in_hook = 1;
            handle_path(p);
            tl_in_hook = 0;
        }
    }
    return r;
}

/* mmap64 is the same as mmap on 64-bit systems with O_LARGEFILE */
void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
    __attribute__((alias("mmap")));

/* ── execve / execveat ─────────────────────────────────────────────────── */

/*
 * For execve: capture the binary and its ELF interpreter (ld-linux).
 * In replay mode: inject LD_PRELOAD into the child's environment so auditing
 * continues across exec (libptu propagates itself to child processes).
 */

static void capture_elf_interp(const char *binary_path)
{
    /* Read ELF header and find PT_INTERP segment */
    int fd = real_open(binary_path, O_RDONLY, 0);
    if (fd < 0) return;

    Elf64_Ehdr ehdr;
    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)
            || ehdr.e_ident[0] != 0x7f
            || ehdr.e_ident[1] != 'E') {
        close(fd); return;
    }

    /* Read program headers */
    if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) { close(fd); return; }

    Elf64_Phdr phdr;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        lseek(fd, (off_t)(ehdr.e_phoff + (uint64_t)i * sizeof(Elf64_Phdr)), SEEK_SET);
        if (read(fd, &phdr, sizeof(phdr)) != sizeof(phdr)) break;
        if (phdr.p_type == PT_INTERP) {
            char interp[PATH_MAX];
            lseek(fd, (off_t)phdr.p_offset, SEEK_SET);
            ssize_t n = read(fd, interp, sizeof(interp) - 1);
            if (n > 0) {
                interp[n] = '\0';
                manifest_record_abs(interp);
            }
            break;
        }
    }
    close(fd);
}

/* Build envp with LD_PRELOAD=<our_path> prepended (if not already present).
 * Returns a newly allocated array that caller must free after exec.
 * (exec replaces process image, so it leaks only on failure.) */
static char **inject_ld_preload(char *const envp[])
{
    if (!envp) return NULL;

    /* Find our own path via /proc/self/maps */
    char our_path[PATH_MAX] = "";
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[4096];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libptu.so")) {
                char *p = strchr(line, '/');
                if (p) {
                    size_t l = strlen(p);
                    if (l > 0 && p[l-1] == '\n') p[l-1] = '\0';
                    strncpy(our_path, p, PATH_MAX - 1);
                    break;
                }
            }
        }
        fclose(maps);
    }
    if (!our_path[0]) return (char **)envp; /* can't find ourselves */

    /* Collect LIBPTU_* session vars from current process that may be absent
     * from explicit envp (e.g. vine_worker warm-pool exec with clean env). */
    static const char *libptu_vars[] = {
        "LIBPTU_MODE", "LIBPTU_OUTPUT", "LIBPTU_MANIFEST",
        "LIBPTU_SHM_ID", "LIBPTU_BUILD_ON_EXIT", NULL
    };
    /* count how many we need to inject */
    int n_inject = 0;
    const char *inject_vals[8] = {NULL};
    for (int k = 0; libptu_vars[k]; k++) {
        char *val = getenv(libptu_vars[k]);
        if (!val) continue;
        /* check if already present in envp */
        int present = 0;
        size_t klen = strlen(libptu_vars[k]);
        for (int i = 0; envp[i]; i++) {
            if (strncmp(envp[i], libptu_vars[k], klen) == 0 && envp[i][klen] == '=') {
                present = 1; break;
            }
        }
        if (!present) inject_vals[n_inject++] = libptu_vars[k];
    }

    int n = 0;
    while (envp[n]) n++;

    char **new_envp = malloc((size_t)(n + 3 + n_inject) * sizeof(char *));
    if (!new_envp) return (char **)envp;

    /* Build LD_PRELOAD entry */
    char preload_entry[PATH_MAX + 16];
    snprintf(preload_entry, sizeof(preload_entry), "LD_PRELOAD=%s", our_path);

    int j = 0;
    int has_preload = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
            /* Prepend our library to existing LD_PRELOAD */
            char combined[PATH_MAX * 2];
            snprintf(combined, sizeof(combined), "LD_PRELOAD=%s:%s",
                     our_path, envp[i] + 11);
            new_envp[j++] = strdup(combined);
            has_preload = 1;
        } else {
            new_envp[j++] = envp[i];
        }
    }
    if (!has_preload) new_envp[j++] = strdup(preload_entry);

    /* Inject missing LIBPTU_* session vars */
    for (int k = 0; k < n_inject; k++) {
        char *val = getenv(inject_vals[k]);
        if (val) {
            char entry[PATH_MAX + 64];
            snprintf(entry, sizeof(entry), "%s=%s", inject_vals[k], val);
            new_envp[j++] = strdup(entry);
        }
    }

    new_envp[j] = NULL;
    return new_envp;
}

/* ── dlopen / dlclose ──────────────────────────────────────────────────── */
/*
 * Python's import system loads .so extension modules via dlopen().
 * ld-linux's internal loader uses private, non-interposable file opens, so
 * our open()/openat() hooks never see these .so files.
 *
 * Fix: after each successful dlopen, use dl_iterate_phdr to walk the
 * in-memory link map (O(loaded libs), no syscall) and record any new libs.
 * Dedup table prevents re-recording already-seen paths.
 *
 * Symlink problem: dlopen("libblas.so.3") asks ld-linux to resolve the name
 * via LD_LIBRARY_PATH. ld-linux opens the real file internally (bypassing our
 * hook) after following the symlink. Our hook sees only the real .so; the
 * symlink "libblas.so.3" is never recorded. At replay, dlopen("libblas.so.3")
 * fails because the symlink is missing from the container.
 *
 * Fix: probe_ldpath_symlink() searches LD_LIBRARY_PATH for the bare library
 * name before real_dlopen runs. When found it calls handle_path(), which calls
 * manifest_record_abs() — this records the symlink as an L entry and recurses
 * through the full chain until it reaches the real file. O(LD_LIBRARY_PATH
 * dirs) per dlopen call, same work ld-linux does anyway.
 */
/*
 * After real_dlopen("libblas.so.3") succeeds, use dlinfo(RTLD_DI_LINKMAP)
 * to find the directory where the real .so landed (resolved via RPATH,
 * LD_LIBRARY_PATH, or ldconfig — any mechanism). Then check whether
 * dir/libname is a symlink and record it via handle_path().
 *
 * This is the correct generic fix: it does not rely on LD_LIBRARY_PATH
 * (which conda envs do not set inside containers) and works for any
 * resolution mechanism the linker uses.
 */
static void probe_resolved_symlink(void *handle, const char *libname)
{
    struct link_map *lm = NULL;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &lm) != 0 || !lm || !lm->l_name || !*lm->l_name)
        return;

    /* lm->l_name = "/path/to/libopenblasp-r0.3.33.so" — the real file.
     * Strip filename to get the containing directory. */
    char dir[PATH_MAX];
    strncpy(dir, lm->l_name, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0'; /* dir = "/shared/miniconda3/envs/ctrend/lib" */

    char sympath[PATH_MAX];
    snprintf(sympath, sizeof(sympath), "%s/%s", dir, libname);

    struct stat st;
    if (syscall(SYS_newfstatat, AT_FDCWD, sympath, &st, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISLNK(st.st_mode)) {
        /* manifest_record_abs records this as an L entry and recurses through
         * the full symlink chain to the real file — handles nested links. */
        handle_path(sympath);
    }
}

void *dlopen(const char *filename, int flags)
{
    /* Use real_dlopen — do NOT set tl_in_hook here since the loaded library's
     * own constructors may legitimately call open() and we want to audit them. */
    if (!real_dlopen) return NULL;

    void *handle = real_dlopen(filename, flags);
    if (handle && g_mode != MODE_DISABLED && !tl_in_hook) {
        tl_in_hook = 1;
        if (filename && strchr(filename, '/') == NULL) {
            /* Bare name: find the symlink in the actual load directory.
             * Must run after real_dlopen so dlinfo(RTLD_DI_LINKMAP) works. */
            probe_resolved_symlink(handle, filename);
        } else if (filename) {
            /* Absolute/relative path: resolve and record directly. */
            char abs_buf[PATH_MAX];
            const char *abs = resolve_abs(filename, abs_buf);
            if (abs) handle_path(abs);
        }
        /* Walk link map to catch transitively loaded libraries */
        scan_loaded_libs();
        tl_in_hook = 0;
    }
    return handle;
}

int dlclose(void *handle)
{
    libptu_lazy_init();
    return real_dlclose ? real_dlclose(handle) : 0;
}

/* ── fork ──────────────────────────────────────────────────────────── */
/*
 * fork() hook — child process attaches to the parent's session.
 *
 * After fork():
 *   - g_session pointer is valid in child (shared memory inherited).
 *   - g_manifest_fp (FILE*) has a COPY of the parent's stdio state —
 *     both would write to the same fd. We flush the parent's buffer
 *     before forking, then give the child its own fd + FILE* in append mode.
 *   - The per-process g_dedup[] is a fresh COW copy — correct behaviour,
 *     the child starts with per-process dedup reflecting parent's state.
 */
pid_t fork(void)
{
    if (!real_fork) real_fork = dlsym(RTLD_NEXT, "fork");
    if (!real_fork) return (errno = ENOSYS, -1);

    if (g_mode != MODE_DISABLED && g_manifest_fp) {
        /* Flush parent buffer so child's fresh FILE* doesn't duplicate it */
        pthread_mutex_lock(&g_manifest_mutex);
        fflush(g_manifest_fp);
        pthread_mutex_unlock(&g_manifest_mutex);
    }

    pid_t pid = real_fork();

    if (pid == 0 && g_mode != MODE_DISABLED) {
        /* ── Child process ── */
        tl_in_hook = 0;

        /* Open a fresh fd + FILE* in append mode.
         * fclose() the inherited FILE* first — buffer is empty (we flushed),
         * so this only closes the fd without writing any data. */
        if (g_manifest_path[0]) {
            if (g_manifest_fp) {
                fclose(g_manifest_fp);
                g_manifest_fp = NULL;
                g_manifest_fd = -1;
            }
            int new_fd = open(g_manifest_path,
                              O_WRONLY | O_APPEND | O_CREAT, 0644);
            if (new_fd >= 0) {
                g_manifest_fd = new_fd;
                g_manifest_fp = fdopen(new_fd, "a");
                /* Fork children exit via os._exit() which bypasses the shared-library
                 * destructor and stdio flush.  Use unbuffered I/O so every fprintf()
                 * goes directly to the kernel — no buffered entries are lost. */
                if (g_manifest_fp)
                    setvbuf(g_manifest_fp, NULL, _IONBF, 0);
            }
        }
        /* g_session is already attached (inherited from parent) — no reattach */
    }

    return pid;
}

/* ── Public launcher API ───────────────────────────────────────────── */

/* Re-run proc/self/maps scan — call after setlocale() in launcher to
 * pick up locale/gconv files that glibc mmap'd during locale init. */
void libptu_scan_maps(void)
{
    if (g_mode == MODE_LOG || g_mode == MODE_CAPTURE)
        scan_proc_maps_init();
}

/* Record an arbitrary path into the manifest (used by launcher for
 * pre-recording ld.so.cache, OS symlinks, timezone file, etc.). */
void libptu_record(const char *path)
{
    if (path && (g_mode == MODE_LOG || g_mode == MODE_CAPTURE))
        manifest_record_abs(path);
}

/* Flush manifest buffer to disk — MUST be called by the launcher before
 * execvp() because exec() does not flush stdio buffers. */
void libptu_flush(void)
{
    pthread_mutex_lock(&g_manifest_mutex);
    if (g_manifest_fp) fflush(g_manifest_fp);
    pthread_mutex_unlock(&g_manifest_mutex);
}

/* ── system() / popen() ────────────────────────────────────────────────── */
/*
 * glibc's system() calls execl("/bin/sh", ...) via INLINE_SYSCALL — a direct
 * kernel call that bypasses PLT interposition entirely.  Our execve hook never
 * fires for the /bin/sh exec inside system().  system() itself IS a public PLT
 * symbol, so we hook it here and record /bin/sh before delegating.
 * manifest_record_abs follows the /bin/sh symlink to the real binary (e.g. dash).
 *
 * popen() has the same internal structure (fork + execl("/bin/sh", ...)).
 */
static int   (*real_system)(const char *)               = NULL;
static FILE *(*real_popen) (const char *, const char *) = NULL;

int system(const char *command)
{
    if (!real_system) real_system = dlsym(RTLD_NEXT, "system");
    if (!real_system) return -1;
    if (command && g_mode != MODE_DISABLED && !tl_in_hook) {
        tl_in_hook = 1;
        handle_path("/bin/sh");
        tl_in_hook = 0;
    }
    return real_system(command);
}

FILE *popen(const char *command, const char *type)
{
    if (!real_popen) real_popen = dlsym(RTLD_NEXT, "popen");
    if (!real_popen) return NULL;
    if (command && g_mode != MODE_DISABLED && !tl_in_hook) {
        tl_in_hook = 1;
        handle_path("/bin/sh");
        tl_in_hook = 0;
    }
    return real_popen(command, type);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    HOOK_PREAMBLE(real_execve(path, argv, envp));

    char abs_buf[PATH_MAX];
    const char *abs = resolve_abs(path, abs_buf);

    if (abs && g_mode != MODE_REPLAY) {
        handle_path(abs);
        capture_elf_interp(abs);
    }

    /* Flush manifest before exec — exec replaces the process image without
     * flushing stdio, so any buffered entries recorded above would be lost. */
    pthread_mutex_lock(&g_manifest_mutex);
    if (g_manifest_fp) fflush(g_manifest_fp);
    pthread_mutex_unlock(&g_manifest_mutex);

    char **new_envp = inject_ld_preload(envp);
    tl_in_hook = 0; /* allow child hooks after exec */
    int r = real_execve(path, argv, new_envp ? new_envp : envp);
    /* Only reached on failure */
    if (new_envp && new_envp != (char **)envp) free(new_envp);
    tl_in_hook = 1;
    HOOK_RETURN(r);
}

int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags)
{
    HOOK_PREAMBLE(real_execveat(dirfd, path, argv, envp, flags));

    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    if (abs && g_mode != MODE_REPLAY) {
        handle_path(abs);
        capture_elf_interp(abs);
    }

    pthread_mutex_lock(&g_manifest_mutex);
    if (g_manifest_fp) fflush(g_manifest_fp);
    pthread_mutex_unlock(&g_manifest_mutex);

    char **new_envp = inject_ld_preload(envp);
    tl_in_hook = 0;
    int r = real_execveat(dirfd, path, argv, new_envp ? new_envp : envp, flags);
    if (new_envp && new_envp != (char **)envp) free(new_envp);
    tl_in_hook = 1;
    HOOK_RETURN(r);
}
