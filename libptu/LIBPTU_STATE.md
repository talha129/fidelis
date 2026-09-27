# libptu — Current State Design Document

LD_PRELOAD shared library for in-process file-access auditing and replay.
Replaces PTU's ptrace-based interception with glibc symbol interposition.

---

## Overview

| Property | Value |
|---|---|
| Source | `libptu/src/libptu.c` + `libptu/include/libptu_internal.h` |
| Build | `make` in `libptu/` → `libptu.so` |
| Platform | Linux x86_64, glibc 2.17+, dynamically linked programs |
| Modes | `log`, `capture` (stub), `replay` |
| Overhead | **<1%** on Python+numpy (0.6s) tasks — measured 20-run mean |
| Manifest entries | 203 for Python+numpy matmul task |

---

## Architecture

### Three Modes

Controlled by `LIBPTU_MODE` environment variable before task launch:

```bash
# Log mode: record all accessed paths to cde.manifest
LIBPTU_MODE=log LIBPTU_OUTPUT=/output/pkg LD_PRELOAD=libptu.so python3 task.py

# Replay mode: redirect file paths into existing cde-root
LIBPTU_MODE=replay LIBPTU_CDE_ROOT=/pkg/cde-root LD_PRELOAD=libptu.so python3 task.py

# Capture mode: log + copy files (not yet implemented — stubs only)
LIBPTU_MODE=capture LIBPTU_OUTPUT=/output/pkg LD_PRELOAD=libptu.so python3 task.py
```

### Initialization Flow

```
ld-linux loads libptu.so (LD_PRELOAD, before main())
    │
    ├── libptu_lazy_init() [called from HOOK_PREAMBLE on any early hook fire]
    │       dlsym(RTLD_NEXT, ...) for all 36 real_* function pointers
    │       sets g_real_initialized = 1
    │
    └── libptu_init() [__attribute__((constructor))]
            tl_in_hook = 1          (block hooks during init)
            libptu_lazy_init()      (ensure real_* set)
            parse LIBPTU_MODE
            init g_cwd from getcwd()
            if log/capture:
                mkdir output_dir, cde-root
                fopen cde.manifest (256KB stdio buffer)
                capture_environment() → cde.full-environment.cde-root
                scan_loaded_libs()   → dl_iterate_phdr + /proc/self/exe
            if replay:
                set g_cde_root from LIBPTU_CDE_ROOT or LIBPTU_OUTPUT/cde-root
            tl_in_hook = 0          (enable hooks)
```

**libptu_lazy_init() rationale**: ld-linux can call `open()`, `stat()`, `mmap()` etc.
during dynamic linking — before `__attribute__((constructor))` fires. Without lazy
init, all `real_*` pointers are NULL at that point → SIGSEGV on NULL call.
`HOOK_PREAMBLE` calls `libptu_lazy_init()` in its early-exit path, guaranteeing all
pointers are valid even on pre-constructor hook invocations.

---

## Global State

```c
libptu_mode_t   g_mode;              // MODE_DISABLED|LOG|CAPTURE|REPLAY
char            g_output_dir[];      // LIBPTU_OUTPUT path
char            g_cde_root[];        // cde-root absolute path (replay)
int             g_cde_root_len;      // strlen(g_cde_root), precomputed

FILE           *g_manifest_fp;       // cde.manifest file handle (log/capture)
pthread_mutex_t g_manifest_mutex;    // guards manifest writes + dedup table

char           *g_dedup[16384];      // open-addressing dedup hash table
                                     // prevents duplicate manifest entries

char           *g_fd_table[65536];   // fd → absolute path mapping
pthread_mutex_t g_fd_mutex;          // guards fd_table writes

char            g_cwd[PATH_MAX];     // current working directory
pthread_rwlock_t g_cwd_rwlock;       // readers (open hooks) / writer (chdir)

__thread int    tl_in_hook;          // per-thread recursion guard
```

---

## Hook Inventory

### Interposed Symbols (36 total)

All hooks follow the pattern:
1. `libptu_lazy_init()` (or via `HOOK_PREAMBLE`)
2. Return early if `tl_in_hook || MODE_DISABLED` (passthrough to real fn)
3. Set `tl_in_hook = 1`
4. Resolve absolute path
5. In replay: redirect path via `maybe_redirect()`
6. Call `real_fn(...)`
7. Record path: `handle_path(abs)` → `manifest_record_abs()`
8. Update fd_table if fd result
9. `tl_in_hook = 0`, return

#### File Open / Create
| Hook | Notes |
|---|---|
| `open(path, flags, [mode])` | extracts mode from varargs only when O_CREAT/O_TMPFILE set |
| `open64(...)` | calls `_open_impl()` directly (avoids recursive hook entry) |
| `openat(dirfd, path, flags, [mode])` | resolves dirfd via fd_table or /proc/self/fd |
| `openat64(...)` | calls `_openat_impl()` directly |
| `creat(path, mode)` | implemented as open(O_WRONLY\|O_CREAT\|O_TRUNC) |

#### Stat / Existence Check
| Hook | Notes |
|---|---|
| `stat(path, buf)` | |
| `lstat(path, buf)` | |
| `fstat(fd, buf)` | resolves fd → path via fd_table |
| `fstatat(dirfd, path, buf, flags)` | |
| `__xstat(ver, path, buf)` | glibc <2.33 versioned wrapper |
| `__lxstat(ver, path, buf)` | glibc <2.33 versioned wrapper |
| `__fxstat(ver, fd, buf)` | glibc <2.33 versioned wrapper |
| `__fxstatat(ver, dirfd, path, buf, flags)` | glibc <2.33 versioned wrapper |

#### Access / Permission
| Hook | Notes |
|---|---|
| `access(path, mode)` | logs regardless of result (dependency revealed) |
| `faccessat(dirfd, path, mode, flags)` | |

#### Symlinks / Links / Delete / Rename
| Hook | Notes |
|---|---|
| `readlink(path, buf, sz)` | logs symlink path |
| `readlinkat(dirfd, path, buf, sz)` | |
| `symlink(target, linkpath)` | passthrough — no audit needed in log mode |
| `symlinkat(target, newdirfd, linkpath)` | passthrough |
| `link(old, new)` | passthrough |
| `linkat(...)` | passthrough |
| `unlink(path)` | passthrough |
| `unlinkat(dirfd, path, flags)` | passthrough |
| `rename(old, new)` | passthrough |
| `renameat(...)` | passthrough |

#### Directory Operations
| Hook | Notes |
|---|---|
| `mkdir(path, mode)` | passthrough |
| `mkdirat(dirfd, path, mode)` | logs created dir path |
| `rmdir(path)` | passthrough |
| `chdir(path)` | updates g_cwd under rwlock write |
| `fchdir(fd)` | updates g_cwd from fd_table |

#### Permission / Ownership
| Hook | Notes |
|---|---|
| `chmod(path, mode)` | logs path |
| `fchmod(fd, mode)` | passthrough (fd not resolved) |
| `fchmodat(dirfd, path, mode, flags)` | logs path |
| `chown(path, uid, gid)` | passthrough |
| `lchown(path, uid, gid)` | passthrough |
| `fchownat(...)` | passthrough |

#### fd Lifecycle
| Hook | Notes |
|---|---|
| `close(fd)` | fd_table_del(fd) |
| `dup(oldfd)` | fd_table_dup(old→new) |
| `dup2(old, new)` | fd_table_dup(old→new) |
| `dup3(old, new, flags)` | fd_table_dup(old→new) |
| `fcntl(fd, F_DUPFD/F_DUPFD_CLOEXEC, ...)` | fd_table_dup on dup cmds only |

#### Memory-Mapped I/O
| Hook | Notes |
|---|---|
| `mmap(addr, len, prot, flags, fd, off)` | file-backed only (MAP_ANONYMOUS skipped); resolves fd → path |
| `mmap64(...)` | alias to `mmap` |

#### Process / Execution
| Hook | Notes |
|---|---|
| `execve(path, argv, envp)` | logs binary + ELF PT_INTERP (ld-linux); injects LD_PRELOAD into child envp |
| `execveat(dirfd, path, argv, envp, flags)` | same as execve |
| `dlopen(filename, flags)` | calls `real_dlopen`; after load calls `scan_loaded_libs()` via `dl_iterate_phdr` to capture transitively loaded .so files |
| `dlclose(handle)` | passthrough with lazy init guard |

---

## Performance-Critical Paths

### Three-Level Dedup (manifest_record_abs)

```
call: manifest_record_abs(abspath)
  │
  ├── Level 1: __thread tl_last[PATH_MAX]
  │   strcmp(abspath, tl_last) == 0 → RETURN (no lock, no hash)
  │   ~94% of calls exit here (sequential duplicate opens)
  │
  ├── Level 2: lockless primary-slot check
  │   slot = djb2(abspath) & 16383
  │   __atomic_load_n(&g_dedup[slot], RELAXED) → if match → RETURN (no lock)
  │
  └── Level 3: real_lstat + mutex + full linear probe + write
      lstat(abspath) → get type (F/D/L)
      pthread_mutex_lock(&g_manifest_mutex)
      linear probe g_dedup[16384] for slot or duplicate
      if new: g_dedup[slot] = strdup(abspath)
               fprintf(g_manifest_fp, "F/D/L %06o %s ...\n")
      pthread_mutex_unlock(&g_manifest_mutex)
```

**Dedup table**: 16384-slot open-addressing hash, djb2 hash, linear probe.
**Manifest buffer**: 256KB `_IOFBF` setvbuf — no per-write flush, flush at destructor.

### HOOK_PREAMBLE Fast Path

```c
#define HOOK_PREAMBLE(fallback_expr)
    do {
        if (__builtin_expect(tl_in_hook | (g_mode == MODE_DISABLED), 0)) {
            libptu_lazy_init();   // no-op if already initialized
            return (fallback_expr);
        }
        tl_in_hook = 1;
    } while (0)
```

`__builtin_expect(..., 0)` marks the re-entry/disabled branch as cold.
Branch predictor never misses on the hot path (tl_in_hook=0, mode=LOG).

### Shared Library Scan (dlopen hook)

After each `dlopen()`, `dl_iterate_phdr()` walks the in-memory link map to find
newly loaded `.so` files. This avoids reading `/proc/self/maps` (procfs regenerates
the VMA list on every read — O(VMAs) kernel work per call). `dl_iterate_phdr`
reads the runtime link map directly — O(loaded libs), no syscall.

### Path Resolution (resolve_at)

```c
const char *resolve_at(int dirfd, const char *path, char *out_buf) {
    if (path[0] == '/') return path;         // absolute: zero copy
    if (dirfd == AT_FDCWD) {
        pthread_rwlock_rdlock(&g_cwd_rwlock);
        snprintf(out_buf, PATH_MAX, "%s/%s", g_cwd, path);
        pthread_rwlock_unlock(&g_cwd_rwlock);
        return out_buf;
    }
    // dirfd: check fd_table first, fallback to /proc/self/fd/<dirfd>
    char *fdt = fd_get(dirfd);
    if (fdt) { snprintf(out_buf, ...); return out_buf; }
    readlink("/proc/self/fd/<dirfd>", ...);
}
```

CWD uses `pthread_rwlock_t` — many concurrent readers (open hooks), one rare
writer (chdir hook). Absolute paths (most common in Python) take the zero-copy
fast path.

---

## Output Package Structure (log mode)

```
$LIBPTU_OUTPUT/
    cde.manifest                  — F/D/L entries, one per unique path
    cde.full-environment.cde-root — null-separated env vars (PTU format)
    cde-root/                     — empty dir (created at init; populated by materialize_libptu.py)
```

### cde.manifest Format

```
F 000644 /usr/lib/x86_64-linux-gnu/libc.so.6
F 000664 /shared/miniconda3/envs/ptu_worker_env/lib/python3.9/__pycache__/abc.cpython-39.pyc
L 000777 /lib/x86_64-linux-gnu/libc.so.6 libc-2.31.so
D 000755 /shared/miniconda3/envs/ptu_worker_env/lib/python3.9
```

Fields: `TYPE MODE ABSPATH [SYMLINK_TARGET]`
- `F` = regular file
- `L` = symlink; target is 4th field (relative or absolute)
- `D` = directory

Compatible with `materialize_libptu.py` and (for log mode packages) the existing
`materialize.py` from PTU.

---

## Benchmark Results (measured on remote, 20-run mean)

Task: `python3 -c "import numpy as np; C=np.random.rand(500,500)@np.random.rand(500,500)"`

| Mode | Mean time | Overhead |
|---|---|---|
| baseline (no libptu) | 0.604s | — |
| libptu log | 0.601s | -0.6% (within noise) |

203 manifest entries captured. stdev 0.06s on baseline, 0.048s on libptu.

Compared to PTU:
| Mode | Per-task overhead |
|---|---|
| PTU logonly (ptrace + manifest) | +0.40s (+80%) |
| PTU current (ptrace + file copy) | +1.38s (+230%) |
| libptu log (LD_PRELOAD + manifest) | ~0s (<1%) |

---

## Known Gaps vs PTU (from manifest comparison)

Based on diff of libptu manifest (203 entries) vs ptu-logonly manifest (438 entries)
on same Python+numpy matmul task. 188 entries overlap; 250 in ptu-logonly only;
15 in libptu only.

### Gap 1: Directory entries not recorded (D entries)
**Count**: ~180 of the 250 missed entries  
**Root cause**: libptu only records paths that are `open()`d or `stat()`d. Directories
that are merely traversed (parent dirs of accessed files) are not captured. PTU
records every directory in the path chain as a `D` entry by calling
`copy_all_ancestor_dirs()` on every captured path.  
**Impact**: `materialize_libptu.py` and replay mode cannot reconstruct the correct
directory tree. `mkdir -p` in the materializer partially mitigates this, but mode
bits on parent dirs are lost.  
**Fix**: In `manifest_record_abs()`, walk each path component and emit a `D` entry
for every ancestor directory not already in the dedup table.

### Gap 2: System config files opened by glibc at startup
**Count**: ~5 entries (`/etc/ld.so.cache`, `/etc/locale.alias`, `/etc/localtime`,
`/usr/lib/locale/locale-archive`, `/usr/lib/x86_64-linux-gnu/gconv/gconv-modules.cache`)  
**Root cause**: glibc opens these files during its own initialization — before
libptu's constructor fires. `libptu_lazy_init()` ensures real_* pointers are set
when hooks first fire, but the actual file access (and thus path logging) doesn't
happen because these opens are internal to glibc's startup and don't go through the
application's PLT.  
**Fix**: In `libptu_init()`, after setting up the manifest, explicitly stat and
record these known glibc startup paths:
```c
static const char *glibc_startup_paths[] = {
    "/etc/ld.so.cache", "/etc/locale.alias", "/etc/localtime",
    "/usr/share/locale/locale.alias", NULL
};
```

### Gap 3: Symlink chain roots (`/bin`, `/lib`, `/lib64`, `/sbin`)
**Count**: ~8 entries  
**Root cause**: On Ubuntu 22.04, `/bin → usr/bin`, `/lib → usr/lib`, etc. are
filesystem-level symlinks. PTU intercepts `lstat` on these and records them.
libptu's `stat`/`lstat` hooks fire when the path is accessed, but these top-level
symlinks are accessed by ld-linux itself during startup (not through our hooks).  
**Fix**: Combine with Gap 2 fix — enumerate and record known OS-level symlinks
at startup. Or: after each `scan_loaded_libs()`, walk the path of each recorded
lib and record any symlinks along the way.

### Gap 4: Python `.py` source files
**Count**: ~50 entries  
**Root cause**: PTU captures `.py` source files because Python's importlib calls
`os.stat()` on them to check freshness vs `.pyc`. Our `stat` hook fires, but the
task compared ran with up-to-date `.pyc` cache — Python may use `statx()` on Linux
4.11+ instead of `stat()`. Since we don't hook `statx`, the `.py` stat is invisible
to libptu. Additionally, libptu captures `.pyc` files (what Python actually opens
and reads) while PTU captures `.py` files (what Python stats for freshness).  
**Both are valid** for different purposes: `.pyc` is what the task actually reads at
runtime; `.py` is needed only if the target machine re-imports from scratch.  
**Fix for completeness**: add a `statx` hook via `syscall(SYS_statx, ...)`.

### Gap 5: Unresolved relative `.so` paths in libptu (extra entries)
**Count**: 7 entries in libptu only  
**Root cause**: `dl_iterate_phdr` returns `dlpi_name` as the raw path string from
the ELF RPATH/RUNPATH, which can be relative (`../../libcrypto.so.3`). libptu logs
these as-is; PTU uses `realpath()` on every intercepted path.  
**Impact**: These paths resolve to the same files already in the manifest under
canonical paths. No functional gap — duplicate capture only.  
**Fix**: Apply `realpath()` in `manifest_record_abs()` before dedup check. Note:
`realpath()` is an extra syscall per new path; only call it in the slow path (Level 3).

---

## Portability Constraints

See `LIBPTU_DESIGN.md` TODO section for full portability notes.

| Constraint | Impact |
|---|---|
| Linux only | `/proc/self/fd`, `dl_iterate_phdr`, `execveat`, `O_TMPFILE` |
| glibc only | `__xstat`/`__lxstat` hooks; `__thread` TLS |
| x86_64 / ARM64 (64-bit ELF) | ELF parser uses `Elf64_Ehdr` hardcoded |
| Dynamically linked programs only | LD_PRELOAD not inherited by statically linked exec |
| Not setuid | kernel strips LD_PRELOAD for setuid binaries |
| No io_uring | kernel async I/O bypasses all libc wrappers |

---

## Files

```
libptu/
  include/libptu_internal.h     — shared types, extern declarations, macros
  src/libptu.c                  — all hooks, constructor, global state (1167 lines)
  Makefile                      — builds libptu.so (-O2 -flto=auto -ldl -lpthread)
  materialize_libptu.py         — reconstructs cde-root from cde.manifest
  test_libptu.sh                — smoke test: /bin/ls + python3+numpy + overhead
  LIBPTU_STATE.md               — this document
```

---

## Next Steps (prioritized)

1. **Record ancestor directories** (Gap 1) — highest impact; needed for correct replay
2. **Add `statx` hook** (Gap 4) — Python 3.10+ uses it; affects capture completeness
3. **Record glibc startup paths** (Gap 2) — needed for hermetic replay on bare machine
4. **Apply `realpath()` in slow path** (Gap 5) — eliminates relative path duplicates
5. **Implement capture mode** — integrate `okapi.c` file copying for full materialization
6. **Benchmark libptu-replay vs cde-exec** — validate that redirect overhead < cde-exec ptrace overhead
