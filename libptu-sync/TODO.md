# libptu TODO

## Performance

### Reduce log-mode overhead (currently ~5% on Python+numpy 500×500 matmul)

**Target**: <1% overhead (achieved before gap fixes, regressed after).

**Likely causes** (in priority order):

1. **Double-lstat in stat-family hooks** — `manifest_record_abs()` calls
   `real_lstat()` internally, but stat/lstat hooks already have the `struct stat`
   result from the user's call. Pass the known stat into manifest_record_abs to
   avoid the second lstat per new unique path.
   - Estimated impact: ~422 extra lstat calls × ~23 µs each = ~10 ms

2. **`__xstat64` hook fires 756×** during numpy import (Python uses the 64-bit
   LFS stat wrappers). These hooks are new and all 756 calls now go through our
   code. Even fast paths add function-call overhead.

3. **`record_ancestors_locked()` holding mutex** during ancestor walk — each new
   unique file triggers a walk of ~8 path components under the manifest mutex,
   including potential `real_lstat()` calls per new directory.

**Fix approach**:
- Add `manifest_record_abs_with_stat(path, const struct stat *)` variant so
  stat-family hooks can skip the internal lstat.
- Profile with `perf stat` or `strace -c` to confirm exact bottleneck before
  optimizing further.

---

## Manifest Coverage

### Files still missing vs PTU (20 entries — fundamentally uncatchable via LD_PRELOAD)

**Category A — glibc internal opens (private `__open64_nocancel`, bypasses PLT)**

These files are opened by glibc's own initialization code (locale, timezone)
using glibc-private symbols that LD_PRELOAD cannot intercept:

| Path | Reason |
|------|--------|
| `/etc/ld.so.cache` | Opened by ld-linux before libptu.so is even loaded |
| `/etc/locale.alias` | glibc `setlocale()` internal open |
| `/etc/localtime` | glibc timezone init internal open |
| `/usr/share/locale/locale.alias` | glibc locale internal open |
| `/usr/share/zoneinfo/Etc/UTC` | glibc timezone internal open |
| `/etc` | Parent dir of above (only PTU records via ancestor walk) |
| `/usr/share` | Parent dir of above |
| `/usr/share/locale` | Parent dir of above |
| `/usr/share/zoneinfo` | Parent dir of above |
| `/usr/share/zoneinfo/Etc` | Parent dir of above |

**Category B — OS-level symlinks resolved by ld-linux before libptu loads**

ld-linux resolves `/bin → usr/bin` etc. before any LD_PRELOAD library is mapped.
Paths in practice use canonical `/usr/...` forms so we never traverse these
as ancestors:

| Path | Notes |
|------|-------|
| `/bin` | Symlink → `usr/bin` |
| `/lib32` | Symlink → `usr/lib32` |
| `/lib64` | Symlink → `usr/lib64` |
| `/libx32` | Symlink → `usr/libx32` |
| `/sbin` | Symlink → `usr/sbin` |
| `/usr/bin` | PTU records this as parent of `/bin` symlink target |
| `/usr/lib32` | Same |
| `/usr/libx32` | Same |
| `/usr/sbin` | Same |

**Category C — ld-linux path via different symlink chain (cosmetic)**

| libptu records | PTU records | Notes |
|----------------|-------------|-------|
| `/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2` (F) | `/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2` (F) | Same physical file; `/lib` → `usr/lib` symlink. For same-machine replay, both work. |

**Impact on replay**: For replay on the **same machine**, all 20 missing entries
are present on the host and libptu's fallthrough behavior handles them. For
**cross-machine hermetic replay** these would need to be added via a static
enumeration in the constructor (see MANIFEST_GAPS.md Gap 2/3 fixes).
