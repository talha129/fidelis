# libptu Manifest Gaps — Missing Entries vs PTU

Comparison of libptu log mode manifest against ptu-logonly manifest on
identical Python+numpy matmul task (500×500, same conda env).

## Summary

| Metric | Value |
|---|---|
| libptu manifest entries | 203 |
| ptu-logonly manifest entries | 438 |
| Overlap (in both) | 188 |
| Missing from libptu | 250 |
| Extra in libptu (not in PTU) | 15 |

---

## Gap 1 — Directory entries not recorded (~180 entries)

**Category**: `D` type entries (directories)

**Example missing entries**:
```
/bin
/etc
/lib
/lib32
/lib64
/libx32
/sbin
/shared
/shared/miniconda3
/shared/miniconda3/envs
/shared/miniconda3/envs/ptu_worker_env
/shared/miniconda3/envs/ptu_worker_env/bin
/shared/miniconda3/envs/ptu_worker_env/lib
...
```

**Root cause**: libptu only records paths for files it intercepts via `open()`,
`stat()`, `readlink()`, etc. It never records the **parent directories** of those
files. PTU calls `copy_all_ancestor_dirs()` on every captured path, recording
a `D` entry for each component of the path that is a directory.

**Impact**:
- `materialize_libptu.py` calls `os.makedirs()` which works around this for
  materialization, but parent directory **permissions are lost** (mode bits not recorded).
- Replay mode cannot reconstruct the directory tree with correct ownership/permissions.
- Total package is incomplete relative to PTU's output.

**Fix**: In `manifest_record_abs()`, after recording the file entry, walk
each ancestor path component and emit a `D` entry for any directory not already
in the dedup table:
```c
static void record_ancestors(const char *abspath) {
    char buf[PATH_MAX];
    strncpy(buf, abspath, PATH_MAX - 1);
    char *p = strrchr(buf, '/');
    while (p && p != buf) {
        *p = '\0';
        // stat buf → if DIR, manifest_record_abs(buf)
        p = strrchr(buf, '/');
    }
}
```

---

## Gap 2 — glibc startup files not captured (~5 entries)

**Category**: System config files opened by glibc before libptu constructor runs

**Missing entries**:
```
/etc/ld.so.cache
/etc/locale.alias
/etc/localtime
/usr/lib/locale/locale-archive
/usr/lib/x86_64-linux-gnu/gconv/gconv-modules.cache
/usr/share/locale/locale.alias
/usr/share/zoneinfo/Etc/UTC
/usr/lib/locale/C.utf8/LC_CTYPE
/shared/miniconda3/envs/ptu_worker_env/ssl/openssl.cnf
```

**Root cause**: glibc opens these files during its own initialization phase
(before `__attribute__((constructor))` fires). Although `libptu_lazy_init()`
ensures `real_*` pointers are valid for any hook that fires pre-constructor,
glibc's internal opens bypass the PLT entirely — they go through glibc's
private `__open` / `__open64` internal symbols, not the interposable `open`
export. LD_PRELOAD cannot intercept these.

**Impact**: A container replayed from libptu manifest on a bare machine will
fail if it lacks locale data, timezone info, or `ld.so.cache`. For replay on
the same machine (same OS install), these files exist on the host and libptu's
fallthrough behavior handles them. For hermetic cross-machine replay, they are
missing.

**Fix**: Enumerate and explicitly record known glibc startup paths in
`libptu_init()` after the manifest is opened:
```c
static const char *glibc_startup_paths[] = {
    "/etc/ld.so.cache",
    "/etc/locale.alias",
    "/etc/localtime",
    "/usr/lib/locale/locale-archive",
    "/usr/share/locale/locale.alias",
    NULL
};
for (int i = 0; glibc_startup_paths[i]; i++)
    manifest_record_abs(glibc_startup_paths[i]);
```

---

## Gap 3 — Top-level OS symlinks not captured (~8 entries)

**Category**: Filesystem-level symlinks at `/bin`, `/lib`, `/lib64`, `/sbin`

**Missing entries**:
```
L 000777 /bin  -> usr/bin
L 000777 /lib  -> usr/lib
L 000777 /lib32 -> usr/lib32
L 000777 /lib64 -> usr/lib64
L 000777 /libx32 -> usr/libx32
L 000777 /sbin -> usr/sbin
L 000777 /usr/lib64 -> lib
```

**Root cause**: On modern Ubuntu (22.04+), `/bin`, `/lib`, `/lib64`, `/sbin`
are symlinks to their `/usr/` counterparts. ld-linux resolves these during its
own startup before any hook can fire. When libptu records a path like
`/lib/x86_64-linux-gnu/libc.so.6`, it logs the canonical resolved path but
never logs the symlink `/lib → usr/lib` itself. PTU intercepts `lstat()` on
these at the syscall level via ptrace and records them.

**Impact**: On a bare machine where `/bin` etc. are real directories (not
symlinks), a replayed container built from libptu manifest will work correctly
because paths are already canonical. However the manifest is structurally
incomplete compared to PTU's output.

**Fix 1** (correct): After `scan_loaded_libs()` in the constructor, for each
recorded path walk the path components and call `lstat()` on each; if a
component is a symlink, emit an `L` entry.

**Fix 2** (pragmatic): Record known Ubuntu OS-level symlinks statically, same
approach as Gap 2.

---

## Gap 4 — Python `.py` source files not captured (~50 entries)

**Category**: Python stdlib and numpy source files

**Missing entries** (sample):
```
/shared/miniconda3/envs/ptu_worker_env/lib/python3.9/abc.py
/shared/miniconda3/envs/ptu_worker_env/lib/python3.9/os.py
/shared/miniconda3/envs/ptu_worker_env/lib/python3.9/site.py
/shared/miniconda3/envs/ptu_worker_env/lib/python3.9/codecs.py
... (and ~45 more stdlib + numpy .py files)
```

**Root cause**: Python 3.9 on Linux 4.11+ uses the `statx()` syscall (via
glibc 2.28+) to check whether a `.py` source file is newer than its `.pyc`
cache. libptu intercepts `stat()` and `lstat()` but does NOT intercept
`statx()`. Therefore, the `os.stat()` calls from Python's importlib that check
`.py` file freshness are invisible to libptu.

libptu captures `.pyc` files (what Python actually `open()`s and reads).
PTU captures `.py` files (what Python `statx()`s for cache validation).
Both sets refer to the same Python modules — the difference is which half of
the import dance each tool sees.

**Functional impact**: A libptu container used for replay on the **same
machine** works correctly because `.pyc` files are present and valid. A
container used on a machine **without matching `.pyc` files** would need the
`.py` sources to recompile — libptu's container would be missing them.

**Fix**: Add a `statx` hook via direct syscall interception:
```c
#include <linux/stat.h>    // struct statx
#include <sys/syscall.h>   // SYS_statx

int statx(int dirfd, const char *path, int flags,
          unsigned int mask, struct statx *buf) {
    HOOK_PREAMBLE(syscall(SYS_statx, dirfd, path, flags, mask, buf));
    char abs_buf[PATH_MAX];
    const char *abs = resolve_at(dirfd, path, abs_buf);
    int r = (int)syscall(SYS_statx, dirfd, path, flags, mask, buf);
    if (abs && g_mode != MODE_REPLAY) handle_path(abs);
    HOOK_RETURN(r);
}
```

---

## Extra in libptu (not in PTU) — 15 entries

These are paths present in libptu manifest but absent from ptu-logonly. Not
gaps — explanations:

### libptu.so itself
```
/home/ubuntu/libptu/libptu.so
```
libptu appears in `/proc/self/maps` and is recorded by `scan_loaded_libs()`.
PTU doesn't capture its own binary.
**Action**: Filter out libptu.so path from manifest (it is not needed for replay).

### Unresolved relative `.so` paths from dl_iterate_phdr
```
/shared/miniconda3/.../lib-dynload/../../libcrypto.so.3
/shared/miniconda3/.../lib-dynload/../../libffi.so.8
/shared/miniconda3/.../lib-dynload/../../libz.so.1
/shared/miniconda3/.../numpy/core/../../../../libopenblas.so.0
/shared/miniconda3/.../numpy/core/../../../../././libgcc_s.so.1
/shared/miniconda3/.../numpy/core/../../../../././libquadmath.so.0
/shared/miniconda3/.../numpy/core/../../../.././libgfortran.so.5
```
`dl_iterate_phdr` returns `dlpi_name` as the raw RPATH-relative string from the
ELF link map. These are the same physical files already captured under their
canonical names (e.g. `/shared/.../libcrypto.so.3`). PTU resolves all paths via
`realpath()` before logging; libptu logs raw paths from `dl_iterate_phdr`.
**Action**: Apply `realpath()` to paths from `dl_iterate_phdr` before recording.

### Worker task script
```
/tmp/worker-1000-69375/task.16/matmul_worker.py
```
The task script staged by TaskVine — present in libptu manifest because Python
`open()`s it to execute it. Absent from ptu-logonly manifest because the task
sandbox is cleaned up between benchmark runs and the paths differ by task ID.
**Action**: None — expected and correct behavior.

---

## Priority Order for Fixes

| Priority | Gap | Effort | Impact |
|---|---|---|---|
| 1 | Gap 1: Directory entries | Medium | Correct directory tree + permissions in replay |
| 2 | Gap 4: `statx` hook | Low | Captures `.py` source files for hermetic replay |
| 3 | Gap 5: `realpath` in dl_iterate_phdr | Low | Eliminates duplicate paths |
| 4 | Gap 2: glibc startup files | Low | Hermetic cross-machine replay |
| 5 | Gap 3: OS symlinks | Medium | Complete structural parity with PTU |
| 6 | Filter libptu.so from manifest | Trivial | Cleaner manifest |
