# Copyright (C) 2024- The University of Notre Dame
# This software is distributed under the GNU General Public License.
# See the file COPYING for details.

##
# @package ndcctools.taskvine.warm_manager
#
# Warm-pool support for TaskVine Manager.
#
# Problem being solved
# --------------------
# Every PythonTask spawns a fresh Python interpreter on the worker. That means
# every task pays the full import cost for numpy, scipy, pandas, etc. even when
# the same packages are used across thousands of identical tasks.
#
# Approach
# --------
# When TASKVINE_WARM_POOL=1 is set in the environment, Manager.submit()
# intercepts each PythonTask and:
#
#   1. DISCOVER the real callable being executed. The submitted func may be a
#      wrapper (e.g. execute_graph_vertex in DaskVine) and the actual user
#      function may be buried inside args/kwargs as a callable or as an object
#      with a .func attribute (Dask task spec pattern).
#
#   2. EXTRACT the Python modules those callables depend on, by inspecting:
#        - func.__globals__  (globals the function actually references)
#        - bytecode IMPORT_NAME instructions (in-body imports)
#
#   3. FIND OR CREATE a warm LibraryTask keyed on the frozenset of module names.
#      The library pre-imports those modules once; all subsequent FunctionCalls
#      routed to it find them already in sys.modules (free dict lookup).
#
#   4. CONVERT the PythonTask into a FunctionCall targeting _warm_universal_runner,
#      a single generic function installed in every warm library. It receives the
#      original func+args+kwargs+env_vars as cloudpickle bytes and calls them.
#
# Environment variables
# ---------------------
# TASKVINE_WARM_POOL=1    Enable warm-pool mode in any Manager instance.
# TASKVINE_WARM_DEBUG=1   Print library creation and per-task conversion details.
#
# Library reuse
# -------------
# Libraries are keyed by frozenset(module_names). Tasks that share the same
# module signature share one warm library process. A new signature triggers a
# single cold library startup; all tasks after that are warm.
#
# Env var propagation
# -------------------
# FunctionCalls run inside an already-running library process. The worker
# cannot inject env vars into a live process via the task struct, so env vars
# declared with set_env_var() are carried in the cloudpickle payload and
# applied by _warm_universal_runner directly in the fork child.
#
# What is NOT converted
# ---------------------
# - Tasks that are already a FunctionCall (including FunctionCallDask) —
#   already targeting a library; converting again would double-wrap.
# - Any non-PythonTask (plain Task, LibraryTask) — passed through unchanged.
#
# Usage
# -----
# Option A — via environment variable (no application code change):
# @code
#   TASKVINE_WARM_POOL=1 python3 my_workflow.py
# @endcode
#
# Option B — via WarmManager (explicit):
# @code
#   import ndcctools.taskvine as vine
#   m = vine.WarmManager(port=9123)
#   t = vine.PythonTask(my_numpy_func, big_array)
#   m.submit(t)
# @endcode

import dis
import hashlib
import os
import sys
import types
import cloudpickle

from .manager import Manager
from .task import FunctionCall, LibraryTask, PythonTask


# ─────────────────────────────────────────────────────────────────────────────
# Universal runner — the single function installed in every warm library.
# ─────────────────────────────────────────────────────────────────────────────

def _warm_universal_runner(func_bytes, args_bytes, kwargs_bytes, env_bytes=None):
    """
    Generic entry point for all warm library invocations.

    Deserializes func, args, kwargs from cloudpickle bytes and calls
    func(*args, **kwargs).  The library process has the relevant modules
    pre-imported, so no import overhead is paid here.

    env_bytes is a cloudpickle-serialized dict of env vars to apply for the
    duration of this call.  Vars are restored after the call so that direct
    exec mode (non-forking) doesn't leak state across concurrent invocations.
    FunctionCalls run in an already-running library process, so env vars cannot
    be injected via the task struct — they travel through this payload instead.

    Set TASKVINE_WARM_DEBUG=1 to emit per-call import tracing.
    """
    import os
    import cloudpickle

    # Apply task-scoped env vars inside the fork child (or direct exec process).
    env_vars = cloudpickle.loads(env_bytes) if env_bytes is not None else {}
    saved_env = {}
    for k, v in env_vars.items():
        saved_env[k] = os.environ.get(k)
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v

    debug = os.environ.get('TASKVINE_WARM_DEBUG') == '1'

    try:
        if debug:
            import sys
            import time
            mods_before = set(sys.modules.keys())
            t0 = time.perf_counter()

        func   = cloudpickle.loads(func_bytes)
        args   = cloudpickle.loads(args_bytes)
        kwargs = cloudpickle.loads(kwargs_bytes)

        if debug:
            t1 = time.perf_counter()
            new_mods = set(sys.modules.keys()) - mods_before
            print(
                f"[_warm_universal_runner] deserialize={t1-t0:.4f}s  "
                f"new_imports={sorted(new_mods) if new_mods else 'none (warm)'}",
                flush=True,
            )

        return func(*args, **kwargs)

    finally:
        # Restore original env values for direct exec mode where the library
        # process handles multiple concurrent calls in the same process.
        for k, orig in saved_env.items():
            if orig is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = orig


# ─────────────────────────────────────────────────────────────────────────────
# Callable discovery
# ─────────────────────────────────────────────────────────────────────────────

def _find_callables(func, args, kwargs, _depth=5):
    """
    Return a list of all Python callables reachable from (func, args, kwargs).

    We search the top-level func and then walk args/kwargs up to _depth levels
    deep, looking for:
      - Plain callables that have a __globals__ dict (Python functions).
      - Objects with a .func attribute that is itself callable (Dask TaskSpec).

    Depth is capped to avoid traversing large data arrays.
    """
    found = []

    def _visit(obj, depth):
        if depth == 0:
            return
        if callable(obj) and hasattr(obj, '__globals__'):
            found.append(obj)
        nested = getattr(obj, 'func', None)
        if nested is not None and callable(nested) and hasattr(nested, '__globals__'):
            found.append(nested)
        if isinstance(obj, dict):
            for v in obj.values():
                _visit(v, depth - 1)
        elif isinstance(obj, (list, tuple)) and not isinstance(obj, (str, bytes)):
            for v in obj:
                _visit(v, depth - 1)

    _visit(func, _depth)
    for a in args:
        _visit(a, _depth)
    for v in kwargs.values():
        _visit(v, _depth)

    return found


# ─────────────────────────────────────────────────────────────────────────────
# Module extraction
# ─────────────────────────────────────────────────────────────────────────────

def _extract_modules(callables):
    """
    Return stub module objects for every top-level package referenced or
    imported inside the bytecode of the given callables.

    Two sources per callable:
      1. LOAD_GLOBAL instructions — names the function actually reads as globals,
         filtered to those whose value in __globals__ is a module.
      2. IMPORT_NAME instructions — explicit in-body imports, taken directly
         from the bytecode without triggering any real import.

    Stubs (types.ModuleType(name)) satisfy generate_hoisting_code's
    isinstance check and __name__ read without importing anything on the
    manager side.
    """
    names = set()

    for func in callables:
        globals_dict = getattr(func, '__globals__', {})
        try:
            instrs = list(dis.get_instructions(func.__code__))
        except (AttributeError, TypeError):
            continue

        used_globals = {i.argval for i in instrs if i.opname == 'LOAD_GLOBAL'}
        for gname in used_globals:
            obj = globals_dict.get(gname)
            if isinstance(obj, types.ModuleType):
                names.add(obj.__name__.split('.')[0])

        for instr in instrs:
            if instr.opname == 'IMPORT_NAME':
                names.add(instr.argval.split('.')[0])

    names.discard('builtins')
    return [types.ModuleType(name) for name in names]


# ─────────────────────────────────────────────────────────────────────────────
# Manager plugin — called by Manager.__init__ and Manager.submit
# ─────────────────────────────────────────────────────────────────────────────

def _warm_init(manager):
    """
    Initialize warm-pool state on a Manager instance.
    Called by Manager.__init__ when TASKVINE_WARM_POOL=1.
    """
    manager._warm_lib_cache = {}
    manager._warm_debug = os.environ.get('TASKVINE_WARM_DEBUG') == '1'


def _warm_intercept(manager, task):
    """
    Convert a PythonTask to a warm FunctionCall if applicable.
    Called by Manager.submit when warm mode is active.

    FunctionCall and all non-PythonTask types pass through unchanged.
    """
    if isinstance(task, PythonTask) and not isinstance(task, FunctionCall):
        return _warm_convert(manager, task)
    return task


# ─────────────────────────────────────────────────────────────────────────────
# Core conversion
# ─────────────────────────────────────────────────────────────────────────────

def _warm_convert(manager, python_task):
    """
    Convert a PythonTask into a FunctionCall targeting a warm library.

    Steps:
      1. Unpack _fn_def to get (func, args, kwargs).
      2. Discover all real callables in the task definition.
      3. Extract module dependencies of those callables.
      4. Find or create a warm library for that module signature.
      5. Extract env vars from the mount log (passed via payload, not task struct).
      6. Build and return a FunctionCall targeting _warm_universal_runner.
      7. Replay all other logged field operations onto the FunctionCall.
    """
    import time as _time

    func, args, kwargs = python_task._fn_def

    t0 = _time.perf_counter()
    callables = _find_callables(func, args, kwargs)
    modules   = _extract_modules(callables)
    t1 = _time.perf_counter()

    if manager._warm_debug:
        print(f"[WarmManager] task func={getattr(func, '__name__', repr(func))!r}")
        print(f"[WarmManager]   callables: {[getattr(c, '__name__', repr(c)) for c in callables]}")
        print(f"[WarmManager]   modules:   {[m.__name__ for m in modules]}")
        print(f"[WarmManager]   module_extract={t1-t0:.4f}s")

    lib_name = _warm_ensure_library(manager, modules)
    t2 = _time.perf_counter()

    if manager._warm_debug:
        print(f"[WarmManager]   ensure_library={t2-t1:.4f}s")

    # Env vars travel via cloudpickle payload because the library process is
    # already running when FunctionCalls arrive; the worker cannot inject env
    # vars into a live process via the task struct.
    env_vars = {
        args_[0]: kwargs_.get('value')
        for (name_, args_, kwargs_) in (python_task._mount_log or [])
        if name_ == 'set_env_var'
    }
    if manager._warm_debug:
        env_vars['TASKVINE_WARM_DEBUG'] = '1'

    fc = FunctionCall(
        lib_name,
        '_warm_universal_runner',
        cloudpickle.dumps(func),
        cloudpickle.dumps(args),
        cloudpickle.dumps(kwargs),
        cloudpickle.dumps(env_vars),
    )
    t3 = _time.perf_counter()

    if manager._warm_debug:
        print(f"[WarmManager]   cloudpickle_dumps={t3-t2:.4f}s")

    # Replay file mounts, resources, scheduling hints from the original task.
    # set_env_var is excluded — handled via env_vars payload above.
    _warm_replay_log(python_task, fc)

    # Carry audit key so _audit_record sees the original func+args hash, not the
    # meaningless FunctionCall hash (func=None, args=() from super().__init__).
    if hasattr(python_task, '_vine_audit_key'):
        fc._vine_audit_key = python_task._vine_audit_key

    # Carry Dask-specific metadata so DaskVine's result-routing loop can read
    # t.key, t.dask_task, and t.decrement_retry() from the converted FunctionCall.
    if hasattr(python_task, '_dask_task'):
        fc._dask_task     = python_task._dask_task
        fc._retries_left  = getattr(python_task, '_retries_left', 5)
        fc.key            = python_task._dask_task.key
        fc.dask_task      = python_task._dask_task

        def _decrement_retry(_fc=fc):
            _fc._retries_left -= 1
            return _fc._retries_left

        fc.decrement_retry = _decrement_retry
    elif hasattr(python_task, '_key'):
        # PythonTaskDask stores key directly as _key (no _dask_task wrapper)
        fc.key           = python_task._key
        fc._retries_left = getattr(python_task, '_retries_left', 5)
        if hasattr(python_task, '_sexpr'):
            fc._sexpr = python_task._sexpr

        def _decrement_retry(_fc=fc):
            _fc._retries_left -= 1
            return _fc._retries_left

        fc.decrement_retry = _decrement_retry

    return fc


# ─────────────────────────────────────────────────────────────────────────────
# Library management
# ─────────────────────────────────────────────────────────────────────────────

def _warm_ensure_library(manager, modules):
    """
    Return the name of an installed warm library that has `modules` pre-imported.

    Exact-match: same signature → return cached library immediately.
    Subset-match: new signature is a subset of an existing library → reuse that
      library; the extra pre-imported modules are harmless and avoiding a second
      library install eliminates one cold start and one core slot on the worker.
    New signature: install a fresh library and cache it.

    SHA256 on sorted module names gives a stable, deterministic name across
    Python runs. hash() is randomised per-process (PYTHONHASHSEED) and would
    create a new cache directory on every run.
    """
    sig = frozenset(m.__name__ for m in modules)

    # Exact match — common path after first install.
    if sig in manager._warm_lib_cache:
        if manager._warm_debug:
            print(f"[WarmManager] reusing library {manager._warm_lib_cache[sig]!r} (exact match)")
        return manager._warm_lib_cache[sig]

    # Subset match — reuse any superset library already installed.
    for cached_sig, cached_name in manager._warm_lib_cache.items():
        if sig <= cached_sig:
            if manager._warm_debug:
                print(f"[WarmManager] reusing library {cached_name!r} "
                      f"(subset match: {sorted(sig)} ⊆ {sorted(cached_sig)})")
            manager._warm_lib_cache[sig] = cached_name
            return cached_name

    # New signature — install a fresh library.
    import time as _time
    sig_key  = hashlib.sha256(' '.join(sorted(sig)).encode()).hexdigest()[:16]
    lib_name = f"_warmlib_{sig_key}"

    if manager._warm_debug:
        print(f"[WarmManager] installing library {lib_name!r}")
        print(f"[WarmManager]   hoisting: {sorted(sig)}")

    t0 = _time.perf_counter()
    lib = manager.create_library_from_functions(
        lib_name,
        _warm_universal_runner,
        hoisting_modules=modules,
        add_env=False,
    )
    t1 = _time.perf_counter()
    manager.install_library(lib)
    t2 = _time.perf_counter()

    if manager._warm_debug:
        print(f"[WarmManager]   create_library={t1-t0:.4f}s  install_library={t2-t1:.4f}s  (non-blocking; library starts async on worker)")

    manager._warm_lib_cache[sig] = lib_name
    return lib_name


# ─────────────────────────────────────────────────────────────────────────────
# Mount log replay
# ─────────────────────────────────────────────────────────────────────────────

def _warm_replay_log(src, dst):
    """
    Replay every logged field operation from src (PythonTask) onto dst
    (FunctionCall), covering file mounts, resources, and scheduling hints.

    set_env_var is excluded: env vars are propagated via the cloudpickle
    payload to _warm_universal_runner instead of via the task struct, because
    FunctionCalls execute inside an already-running library process where the
    worker cannot inject env vars after process start.

    When _mount_log is None (TASKVINE_WARM_POOL not set), this is a no-op.
    """
    _skip = frozenset({'set_env_var'})
    for (name, args, kwargs) in (src._mount_log or []):
        if name not in _skip:
            getattr(dst, name)(*args, **kwargs)


# ─────────────────────────────────────────────────────────────────────────────
# WarmManager — explicit opt-in subclass (backward compatible)
# ─────────────────────────────────────────────────────────────────────────────

class WarmManager(Manager):
    ##
    # @class ndcctools.taskvine.warm_manager.WarmManager
    #
    # A Manager subclass that enables warm-pool mode explicitly, without
    # requiring TASKVINE_WARM_POOL to be set in the environment.
    #
    # Functionally identical to:
    #   TASKVINE_WARM_POOL=1 python3 my_workflow.py
    # but activated programmatically per-manager instance.
    #
    # @code
    # m = vine.WarmManager(port=9123)
    # t = vine.PythonTask(my_func, arg1, arg2)
    # m.submit(t)   # automatically converted to warm FunctionCall
    # @endcode

    def __init__(self, *args, debug=False, **kwargs):
        # Set env vars before super().__init__ so that:
        #   1. TASKVINE_WARM_POOL activates the Manager.__init__ hook.
        #   2. PythonTask instances created after this point activate _mount_log.
        os.environ['TASKVINE_WARM_POOL'] = '1'
        if debug:
            os.environ['TASKVINE_WARM_DEBUG'] = '1'

        super().__init__(*args, **kwargs)

        # Allow debug=True passed directly to override the env var default.
        if debug:
            self._warm_debug = True
