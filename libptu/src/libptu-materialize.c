/*
 * libptu-materialize.c — standalone materializer + SIF builder.
 *
 * Exact logical equivalent of materialize_libptu.py.
 * Has zero dependency on libptu.so — safe to call without LD_PRELOAD.
 *
 * File copy: sendfile() (kernel zero-copy) instead of Python shutil.copy2.
 *
 * Usage:
 *   libptu-materialize [--build-sif <sif_out>] <pkg_dir> [<cde_root>]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/wait.h>
#include <utime.h>
#include <limits.h>
#include <time.h>

/* ── entry structs ─────────────────────────────────────────────────────── */

typedef struct { char path[PATH_MAX]; mode_t mode; int depth; } DirEnt;
typedef struct { char path[PATH_MAX]; mode_t mode;            } FileEnt;
typedef struct { char path[PATH_MAX]; char target[PATH_MAX]; int depth; } LinkEnt;

/* ── helpers ────────────────────────────────────────────────────────────── */

static int count_slashes(const char *p)
{
    int n = 0;
    for (; *p; p++) if (*p == '/') n++;
    return n;
}

/* Create all components of path (like mkdir -p). */
static void mkdirs_p(const char *path, mode_t mode)
{
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

/* Create parent directory of path. */
static void mkdirs_for_parent(const char *path)
{
    char tmp[PATH_MAX];
    strncpy(tmp, path, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';
    char *sl = strrchr(tmp, '/');
    if (sl && sl != tmp) {
        *sl = '\0';
        mkdirs_p(tmp, 0755);
    }
}

/* Copy src → dst using sendfile (kernel zero-copy), preserve timestamps.
 * Mirrors shutil.copy2 semantics. */
static int copy_file(const char *src, const char *dst, mode_t mode)
{
    struct stat st;
    if (stat(src, &st) != 0) return -1;

    /* Remove dst if it exists */
    unlink(dst);

    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;

    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dfd < 0) { close(sfd); return -1; }

    off_t off = 0;
    off_t rem = st.st_size;
    while (rem > 0) {
        ssize_t n = sendfile(dfd, sfd, &off, (size_t)rem);
        if (n <= 0) { close(sfd); close(dfd); return -1; }
        rem -= n;
    }
    close(sfd);
    close(dfd);

    chmod(dst, mode);

    /* Preserve timestamps (shutil.copy2 behavior) */
    struct utimbuf ut;
    ut.actime  = st.st_atime;
    ut.modtime = st.st_mtime;
    utime(dst, &ut);

    return 0;
}

/* Place a symlink at dst with given target.
 * If dst exists as a non-symlink dir: rmdir → rm_rf; else unlink. */
static void rm_rf_path(const char *dir)
{
    pid_t pid = fork();
    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        execlp("rm", "rm", "-rf", "--", dir, (char *)NULL);
        _exit(1);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

static int place_symlink(const char *target, const char *dst)
{
    struct stat st;
    if (lstat(dst, &st) == 0) {
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            /* directory, not a symlink: rmdir first, rm_rf if non-empty */
            if (rmdir(dst) != 0)
                rm_rf_path(dst);
        } else {
            unlink(dst);
        }
    }
    return symlink(target, dst);
}

/* ── virtual filesystem check (exact match or prefix+'/') ──────────────── */

static const char *VIRTUAL_FS[] = { "/proc", "/sys", "/dev", NULL };

static int is_virtual(const char *path)
{
    for (int i = 0; VIRTUAL_FS[i]; i++) {
        size_t l = strlen(VIRTUAL_FS[i]);
        if (strcmp(path, VIRTUAL_FS[i]) == 0) return 1;
        if (strncmp(path, VIRTUAL_FS[i], l) == 0 && path[l] == '/') return 1;
    }
    return 0;
}

/* ── symlink ancestor check ─────────────────────────────────────────────── */
/* Mirrors Python: check each proper prefix component against the symlink set */

static char **g_symlink_paths = NULL;
static int    g_symlink_count = 0;

static int has_symlink_ancestor(const char *path)
{
    /* Walk prefix components: /a, /a/b, /a/b/c (but not the full path itself) */
    char tmp[PATH_MAX];
    strncpy(tmp, path, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';
    char *p = tmp + 1; /* skip leading '/' */
    while (*p) {
        /* advance to next '/' */
        while (*p && *p != '/') p++;
        if (!*p) break; /* reached end — don't check the full path itself */
        char saved = *p;
        *p = '\0';
        for (int i = 0; i < g_symlink_count; i++)
            if (strcmp(g_symlink_paths[i], tmp) == 0) { *p = saved; return 1; }
        *p = saved;
        p++;
    }
    return 0;
}

/* ── sort comparators ───────────────────────────────────────────────────── */

static int cmp_dir_depth(const void *a, const void *b)
{
    return ((DirEnt *)a)->depth - ((DirEnt *)b)->depth;
}
static int cmp_link_depth(const void *a, const void *b)
{
    return ((LinkEnt *)a)->depth - ((LinkEnt *)b)->depth;
}

/* ── env skip logic (mirrors Python _env_line_skip) ────────────────────── */

static const char *ENV_SKIP_PREFIX[] = {
    "LIBPTU_", "LD_PRELOAD=", "BASH_FUNC", "SSH_", "DBUS_",
    "MOTD_", "XDG_SESSION_", "XDG_RUNTIME_",
    /* Captured values wrong at replay — overridden explicitly in write_def */
    "VINE_AUDIT_MODE", "VINE_REPLAY_MODE", "TASKVINE_WARM_POOL",
    NULL
};
static const char *ENV_SKIP_EXACT[] = { "_=", "SHLVL=", "PWD=", NULL };

static int env_skip(const char *line)
{
    if (!line[0] || line[0] == '}') return 1;
    for (int i = 0; ENV_SKIP_PREFIX[i]; i++)
        if (strncmp(line, ENV_SKIP_PREFIX[i], strlen(ENV_SKIP_PREFIX[i])) == 0) return 1;
    for (int i = 0; ENV_SKIP_EXACT[i]; i++)
        if (strncmp(line, ENV_SKIP_EXACT[i], strlen(ENV_SKIP_EXACT[i])) == 0) return 1;
    return 0;
}

/* Write val single-quote-escaped to f (mirrors Python _single_quote_escape). */
static void fwrite_sq_escaped(FILE *f, const char *val)
{
    fputc('\'', f);
    for (const char *p = val; *p; p++) {
        if (*p == '\'') fputs("'\\''", f);
        else fputc(*p, f);
    }
    fputc('\'', f);
}

/* ── write apptainer def ────────────────────────────────────────────────── */

static void write_def(const char *pkg_dir, const char *cde_root, const char *def_path)
{
    FILE *f = fopen(def_path, "w");
    if (!f) { perror("fopen def"); return; }

    fprintf(f, "Bootstrap: localimage\nFrom: %s\n\n%%environment\n", cde_root);

    char env_path[PATH_MAX];
    snprintf(env_path, sizeof(env_path), "%s/env.txt", pkg_dir);
    FILE *ef = fopen(env_path, "r");
    if (ef) {
        char raw[8192];
        while (fgets(raw, sizeof(raw), ef)) {
            size_t l = strlen(raw);
            if (l > 0 && raw[l-1] == '\n') raw[--l] = '\0';
            if (env_skip(raw)) continue;
            char *eq = strchr(raw, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *key = raw;
            const char *val = eq + 1;
            fprintf(f, "    export %s=", key);
            fwrite_sq_escaped(f, val);
            fputc('\n', f);
        }
        fclose(ef);
    }

    /* Inject correct replay-time values regardless of capture-time settings */
    fputs("    export VINE_AUDIT_MODE='0'\n", f);
    fputs("    export VINE_REPLAY_MODE='1'\n", f);
    fputs("    export TASKVINE_WARM_POOL='1'\n", f);

    fputs("\n%runscript\n    exec \"$@\"\n", f);
    fclose(f);
}

/* ── build SIF via fork+exec apptainer ─────────────────────────────────── */

static int build_sif(const char *def_path, const char *sif_out)
{
    pid_t pid = fork();
    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        unsetenv("LIBPTU_MODE");
        unsetenv("LIBPTU_OUTPUT");
        unsetenv("LIBPTU_SHM_ID");
        unsetenv("LIBPTU_MANIFEST");
        unsetenv("LIBPTU_BUILD_ON_EXIT");
        execlp("apptainer", "apptainer", "build", "--force", sif_out, def_path, (char *)NULL);
        _exit(1);
    }
    if (pid < 0) { perror("fork"); return -1; }
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* ── materialize ────────────────────────────────────────────────────────── */

static int materialize(const char *pkg_dir, const char *cde_root)
{
    char mf_path[PATH_MAX];
    snprintf(mf_path, sizeof(mf_path), "%s/cde.manifest", pkg_dir);
    FILE *mf = fopen(mf_path, "r");
    if (!mf) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n", mf_path, strerror(errno));
        return -1;
    }

    /* Dynamic arrays for dirs, files, links */
    int dcap = 4096, dn = 0;
    int fcap = 65536, fn = 0;
    int lcap = 1024,  ln = 0;
    DirEnt  *dirs  = malloc((size_t)dcap * sizeof(*dirs));
    FileEnt *files = malloc((size_t)fcap * sizeof(*files));
    LinkEnt *links = malloc((size_t)lcap * sizeof(*links));

    char raw[PATH_MAX * 2 + 16];
    int lineno = 0;
    while (fgets(raw, sizeof(raw), mf)) {
        lineno++;
        /* strip trailing newline */
        size_t l = strlen(raw);
        while (l > 0 && (raw[l-1] == '\n' || raw[l-1] == '\r')) raw[--l] = '\0';
        if (!l) continue;

        char kind[4], mode_s[16], path[PATH_MAX], target[PATH_MAX];
        target[0] = '\0';
        int np = sscanf(raw, "%3s %15s %4095s %4095s", kind, mode_s, path, target);
        if (np < 3) {
            fprintf(stderr, "WARNING: line %d malformed: %s\n", lineno, raw);
            continue;
        }
        char *end;
        mode_t mode = (mode_t)strtol(mode_s, &end, 8);
        if (*end != '\0') {
            fprintf(stderr, "WARNING: line %d bad mode: %s\n", lineno, raw);
            continue;
        }

        if (strcmp(kind, "D") == 0) {
            if (dn >= dcap) { dcap *= 2; dirs = realloc(dirs, (size_t)dcap * sizeof(*dirs)); }
            strncpy(dirs[dn].path, path, PATH_MAX - 1);
            dirs[dn].mode  = mode;
            dirs[dn].depth = count_slashes(path);
            dn++;
        } else if (strcmp(kind, "F") == 0) {
            if (fn >= fcap) { fcap *= 2; files = realloc(files, (size_t)fcap * sizeof(*files)); }
            strncpy(files[fn].path, path, PATH_MAX - 1);
            files[fn].mode = mode;
            fn++;
        } else if (strcmp(kind, "L") == 0) {
            if (np < 4) {
                fprintf(stderr, "WARNING: line %d L entry missing target: %s\n", lineno, raw);
                continue;
            }
            if (ln >= lcap) { lcap *= 2; links = realloc(links, (size_t)lcap * sizeof(*links)); }
            strncpy(links[ln].path,   path,   PATH_MAX - 1);
            strncpy(links[ln].target, target, PATH_MAX - 1);
            links[ln].depth = count_slashes(path);
            ln++;
        }
    }
    fclose(mf);

    /* Build symlink path set for ancestor checks */
    g_symlink_count = ln;
    g_symlink_paths = malloc((size_t)ln * sizeof(char *));
    for (int i = 0; i < ln; i++) g_symlink_paths[i] = links[i].path;

    /* Sort dirs and links by depth ascending (shallow first) */
    qsort(dirs,  (size_t)dn, sizeof(*dirs),  cmp_dir_depth);
    qsort(links, (size_t)ln, sizeof(*links), cmp_link_depth);

    /* Clear stale cde_root so the SIF reflects exactly this manifest.
     * Without this, files from previous captures accumulate and the SIF
     * appears correct even when the current capture is incomplete. */
    rm_rf_path(cde_root);

    /* Ensure cde_root exists */
    mkdirs_p(cde_root, 0755);

    /* ── Phase 1: directories ────────────────────────────────────────────── */
    for (int i = 0; i < dn; i++) {
        if (is_virtual(dirs[i].path) || has_symlink_ancestor(dirs[i].path)) continue;
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s%s", cde_root, dirs[i].path);
        mkdirs_p(dst, dirs[i].mode);
        chmod(dst, dirs[i].mode); /* chmod after makedirs, ignore errors */
    }

    /* ── Phase 2: symlinks (shallow first) ──────────────────────────────── */
    int lk_ok = 0;
    for (int i = 0; i < ln; i++) {
        if (is_virtual(links[i].path)) continue;
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s%s", cde_root, links[i].path);
        mkdirs_for_parent(dst);
        if (place_symlink(links[i].target, dst) == 0) {
            lk_ok++;
        } else {
            fprintf(stderr, "WARN: symlink %s -> %s: %s\n",
                    dst, links[i].target, strerror(errno));
        }
    }

    /* ── Phase 3: files (manifest order, no sort — mirrors Python) ──────── */
    int ok = 0, miss = 0;
    int so_cap = 256, so_n = 0;
    char (*so_files)[PATH_MAX] = malloc((size_t)so_cap * sizeof(*so_files));

    for (int i = 0; i < fn; i++) {
        if (is_virtual(files[i].path)) continue;
        const char *src = files[i].path;
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s%s", cde_root, src);
        mkdirs_for_parent(dst);

        /* os.path.exists follows symlinks — stat() does too */
        struct stat st_follow;
        if (stat(src, &st_follow) != 0) {
            fprintf(stderr, "MISSING: %s\n", src);
            miss++;
            continue;
        }

        /* os.path.islink checks lstat */
        struct stat st_lstat;
        lstat(src, &st_lstat);
        if (S_ISLNK(st_lstat.st_mode)) {
            /* source is a symlink — copy the link itself */
            char lnk[PATH_MAX];
            ssize_t n = readlink(src, lnk, sizeof(lnk) - 1);
            if (n > 0) {
                lnk[n] = '\0';
                unlink(dst); /* remove dst if exists */
                if (symlink(lnk, dst) != 0)
                    fprintf(stderr, "WARN: symlink %s -> %s: %s\n", dst, lnk, strerror(errno));
            }
        } else {
            if (copy_file(src, dst, files[i].mode) != 0) {
                fprintf(stderr, "ERROR copying %s: %s\n", src, strerror(errno));
                miss++;
                continue;
            }
            ok++;
            if (strstr(files[i].path, ".so") != NULL) {
                if (so_n >= so_cap) {
                    so_cap *= 2;
                    so_files = realloc(so_files, (size_t)so_cap * sizeof(*so_files));
                }
                strncpy(so_files[so_n], files[i].path, PATH_MAX - 1);
                so_files[so_n][PATH_MAX - 1] = '\0';
                so_n++;
            }
        }
    }

    /* ── Phase 4: discover .so symlinks missed by libptu capture ──────────
     * ld-linux resolves DT_NEEDED entries (e.g. "libblas.so.3") internally
     * when loading a real .so, bypassing PLT hooks.  libptu records the real
     * file (libopenblasp-r0.3.33.so) but never sees the symlink name.  At
     * replay, dlopen("libblas.so.3") fails because the symlink is absent.
     *
     * Fix: for each real .so copied above, scan its host directory for
     * symlinks that resolve to the same inode.  Add any missing ones to the
     * cde-root.  One scan per unique directory (scanned_dirs dedup). */
    int lk_so = 0;

    for (int i = 0; i < so_n; i++) {
        char host_dir[PATH_MAX];
        strncpy(host_dir, so_files[i], PATH_MAX - 1);
        host_dir[PATH_MAX - 1] = '\0';
        char *sl = strrchr(host_dir, '/');
        if (!sl) continue;
        *sl = '\0';

        /* Get inode of the real .so file (stat follows symlinks) */
        struct stat real_st;
        if (stat(so_files[i], &real_st) != 0) continue;

        DIR *dirp = opendir(host_dir);
        if (!dirp) continue;

        struct dirent *de;
        while ((de = readdir(dirp)) != NULL) {
            if (strstr(de->d_name, ".so") == NULL) continue;

            char entry_path[PATH_MAX];
            snprintf(entry_path, sizeof(entry_path), "%s/%s", host_dir, de->d_name);

            /* Must be a symlink */
            struct stat entry_lstat;
            if (lstat(entry_path, &entry_lstat) != 0) continue;
            if (!S_ISLNK(entry_lstat.st_mode)) continue;

            /* Follow symlink to real file and compare inode+device */
            struct stat entry_st;
            if (stat(entry_path, &entry_st) != 0) continue;
            if (entry_st.st_ino != real_st.st_ino || entry_st.st_dev != real_st.st_dev) continue;

            /* Skip if already in cde-root */
            char dst[PATH_MAX];
            snprintf(dst, sizeof(dst), "%s%s", cde_root, entry_path);
            struct stat dst_lstat;
            if (lstat(dst, &dst_lstat) == 0) continue;

            char link_target[PATH_MAX];
            ssize_t n = readlink(entry_path, link_target, sizeof(link_target) - 1);
            if (n <= 0) continue;
            link_target[n] = '\0';

            mkdirs_for_parent(dst);
            if (symlink(link_target, dst) == 0) lk_so++;
        }
        closedir(dirp);
    }
    free(so_files);

    /* Compute total size of materialized regular files */
    off_t total_size = 0;
    for (int i = 0; i < fn; i++) {
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s%s", cde_root, files[i].path);
        struct stat st;
        if (stat(dst, &st) == 0 && S_ISREG(st.st_mode))
            total_size += st.st_size;
    }

    printf("Materialized: %d files, %d dirs, %d symlinks, %d so-symlinks\n", ok, dn, lk_ok, lk_so);
    if (miss) printf("Missing from host: %d files\n", miss);
    printf("cde-root: %s\n", cde_root);
    printf("Total size: %.1f MB\n", (double)total_size / 1e6);

    free(dirs); free(files); free(links); free(g_symlink_paths);
    return 0;
}

/* ── entry point ────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--build-sif <sif_out>] <pkg_dir> [<cde_root>]\n"
        "\n"
        "  <pkg_dir>    directory with cde.manifest + env.txt\n"
        "  <cde_root>   destination (default: <pkg_dir>/cde-root)\n"
        "  --build-sif  also write def and build apptainer SIF at <sif_out>\n",
        prog);
}

int main(int argc, char *argv[])
{
    const char *sif_out  = NULL;
    const char *pkg_dir  = NULL;
    const char *cde_root_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--build-sif") == 0 && i + 1 < argc) {
            sif_out = argv[++i];
        } else if (!pkg_dir) {
            pkg_dir = argv[i];
        } else if (!cde_root_arg) {
            cde_root_arg = argv[i];
        }
    }

    if (!pkg_dir) { usage(argv[0]); return 1; }

    char root_buf[PATH_MAX];
    if (!cde_root_arg) {
        snprintf(root_buf, sizeof(root_buf), "%s/cde-root", pkg_dir);
        cde_root_arg = root_buf;
    }

    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (materialize(pkg_dir, cde_root_arg) != 0) return 1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double mat_s = (double)(t1.tv_sec - t0.tv_sec) + 1e-9*(double)(t1.tv_nsec - t0.tv_nsec);
    fprintf(stderr, "[libptu-materialize] materialization: %.2f s\n", mat_s);

    if (sif_out) {
        char tmp_dir[PATH_MAX];
        snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/libptu-sifbuild-%d", (int)getpid());
        mkdir(tmp_dir, 0755);

        char def_path[PATH_MAX];
        snprintf(def_path, sizeof(def_path), "%s/container.def", tmp_dir);

        write_def(pkg_dir, cde_root_arg, def_path);

        fprintf(stderr, "[libptu-materialize] building SIF: %s ...\n", sif_out);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        int rc = build_sif(def_path, sif_out);
        clock_gettime(CLOCK_MONOTONIC, &t3);
        double sif_s = (double)(t3.tv_sec - t2.tv_sec) + 1e-9*(double)(t3.tv_nsec - t2.tv_nsec);
        fprintf(stderr, "[libptu-materialize] SIF build: %.2f s\n", sif_s);

        /* cleanup tmp def dir */
        char rm_cmd[PATH_MAX + 16];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmp_dir);
        system(rm_cmd);

        if (rc != 0) {
            fprintf(stderr, "ERROR: apptainer build failed\n");
            return 1;
        }
        fprintf(stderr, "[libptu-materialize] SIF ready: %s\n", sif_out);
    }

    return 0;
}
