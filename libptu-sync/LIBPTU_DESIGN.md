# libptu Architecture — How It Captures Dependencies

> **This is the `libptu-sync` fork.** It implements `MODE_CAPTURE` for real:
> files are copied into `cde-root` synchronously, the moment each is first
> touched, instead of only being recorded to `cde.manifest` for a later
> `libptu-materialize` pass. The original `libptu/` is untouched — its
> `MODE_CAPTURE` is still a no-op stub identical to `MODE_LOG`. Trigger this
> with `LIBPTU_MODE=capture` (the existing, already-documented env var and
> mode value — no new env var was needed). `libptu-sync/libptu-launcher`
> defaults to `LIBPTU_MODE=capture` for convenience but respects an
> already-exported `LIBPTU_MODE` if the caller set one first.
>
> Ablation-study mapping: this is what conditions 5/6 ("Interposition Only /
> + Reuse (sync materialization)") in `revised_experiments/` need — swap
> `/shared/libptu/libptu-launcher` for `/shared/libptu-sync/libptu-launcher`
> in the worker/manager sbatch scripts for those two conditions. No separate
> async-materialize step is needed afterward for the file-copy part (files
> are already in `cde-root` by the time the process exits) — though you may
> still want to invoke `libptu-materialize`'s SIF-build step if the ablation
> wants a Container Construction Time number for these conditions too.
>
> See `capture_dest_path`/`capture_copy_file`/`capture_place_symlink`/
> `capture_mkdirs_p` in `src/libptu.c` for the implementation, and the "Known
> limitations" section at the bottom of this doc for what synchronous mode
> trades away vs. the async path.

## Background: The Problem

When a Python + numpy task runs on Worker A, it silently relies on hundreds of files: the Python interpreter, shared libraries (`.so` files), Python source files, locale data, timezone files, and more. To replay that same task on Worker B — possibly a different machine — every file the task touched must be captured and shipped.

PTU (provenance-to-use) solves this using `strace`, which intercepts every system call the program makes. It works perfectly, but the interception mechanism adds 30-50% overhead to every task. For short tasks (< 1 second), that overhead dominates.

**libptu** achieves the same goal with near-zero overhead using a completely different technique.

---

## Core Mechanism: LD_PRELOAD Symbol Interposition

### What LD_PRELOAD does

On Linux, every program links against the C standard library (`glibc`). When a program wants to open a file, it calls a function like `open()` or `stat()` — these are defined in glibc. The dynamic linker (ld-linux) loads glibc and wires up these function calls at program startup.

`LD_PRELOAD` lets you inject a shared library **before** glibc in this loading order. Your library's functions get called instead of glibc's. This is called **symbol interposition** — your symbol "interposes" the real one.

```
Without libptu:
  program → open("/etc/foo") → glibc open() → kernel

With libptu:
  program → open("/etc/foo") → libptu open() → records path → glibc open() → kernel
```

libptu exploits this to intercept every file-access call the program makes. No kernel patches, no root privileges, no separate tracer process — it runs entirely inside the target program's memory.

### What libptu intercepts

libptu overrides ~40 glibc functions. These cover every way a program can access a file:

| Category | Functions hooked | What it captures |
|---|---|---|
| Open files | `open`, `openat`, `open64`, `openat64`, `creat`, `fopen`, `fopen64` | Every file opened for reading |
| Stat files | `stat`, `lstat`, `fstat`, `fstatat`, `__xstat64`, `__lxstat64`, `statx` | Files whose metadata is checked (Python checks `.py` source this way) |
| Read symlinks | `readlink`, `readlinkat` | Symlink targets (OS-level symlinks like `/bin → usr/bin`) |
| Memory maps | `mmap` | Shared libraries mapped into memory |
| Load libraries | `dlopen` | Python extension modules (`.so` files) loaded dynamically |
| Execute programs | `execve`, `execveat` | Child programs (captures ld-linux path from ELF header) |
| File metadata | `access`, `faccessat`, `chmod`, `fchmodat` | Files whose permissions are checked |
| Directory changes | `chdir`, `fchdir` | Tracks current working directory for relative paths |

Each hook follows the same pattern:
1. Resolve the path to an absolute path
2. Call the **real** glibc function (so the program works correctly)
3. Record the absolute path in the manifest

---

## The Manifest

Every unique file the program touches gets one line in `cde.manifest`:

```
F 000664 /shared/miniconda3/envs/ptu_env/lib/python3.9/os.py
D 000755 /shared/miniconda3/envs/ptu_env/lib/python3.9
L 000777 /bin usr/bin
```

Three entry types:
- **F** — regular file (mode, absolute path)
- **D** — directory (mode, absolute path) — needed so replay can reconstruct the directory tree
- **L** — symbolic link (mode, path, target) — needed for OS-level symlinks like `/lib → usr/lib`

Mode bits (e.g. `000664`) record the Unix permissions, so replay can set them correctly.

---

## Three-Level Deduplication

A Python + numpy task touches 400+ files, but most are touched many times (e.g., `stat()` on the same `.pyc` file happens dozens of times). libptu records each path exactly once using a three-level cache that avoids locking in 99% of cases:

```
File access arrives at hook
        │
        ▼
Level 1: Thread-local last-seen cache
  "Is this the same path I just recorded in this thread?"
  ─── YES → return immediately (no lock, no hash, ~2 ns) ───►
        │ NO
        ▼
Level 2: Lockless primary hash slot check
  Read one entry from a 16K-slot hash table (atomic, no lock).
  ─── HIT → return immediately (~5 ns) ───────────────────►
        │ MISS
        ▼
Level 2.5: Cross-process shared memory check
  Check 1M-slot hash table shared with ALL other processes.
  ─── HIT → skip (already recorded by another task) ──────►
        │ MISS
        ▼
Level 3: Full slow path (rare — only for new unique paths)
  • Acquire mutex (thread safety)
  • Acquire flock on manifest file (process safety)
  • Call lstat() to get file type and permissions
  • Write F / D / L entry to manifest
  • Update both hash tables
  • Release locks
  • Record ancestor directories
```

For a typical workflow where Task 2 runs the same Python+numpy code as Task 1: Task 1 records ~420 unique files. Task 2 hits the shared-memory check at Level 2.5 for every path → skips all ~420 → adds zero new entries → manifest overhead is negligible.

---

## The Hybrid Architecture: Launcher + LD_PRELOAD

### The fundamental limitation of LD_PRELOAD

LD_PRELOAD works great once the program is running — but there is a window at program startup where files are opened **before** libptu's hooks are active. This happens in two places:

1. **ld-linux itself** opens `/etc/ld.so.cache` to find shared libraries. This happens before any user library (including libptu) is loaded.
2. **glibc's own initialization code** opens locale and timezone files (`/usr/lib/locale/locale-archive`, `/etc/localtime`, etc.) using its own private internal functions that bypass LD_PRELOAD entirely.

Without addressing this, those files would be missing from the manifest and cross-machine replay would fail.

### Solution: libptu-launcher

The launcher is a small binary that runs **before** the target program:

```
libptu-launcher --output /tmp/audit vine_worker localhost 9123
```

It acts as a pre-recording phase before the real work begins:

```
libptu-launcher
    │
    ├─ 1. Open /etc/ld.so.cache (explicitly reads it → hook captures it)
    │
    ├─ 2. lstat /bin, /lib, /lib32, /lib64, /sbin
    │      (Linux OS symlinks resolved by ld-linux before any hook fires)
    │
    ├─ 3. Follow /etc/localtime symlink → open actual timezone file
    │
    ├─ 4. Call setlocale() → glibc maps locale files → scan /proc/self/maps
    │      captures: locale-archive, LC_CTYPE, gconv-modules.cache
    │
    ├─ 5. Open /etc/locale.alias, /usr/share/locale/locale.alias
    │
    ├─ 6. Create shared memory session (dedup table + manifest path)
    │
    └─ exec → vine_worker [LD_PRELOAD=libptu.so, LIBPTU_SHM_ID=...]
```

The critical insight: by explicitly accessing these files in step 1-5, the launcher causes its own libptu hooks to record them — no hardcoded path lists.

---

## Session: Sharing State Across Processes

When vine_worker runs, it forks child processes for each task. Without coordination, each child process would independently record the same 420 files → manifest bloat and duplicates.

libptu uses a **shared memory session** to coordinate all processes in the same workflow:

```
┌─────────────────────────────────────────────────────────┐
│                    Shared Memory (4 MB)                  │
│                                                         │
│  seen_hashes[1M]:  [hash₁][hash₂][hash₃]...            │
│  (atomic uint32 — one hash per recorded path)           │
│                                                         │
│  manifest_path: "/tmp/audit/cde.manifest"               │
└─────────────────────────────────────────────────────────┘
         ▲              ▲              ▲
         │              │              │
   vine_worker      task 1 py3    task 2 py3
   (LIBPTU_SHM_ID)  (inherits)    (inherits)
```

### How processes join the session

The launcher creates the shared memory and stores its ID in the environment variable `LIBPTU_SHM_ID`. Since environment variables are inherited across `fork()` and `exec()`, every child process automatically knows the session ID and attaches to the same table.

### fork() handling

When vine_worker `fork()`s a task process:
- Parent: flushes manifest buffer (so the child doesn't get a partial copy)
- Child: opens a fresh file descriptor to the manifest in **append** mode (not truncate)
- Both: still attached to shared memory — no re-attachment needed

### exec() handling

When vine_worker `exec()`s python3 for a task:
- execve hook injects `LD_PRELOAD=libptu.so` + `LIBPTU_SHM_ID` + `LIBPTU_MANIFEST` into the child's environment
- python3 starts, libptu's constructor fires, reads `LIBPTU_SHM_ID` → attaches to existing session
- Manifest opens in **append** mode (the existing content is preserved)

### Concurrent writes

Multiple processes may try to write to `cde.manifest` simultaneously. libptu uses two-level locking:
- `pthread_mutex` — prevents races between threads within one process
- `flock(LOCK_EX)` — prevents races between separate processes

The lock is only held during actual disk writes (rare, only for new unique paths). The fast paths (Level 1, 2, 2.5) require no locking.

---

## What Gets Captured (Full Inventory)

### At launcher startup (pre-recorded before exec)

| File | How | Why it would be missed otherwise |
|---|---|---|
| `/etc/ld.so.cache` | Launcher opens it | ld-linux reads it before libptu loads |
| `/etc/localtime` | readlink + open | glibc opens via private internal function |
| Actual timezone file (e.g. `/usr/share/zoneinfo/UTC`) | Follow symlink | Same |
| `/usr/lib/locale/locale-archive` | setlocale() + maps scan | glibc mmap's it internally |
| `/usr/lib/locale/C.utf8/LC_CTYPE` | setlocale() + maps scan | Same |
| `/usr/lib/x86_64-linux-gnu/gconv/gconv-modules.cache` | setlocale() + maps scan | Same |
| `/bin`, `/lib`, `/lib32`, `/lib64`, `/sbin` | lstat each | ld-linux resolves them before libptu loads |
| `/etc/locale.alias`, `/usr/share/locale/locale.alias` | Explicit open | Opened+closed by glibc internally |

### At program startup (constructor)

When libptu's constructor runs (before `main()`):
- `dl_iterate_phdr()` — walks the in-memory list of loaded `.so` files. Captures ld-linux, glibc, libpython, libopenblas, and all other libraries that were loaded as static dependencies.
- `realpath()` on each path — resolves `..` in RPATH-relative paths (e.g. `../../lib/libopenblas.so.0`) to canonical absolute paths, keeping symlink names intact (so `libopenblas.so.0` is recorded as a symlink, and its target `libopenblasp-r0.3.32.so` is recorded as the real file).
- `/proc/self/exe` — captures the Python interpreter binary path.
- `/proc/self/maps` scan — catches any file-backed mappings already present (redundant with dl_iterate_phdr but catches edge cases).

### At runtime (hooks)

Every file the program opens, stats, or mmap-s after the constructor fires is captured by the hooks. For a Python+numpy matmul:
- Python stdlib `.pyc` files (200+ files)
- Python source `.py` files (Python checks these with `__xstat64` for import freshness)
- numpy extension `.so` files (captured via dlopen hook + dl_iterate_phdr)
- numpy data files (`.npy`, `.pyi`, etc.)
- OpenSSL config (`openssl.cnf`) — captured by the `fopen()` hook (OpenSSL opens it via fopen)

### At runtime (destructor)

When the program exits, libptu's destructor runs a final `/proc/self/maps` scan to catch any file-backed mappings created after the constructor (e.g., locale files mmap'd during Python's locale initialization).

### For each file recorded

For each new unique path, libptu also records its **ancestor directories**. If `/shared/miniconda3/envs/ptu_env/lib/python3.9/abc.py` is recorded, libptu automatically records D entries for:
- `/shared/miniconda3/envs/ptu_env/lib/python3.9`
- `/shared/miniconda3/envs/ptu_env/lib`
- `/shared/miniconda3/envs/ptu_env`
- `/shared/miniconda3/envs`
- `/shared/miniconda3`
- `/shared`

And if any of these happen to be OS-level symlinks (like `/lib → usr/lib` on Ubuntu 22.04), they are recorded as L entries with their targets — so replay can reconstruct the exact symlink structure.

---

## Replay Mode

To replay a captured task on a different machine:

1. **Materialize**: `materialize_libptu.py` reads `cde.manifest` and copies each listed file from the host into a `cde-root/` directory, preserving directory structure and symlinks.

2. **Replay**: `LIBPTU_MODE=replay LIBPTU_CDE_ROOT=/path/to/cde-root LD_PRELOAD=libptu.so python3 task.py`

In replay mode, every file open is intercepted and redirected:
```
open("/shared/miniconda3/envs/ptu_env/lib/python3.9/os.py")
  ↓ libptu checks: does cde-root + path exist?
  ↓ YES → open("/tmp/pkg/cde-root/shared/miniconda3/envs/ptu_env/lib/python3.9/os.py")
  ↓ NO  → fall through to host filesystem (graceful degradation)
```

The fallthrough behavior means replay works even for files that libptu couldn't capture (the 20 or so glibc-internal files). On a machine with the same OS, those files exist locally and the task runs correctly.

---

## Summary of Design Choices

| Choice | Reason |
|---|---|
| LD_PRELOAD instead of ptrace | ~0% overhead vs 30-50% for ptrace-based PTU |
| Launcher binary | Catches pre-constructor files (ld.so.cache, locale, timezone) without ptrace |
| Shared memory for dedup | Zero-overhead cross-task deduplication; Task N adds zero entries if Task 1 already covered them |
| Hash-only shmem (not string comparison) | Strings can't be shared across processes with pointer-based tables; hashes work with atomic ops |
| 1M hash slots | Collision rate < 0.05% for typical workloads (~500 unique paths) |
| flock for manifest writes | Works across processes; pthread_mutex only works within one process |
| ancestor directory recording | PTU records these; needed for hermetic replay to reconstruct the directory tree |
| Symlink following | Recording `libopenblas.so.0` (L entry) AND `libopenblasp-r0.3.32.so` (F entry) matches PTU's output and ensures the actual bytes are captured |

---

## Known Limitations

1. **~20 files still missed** — glibc's locale/timezone init uses private internal functions that no LD_PRELOAD library can intercept. These files exist on any standard Linux machine, so same-OS replay works. Cross-OS hermetic replay would need either ptrace or seccomp-unotify.

2. **Shared memory leaks if process crashes** — `IPC_RMID` is not called automatically. Clean up with `ipcrm -m <LIBPTU_SHM_ID>` or the launcher auto-cleans the previous session's shmem at startup.

3. **Fork without exec not fully tested** — the fork hook handles the common vine_worker pattern (fork+exec for each task). Programs that fork and run substantial code in both parent and child (without exec) may have edge cases.

4. **~5% overhead in log mode** — adding hooks for `__xstat64` (Python's stat family) and ancestor directory recording added overhead vs the original implementation. See TODO.md for the optimization plan (pass stat results from hooks to avoid double-lstat).

## MODE_CAPTURE (libptu-sync) — Additional Limitations

5. **MODE_CAPTURE is inherently much slower than MODE_LOG** — every first
   touch of a new file now does a real `sendfile()` copy inline in the hook,
   instead of just an `fprintf`. This is the whole point (no separate
   materialize pass needed), but it means Avg Task Time / Workflow Time
   under this mode are not comparable to MODE_LOG's near-zero-overhead
   numbers — expect noticeably higher per-task cost, roughly proportional to
   total bytes touched per task.

6. **OS-level ancestor symlinks are pre-bootstrapped for the known Ubuntu
   set only** (`/bin`, `/lib`, `/lib32`, `/lib64`, `/libx32`, `/sbin` —
   created in cde-root immediately at init, before any other file copying,
   specifically to close a race: without this, a file nested under one of
   these paths can get its parent directories created as **real** directories
   in cde-root before the ancestor symlink itself is ever recorded — ancestor
   symlinks are normally only discovered by `record_ancestors_locked()`,
   which runs *after* the triggering file is already copied). If the target
   environment has some *other*, non-standard top-level symlink not in this
   list, the same race can still occur for it; `capture_place_symlink()` has
   a fallback (demote the real directory back to a symlink, `rmdir`/recursive
   delete first) but that fallback is destructive — a file that was
   already copied into the collapsed subtree is deleted along with it and,
   since libptu only ever records each real underlying path once (dedup), it
   is **not** re-copied under the post-symlink-resolved path. In practice
   this only bites exotic/non-Ubuntu-multiarch layouts; validated clean
   (manifest F/L counts == cde-root file/symlink counts, byte-identical
   content) against both a plain `/bin/ls` run and a real Python+numpy
   import (275 files, 13 symlinks, 49 dirs, zero mismatches).

7. **Do not call `fork()`/`exec()` from inside a `manifest_record_abs()`-
   locked region** — this was tried during development (for the symlink
   fallback above, mirroring `libptu-materialize`'s own `rm_rf_path`) and
   reliably hung the host process. `libptu-materialize` is a standalone
   offline tool where forking is unremarkable; `libptu.so` is injected via
   `LD_PRELOAD` into an arbitrary target process, and forking out of it while
   holding one of the library's own locks is a fundamentally different,
   fragile situation — the exact hang mechanism wasn't pinned down (the
   `tl_in_hook` thread-local reentrancy guard is correctly inherited across
   `fork()` and should make nested hook calls in the child safe, and the
   plain `unlink`/`symlink`/`mkdir`/`rmdir` hooks are simple `PASSTHROUGH`
   macros that don't touch `g_manifest_mutex` at all — neither theory
   actually explains it) but it reproduced consistently, so the fallback
   directory removal is implemented as a plain in-process recursive
   `opendir`/`readdir`/`unlink`/`rmdir` walk instead (see `capture_rm_rf`).
   No fork, no exec, nothing to debug further.

8. **Fixed: self-referential cde-root nesting.** `g_cde_root` lives *inside*
   `g_output_dir` (e.g. `/tmp/libptu-capture-<pid>/cde-root`, since
   `libptu-sync/libptu-launcher` points `LIBPTU_OUTPUT` at a local `/tmp` dir
   to keep manifest flocks off NFS). If the target process — or in practice,
   whatever `mapreduce_benchmark.py`'s Dask/TaskVine stack does under the
   hood — ever touched a path under `g_output_dir` (including `g_cde_root`
   itself, or a file already copied there), that touch would get
   captured too, mirroring the path back into cde-root *again*, one level
   deeper: `.../cde-root/tmp/libptu-capture-N/cde-root/tmp/libptu-capture-N/
   ...`, repeating until `PATH_MAX` truncated it. Bounded (never grew past a
   few MB in testing) but left real, un-`rm`-able leftover directories on
   every run — first observed during a real MapReduce medium-scale run
   (`libptu-launcher`'s own post-run cleanup failed with `rm: ... Directory
   not empty`). Root-caused and fixed by adding a guard at the top of
   `manifest_record_abs()` (not just `handle_path()` — the function is also
   called directly from several other sites: symlink-target recursion,
   `scan_loaded_libs()`, the OS-symlink bootstrap above) that skips any path
   already under `g_output_dir` — our own bookkeeping is never something a
   replay needs anyway. Verified fixed against a real MapReduce run
   end-to-end (no nested `libptu-capture-N` subtree afterward, 128 dirs /
   83 MB, clean) without changing any other captured counts (`/bin/ls`
   still 19 F / 8 L, byte-identical, exactly as before the fix).
