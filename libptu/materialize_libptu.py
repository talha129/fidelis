#!/usr/bin/env python3
"""
materialize_libptu.py — reconstruct cde-root from a libptu cde.manifest.

Reads F/D/L entries from cde.manifest and copies the corresponding files
from the live host filesystem into <pkg>/cde-root/, mirroring the absolute
path structure expected by libptu replay mode.

Usage:
    python3 materialize_libptu.py <libptu-pkg-dir> [<output-cde-root>]
    python3 materialize_libptu.py --build-sif <sif-out> <libptu-pkg-dir> [<output-cde-root>]

    <libptu-pkg-dir>   directory produced by LIBPTU_MODE=log (contains cde.manifest + env.txt)
    <output-cde-root>  destination cde-root (default: <libptu-pkg-dir>/cde-root)

    --build-sif <sif-out>   also write apptainer def and build SIF at <sif-out>
"""

import os
import shutil
import subprocess
import sys
import tempfile


def materialize(pkg_dir: str, cde_root: str) -> None:
    manifest = os.path.join(pkg_dir, "cde.manifest")
    if not os.path.isfile(manifest):
        print(f"ERROR: {manifest} not found", file=sys.stderr)
        sys.exit(1)

    # Clear stale cde_root so the SIF reflects exactly this manifest.
    # Without this, files from previous captures accumulate and the SIF
    # appears correct even when the current capture is incomplete.
    if os.path.exists(cde_root):
        shutil.rmtree(cde_root)
    os.makedirs(cde_root, exist_ok=True)

    dirs, files, links = [], [], []
    for lineno, raw in enumerate(open(manifest), 1):
        line = raw.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 3:
            print(f"WARNING: line {lineno} malformed: {raw!r}", file=sys.stderr)
            continue
        kind = parts[0]
        try:
            mode = int(parts[1], 8)
        except ValueError:
            print(f"WARNING: line {lineno} bad mode: {raw!r}", file=sys.stderr)
            continue
        if kind == "D":
            dirs.append((parts[2], mode))
        elif kind == "F":
            files.append((parts[2], mode))
        elif kind == "L":
            if len(parts) < 4:
                print(f"WARNING: line {lineno} L entry missing target: {raw!r}", file=sys.stderr)
                continue
            links.append((parts[2], parts[3], mode))

    # Build set of symlink paths so we can skip creating directories
    # whose ancestor is a symlink (e.g. /lib/x86_64-linux-gnu should not
    # be created as a real dir when /lib → usr/lib is a symlink).
    symlink_paths = {path for path, _, _ in links}

    def has_symlink_ancestor(path: str) -> bool:
        """Return True if any prefix of path is recorded as a symlink."""
        parts = path.split("/")
        for i in range(1, len(parts)):
            if "/".join(parts[:i]) in symlink_paths:
                return True
        return False

    # /proc and /sys are kernel virtual filesystems — no real files, can't be copied.
    # They're always present on any Linux host at replay time.
    VIRTUAL_FS = ("/proc", "/sys", "/dev")

    def is_virtual(path: str) -> bool:
        return path in VIRTUAL_FS or any(path.startswith(v + "/") for v in VIRTUAL_FS)

    # 1. Create directories (depth-sorted), skipping those under a symlink ancestor.
    for path, mode in sorted(dirs, key=lambda x: x[0].count("/")):
        if is_virtual(path) or has_symlink_ancestor(path):
            continue
        dst = cde_root + path
        os.makedirs(dst, exist_ok=True)
        try:
            os.chmod(dst, mode)
        except OSError:
            pass

    # 2. Create symlinks before copying files so OS-level symlinks like
    #    /lib → usr/lib exist before os.makedirs tries to create paths under them.
    lk_ok = 0
    for path, target, mode in sorted(links, key=lambda x: x[0].count("/")):
        if is_virtual(path):
            continue
        dst = cde_root + path
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.lexists(dst):
            if os.path.isdir(dst) and not os.path.islink(dst):
                try:
                    os.rmdir(dst)
                except OSError:
                    shutil.rmtree(dst)
            else:
                os.remove(dst)
        os.symlink(target, dst)
        lk_ok += 1

    # 3. Copy files from host
    ok = miss = skip = 0
    so_files = []   # track copied .so files for symlink discovery in step 4
    for path, mode in files:
        if is_virtual(path):
            continue
        src = path          # absolute host path
        dst = cde_root + path
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(src):
            print(f"MISSING: {src}")
            miss += 1
            continue
        if os.path.islink(src):
            # source is a symlink — copy the link itself
            target = os.readlink(src)
            if os.path.lexists(dst):
                os.remove(dst)
            os.symlink(target, dst)
            skip += 1
        else:
            try:
                shutil.copy2(src, dst)
                os.chmod(dst, mode)
                ok += 1
                if ".so" in os.path.basename(path):
                    so_files.append(path)
            except OSError as e:
                print(f"ERROR copying {src}: {e}", file=sys.stderr)
                miss += 1

    # 4. Discover .so symlinks missed by libptu capture.
    #
    # ld-linux resolves DT_NEEDED entries (e.g. "libblas.so.3") internally
    # when loading a real .so, bypassing PLT hooks.  libptu records the real
    # file (libopenblasp-r0.3.33.so) but never sees the symlink name.  At
    # replay, dlopen("libblas.so.3") fails because the symlink is absent.
    #
    # Fix: for each real .so copied above, scan its host directory for
    # symlinks that resolve to the same inode.  Add any missing ones to the
    # cde-root.  One scan per unique directory (scanned_dirs dedup).
    lk_so = 0
    for path in so_files:
        host_dir = os.path.dirname(path)
        try:
            real_st = os.stat(path)         # follow symlinks → real file inode
        except OSError:
            continue
        try:
            entries = os.scandir(host_dir)
        except OSError:
            continue
        with entries:
            for entry in entries:
                if ".so" not in entry.name:
                    continue
                if not entry.is_symlink():
                    continue
                try:
                    link_st = os.stat(entry.path)   # follow to real file
                except OSError:
                    continue
                # Same inode+device → this symlink resolves to the same file
                if (link_st.st_ino != real_st.st_ino or
                        link_st.st_dev != real_st.st_dev):
                    continue
                sym_abs = entry.path           # e.g. /shared/.../lib/libblas.so.3
                dst = cde_root + sym_abs
                if os.path.lexists(dst):
                    continue                   # already recorded in manifest
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.symlink(os.readlink(sym_abs), dst)
                lk_so += 1

    total_size = sum(
        os.path.getsize(cde_root + p)
        for p, _ in files
        if os.path.isfile(cde_root + p)
    )
    print(f"Materialized: {ok} files, {len(dirs)} dirs, {lk_ok} symlinks, {lk_so} so-symlinks")
    if miss:
        print(f"Missing from host: {miss} files")
    print(f"cde-root: {cde_root}")
    print(f"Total size: {total_size / 1e6:.1f} MB")


# Keys to skip in %environment (internal/session vars irrelevant at replay time)
_ENV_SKIP_PREFIXES = (
    "LIBPTU_", "LD_PRELOAD=", "BASH_FUNC", "SSH_", "DBUS_",
    "MOTD_", "XDG_SESSION_", "XDG_RUNTIME_",
    # Captured values of these are wrong at replay time — overridden explicitly below
    "VINE_AUDIT_MODE", "VINE_REPLAY_MODE", "TASKVINE_WARM_POOL",
)
_ENV_SKIP_EXACT = {"_=", "SHLVL=", "PWD="}


def _env_line_skip(line: str) -> bool:
    for pfx in _ENV_SKIP_PREFIXES:
        if line.startswith(pfx):
            return True
    for exact in _ENV_SKIP_EXACT:
        if line.startswith(exact):
            return True
    if not line or line[0] == "}":
        return True
    return False


def _single_quote_escape(val: str) -> str:
    return val.replace("'", "'\\''")


def write_apptainer_def(pkg_dir: str, cde_root: str, def_path: str) -> None:
    env_path = os.path.join(pkg_dir, "env.txt")
    lines = []
    if os.path.isfile(env_path):
        for raw in open(env_path):
            line = raw.rstrip("\n")
            if _env_line_skip(line):
                continue
            eq = line.find("=")
            if eq < 0:
                continue
            key = line[:eq]
            val = line[eq + 1:]
            lines.append(f"    export {key}='{_single_quote_escape(val)}'")

    with open(def_path, "w") as f:
        f.write(f"Bootstrap: localimage\nFrom: {cde_root}\n\n%environment\n")
        for l in lines:
            f.write(l + "\n")
        # Inject correct replay-time values regardless of what was set at capture
        f.write("    export VINE_AUDIT_MODE='0'\n")
        f.write("    export VINE_REPLAY_MODE='1'\n")
        f.write("    export TASKVINE_WARM_POOL='1'\n")
        f.write("\n%runscript\n    exec \"$@\"\n")


def build_sif(pkg_dir: str, cde_root: str, sif_out: str) -> None:
    with tempfile.TemporaryDirectory(prefix="libptu-apptainer-") as tmpdir:
        def_path = os.path.join(tmpdir, "container.def")
        write_apptainer_def(pkg_dir, cde_root, def_path)
        print(f"[libptu] building SIF: {sif_out} ...")
        env = {k: v for k, v in os.environ.items() if k != "LD_PRELOAD"}
        result = subprocess.run(
            ["apptainer", "build", "--force", sif_out, def_path],
            env=env,
        )
        if result.returncode != 0:
            print(f"ERROR: apptainer build failed (exit {result.returncode})", file=sys.stderr)
            sys.exit(result.returncode)
        print(f"[libptu] SIF ready: {sif_out}")


if __name__ == "__main__":
    args = sys.argv[1:]
    sif_out = None

    if args and args[0] == "--build-sif":
        if len(args) < 2:
            print("ERROR: --build-sif requires <sif-out>", file=sys.stderr)
            sys.exit(1)
        sif_out = args[1]
        args = args[2:]

    if not args:
        print(__doc__)
        sys.exit(1)

    pkg  = os.path.abspath(args[0])
    root = os.path.abspath(args[1]) if len(args) > 1 \
           else os.path.join(pkg, "cde-root")

    materialize(pkg, root)

    if sif_out:
        build_sif(pkg, root, os.path.abspath(sif_out))
