/*
 * libptu-launcher.c — pre-recording launcher for the libptu hybrid approach.
 *
 * *** libptu-sync VARIANT *** — defaults LIBPTU_MODE to "capture" (synchronous
 * copy-on-touch into cde-root) instead of "log". Set LIBPTU_MODE yourself
 * before invoking this launcher to override (e.g. LIBPTU_MODE=log for the
 * old async-materialize behavior) — it is only defaulted here, not forced.
 *
 * Usage:
 *   libptu-launcher --output <dir> [--libptu <path>] <command> [args...]
 *
 * What it does before exec'ing the target command:
 *   1. Sets LIBPTU_MODE=capture (unless already set), LIBPTU_OUTPUT=<dir>,
 *      LD_PRELOAD=<libptu.so>
 *   2. dlopen's libptu.so — constructor fires, creates shmem session,
 *      opens manifest, scans loaded libs.
 *   3. Pre-records files that the target's ld-linux opens before LD_PRELOAD
 *      hooks can fire:
 *        - /etc/ld.so.cache        (explicit open)
 *        - OS symlinks /bin /lib32 /lib64 /libx32 /sbin  (lstat)
 *        - /etc/localtime + zone file  (readlink + stat)
 *        - locale/gconv files       (setlocale + scan_proc_maps)
 *        - /usr/share/locale/locale.alias (open if present)
 *   4. exec's the target command with LIBPTU_SHM_ID + LIBPTU_MANIFEST
 *      already in the environment so all fork'd/exec'd children attach
 *      to the same session automatically.
 *
 * All pre-recording goes through libptu's normal hooks — no hardcoded
 * path lists.  The launcher OPENS these files so hooks capture them.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <locale.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <limits.h>
#include <errno.h>
#include <getopt.h>

/* ── Resolve libptu.so path ────────────────────────────────────────── */

static void find_libptu_default(char *out, size_t sz)
{
    /* Try next to the launcher binary via /proc/self/exe */
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, sz, "%s/libptu.so", exe);
            if (access(out, R_OK) == 0) return;
            /* try parent dir */
            slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                snprintf(out, sz, "%s/libptu.so", exe);
                if (access(out, R_OK) == 0) return;
            }
        }
    }
    snprintf(out, sz, "libptu.so"); /* fallback: dynamic loader search */
}

/* ── Pre-record a path via the libptu API ──────────────────────────── */

typedef void (*libptu_record_fn)(const char *);
typedef void (*libptu_scan_fn)(void);
typedef void (*libptu_flush_fn)(void);
typedef void (*libptu_build_fn)(void);

static libptu_record_fn g_record = NULL;
static libptu_scan_fn   g_scan   = NULL;
static libptu_flush_fn  g_flush  = NULL;
static libptu_build_fn  g_build  = NULL;

static void probe_open(const char *path)
{
    /* Open the file — libptu's open() hook captures it.
     * Fallback: call libptu_record() directly in case the hook missed it. */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) close(fd);
    if (g_record) g_record(path);
}

static void probe_lstat(const char *path)
{
    struct stat st;
    lstat(path, &st); /* lstat hook captures symlinks + dirs */
    if (g_record) g_record(path);
}

/* ── Pre-record all files that open before LD_PRELOAD fires ────────── */

static void prerecord(void)
{
    /* apptainer exec uses /bin/sh inside the container to set up the execution
     * environment before running the target command.  Processes that never spawn
     * a shell (e.g. pure-Python managers) won't open /bin/sh themselves, so we
     * always capture it here.  probe_lstat records the symlink; probe_open
     * follows it and captures the real binary (/bin/dash on Ubuntu). */
    probe_lstat("/bin/sh");
    probe_open("/bin/sh");

    /* /bin is a symlink (/bin → usr/bin) on Ubuntu 20+.  Record the symlink so
     * the container path structure is intact for path resolution. */
    probe_lstat("/bin");

    /* ld-linux opens /etc/ld.so.cache at startup — capture it explicitly. */
    probe_open("/etc/ld.so.cache");

    /* OS compat symlinks: /lib32, /lib64, /libx32, /sbin may be symlinks on
     * modern Ubuntu.  Capture so the container path structure is correct. */
    probe_lstat("/lib32");
    probe_lstat("/lib64");
    probe_lstat("/libx32");
    probe_lstat("/sbin");

    /* /etc/localtime and the actual zone file it points to. */
    {
        char zone[PATH_MAX];
        probe_open("/etc/localtime");
        ssize_t n = readlink("/etc/localtime", zone, sizeof(zone) - 1);
        if (n > 0) { zone[n] = '\0'; probe_open(zone); }
    }

    /* glibc opens locale-archive via __open_nocancel — an internal glibc call
     * that bypasses PLT hooks.  The fd is never tracked, so our mmap() hook
     * cannot resolve the path and the file is silently missed.  Probing it here
     * from the launcher (where our PLT open() hook IS active) forces it into the
     * manifest so containers get the locale database and Python site.py does not
     * fail with "LookupError: unknown encoding: ANSI_X3.4-1968". */
    probe_open("/usr/lib/locale/locale-archive");
    probe_lstat("/usr/lib/locale");

    /* locale.alias is read by setlocale() on some systems. */
    probe_open("/usr/share/locale/locale.alias");

    /* Force setlocale() so glibc loads locale data and scan_proc_maps catches
     * any remaining locale/gconv mmap'd files. */
    setlocale(LC_ALL, "");
    if (g_scan) g_scan();
}

/* ─────────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --output <dir> [--libptu <path>] <command> [args...]\n"
        "\n"
        "  --output  <dir>   Directory for cde.manifest and cde-root\n"
        "  --libptu  <path>  Path to libptu.so (default: auto-detect)\n"
        "\n"
        "Example:\n"
        "  %s --output /tmp/audit vine_worker localhost 9123 --cores 4\n",
        prog, prog);
}

int main(int argc, char *argv[])
{
    char libptu_path[PATH_MAX] = "";
    char output_dir[PATH_MAX]  = "";   /* final (NFS) output — SIF copied here */
    char local_dir[PATH_MAX]   = "";   /* local /tmp capture dir — manifest stays local */

    /* ── Parse arguments ── */
    static struct option opts[] = {
        {"output", required_argument, 0, 'o'},
        {"libptu", required_argument, 0, 'l'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c, idx = 0;
    while ((c = getopt_long(argc, argv, "+o:l:h", opts, &idx)) != -1) {
        switch (c) {
        case 'o': strncpy(output_dir,  optarg, PATH_MAX - 1); break;
        case 'l': strncpy(libptu_path, optarg, PATH_MAX - 1); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!output_dir[0]) {
        fprintf(stderr, "ERROR: --output required\n");
        usage(argv[0]);
        return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "ERROR: no command specified\n");
        usage(argv[0]);
        return 1;
    }

    /* ── Resolve libptu.so path ── */
    if (!libptu_path[0])
        find_libptu_default(libptu_path, sizeof(libptu_path));

    if (access(libptu_path, R_OK) != 0) {
        fprintf(stderr, "ERROR: libptu.so not found at %s\n", libptu_path);
        return 1;
    }

    /* ── Clean up any stale session from a previous run ─────────────────
     * If LIBPTU_SHM_ID is in the environment (from a parent shell or prior
     * run), it points to an old/alien shmem segment.  Detach + invalidate it
     * so the subprocess children don't accidentally attach to stale data. */
    const char *old_shmid = getenv("LIBPTU_SHM_ID");
    if (old_shmid) {
        int old_id = atoi(old_shmid);
        shmctl(old_id, IPC_RMID, NULL); /* best-effort cleanup of prior session */
        unsetenv("LIBPTU_SHM_ID");
        unsetenv("LIBPTU_MANIFEST");
    }

    /* ── Create local capture dir in /tmp — manifest flocks stay off NFS ── */
    snprintf(local_dir, sizeof(local_dir), "/tmp/libptu-capture-%d", (int)getpid());
    if (mkdir(local_dir, 0755) != 0) {
        fprintf(stderr, "ERROR: cannot create local capture dir %s: %s\n",
                local_dir, strerror(errno));
        return 1;
    }

    /* ── Ensure final NFS output dir exists (SIF copied here after build) ── */
    {
        char tmp[PATH_MAX];
        strncpy(tmp, output_dir, PATH_MAX - 1);
        tmp[PATH_MAX - 1] = '\0';
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "ERROR: cannot create output dir %s: %s\n",
                    output_dir, strerror(errno));
            return 1;
        }
    }

    /* ── Set env vars BEFORE dlopen so libptu constructor initializes ──
     * overwrite=0 on LIBPTU_MODE: default to "capture" for this variant, but
     * respect an already-exported LIBPTU_MODE (e.g. LIBPTU_MODE=log) if the
     * caller set one. */
    setenv("LIBPTU_MODE",   "capture",   0);
    setenv("LIBPTU_OUTPUT",  local_dir,   1); /* local — manifest flocks stay off NFS */
    /* LD_PRELOAD for the exec'd target (not this process — already running) */
    setenv("LD_PRELOAD",     libptu_path, 1);

    /* ── Load libptu.so — constructor fires, creates shmem session ── */
    void *handle = dlopen(libptu_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "ERROR: dlopen(%s): %s\n", libptu_path, dlerror());
        return 1;
    }

    /* Get public API functions */
    g_record = (libptu_record_fn)dlsym(handle, "libptu_record");
    g_scan   = (libptu_scan_fn)  dlsym(handle, "libptu_scan_maps");
    g_flush  = (libptu_flush_fn) dlsym(handle, "libptu_flush");
    g_build  = (libptu_build_fn) dlsym(handle, "libptu_build_env");

    /* ── Pre-record files that would be missed in the target ── */
    prerecord();

    /* ── LIBPTU_SHM_ID + LIBPTU_MANIFEST already set by libptu constructor ──
     * inject_ld_preload() in the execve hook copies all env vars including
     * these, so every exec'd child automatically attaches to the session. */

    /* ── Flush manifest before exec — exec() does NOT flush stdio buffers ── */
    if (g_flush) g_flush();

    fprintf(stderr,
            "[libptu-launcher] session ready → manifest: %s/cde.manifest\n",
            output_dir);

    /* ── Fork + exec the target; parent waits so its destructor runs with
     *    g_shmid >= 0 and can invoke _build_on_exit() after child exits. ── */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* Child: restore default signal handlers then exec */
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execvp(argv[optind], argv + optind);
        perror("execvp");
        _exit(1);
    }

    /* Parent: ignore SIGINT/SIGTERM so Ctrl+C kills only the child (via
     * process group) but parent survives to run the libptu destructor. */
    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    /* Flush manifest + write env.txt via libptu API before copying */
    const char *do_build = getenv("LIBPTU_BUILD_ON_EXIT");
    if (g_flush) g_flush();
    if (g_build) g_build();   /* writes env.txt; C materialize + SIF build handled below */

    /* Old C-build path (kept for reference — now handled by Python script below):
    if (do_build && do_build[0] == '1') {
        char src_sif[PATH_MAX], dst_sif[PATH_MAX];
        snprintf(src_sif, sizeof(src_sif), "%s/vine-worker.sif", local_dir);
        snprintf(dst_sif, sizeof(dst_sif), "%s/vine-worker.sif", output_dir);
        if (rename(src_sif, dst_sif) != 0) {
            char cp_cmd[PATH_MAX * 2 + 16];
            snprintf(cp_cmd, sizeof(cp_cmd), "cp %s %s", src_sif, dst_sif);
            system(cp_cmd);
        }
        fprintf(stderr, "[libptu-launcher] SIF → %s\n", dst_sif);
    }
    */

    /* Copy manifest + env.txt to output_dir for inspection / SIF build */
    {
        char src_mf[PATH_MAX], dst_mf[PATH_MAX];
        snprintf(src_mf, sizeof(src_mf), "%s/cde.manifest", local_dir);
        snprintf(dst_mf, sizeof(dst_mf), "%s/cde.manifest", output_dir);
        if (rename(src_mf, dst_mf) != 0) {
            char cp_cmd[PATH_MAX * 2 + 16];
            snprintf(cp_cmd, sizeof(cp_cmd), "cp %s %s", src_mf, dst_mf);
            system(cp_cmd);
        }

        char src_env[PATH_MAX], dst_env[PATH_MAX];
        snprintf(src_env, sizeof(src_env), "%s/env.txt", local_dir);
        snprintf(dst_env, sizeof(dst_env), "%s/env.txt", output_dir);
        if (rename(src_env, dst_env) != 0) {
            char cp_cmd[PATH_MAX * 2 + 16];
            snprintf(cp_cmd, sizeof(cp_cmd), "cp %s %s", src_env, dst_env);
            system(cp_cmd);
        }
    }

    /* Materialize + build SIF via libptu-materialize C binary (no interpreter overhead).
     * Old Python path kept below for reference. */
    if (do_build && do_build[0] == '1') {
        /* Derive libptu-materialize path from libptu.so path */
        char bin_path[PATH_MAX];
        strncpy(bin_path, libptu_path, PATH_MAX - 1);
        bin_path[PATH_MAX - 1] = '\0';
        char *slash = strrchr(bin_path, '/');
        if (slash) {
            snprintf(slash + 1, PATH_MAX - (size_t)(slash + 1 - bin_path),
                     "libptu-materialize");
        } else {
            snprintf(bin_path, sizeof(bin_path), "libptu-materialize");
        }

        char sif_out[PATH_MAX];
        snprintf(sif_out, sizeof(sif_out), "%s/audit.sif", output_dir);

        pid_t pid = fork();
        if (pid == 0) {
            unsetenv("LD_PRELOAD");
            unsetenv("LIBPTU_MODE");
            unsetenv("LIBPTU_OUTPUT");
            unsetenv("LIBPTU_SHM_ID");
            unsetenv("LIBPTU_MANIFEST");
            unsetenv("LIBPTU_BUILD_ON_EXIT");
            execl(bin_path, bin_path, "--build-sif", sif_out, output_dir, (char *)NULL);
            _exit(1);
        }
        if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (rc == 0)
                fprintf(stderr, "[libptu-launcher] SIF → %s\n", sif_out);
            else
                fprintf(stderr, "[libptu-launcher] WARNING: SIF build failed (rc=%d)\n", rc);
        }

        /* Old Python path (kept for reference):
        char script_path[PATH_MAX];
        strncpy(script_path, libptu_path, PATH_MAX - 1);
        script_path[PATH_MAX - 1] = '\0';
        char *pslash = strrchr(script_path, '/');
        if (pslash) {
            snprintf(pslash + 1, PATH_MAX - (size_t)(pslash + 1 - script_path),
                     "materialize_libptu.py");
        } else {
            snprintf(script_path, sizeof(script_path), "materialize_libptu.py");
        }
        char build_cmd[PATH_MAX * 4 + 64];
        snprintf(build_cmd, sizeof(build_cmd),
                 "LD_PRELOAD= python3 %s --build-sif %s %s",
                 script_path, sif_out, output_dir);
        int rc = system(build_cmd);
        */
    }

    /* Clean up local capture dir */
    {
        char rm_cmd[PATH_MAX + 16];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", local_dir);
        system(rm_cmd);
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
