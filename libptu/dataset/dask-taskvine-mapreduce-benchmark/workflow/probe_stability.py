#!/usr/bin/env python3
"""
Probe stability of the actual callable inside PythonTaskDask._fn_def.

In DaskVine, _fn_def = (execute_graph_vertex, (wrapper, dask_task, task_args, keys_of_files), {})
The real user function is dask_task.func.

Run twice and diff:
  python probe_stability.py > run1.txt
  python probe_stability.py > run2.txt
  diff run1.txt run2.txt
"""
import hashlib
import os
import sys
sys.path.insert(0, '.')

# Build same graph as the benchmark
from dask import delayed
from mapreduce_benchmark import map_file_to_stats, combine_stats_files, final_reduce, chunk_list

INPUT_DIR = "/shared/map-reduce-data"
input_paths = sorted(str(p) for p in __import__('pathlib').Path(INPUT_DIR).glob("*.csv"))

stats_tasks   = [map_file_to_stats(p) for p in input_paths]
grouped_stats = chunk_list(stats_tasks, 2)
combine_tasks = [combine_stats_files(g) for g in grouped_stats]
final_task    = final_reduce(combine_tasks)

# Simulate DaskVine submission to capture _fn_def
from ndcctools.taskvine.dask_executor import DaskVine, PythonTaskDask
import dask

# Get the task graph
graph = dask.base.collections_to_dsk([final_task], optimize_graph=False)
dsk = dict(graph)

print(f"Graph has {len(dsk)} nodes")
print()

def hash_code_obj(code):
    h = hashlib.sha256()
    co = code.co_code if isinstance(code.co_code, bytes) else code.co_code.tobytes()
    h.update(co)
    for c in code.co_consts:
        if hasattr(c, 'co_code'):
            h.update(hash_code_obj(c).encode())
        elif isinstance(c, (str, int, float, bytes, bool, type(None))):
            h.update(repr(c).encode())
    return h.hexdigest()

for key, task_spec in list(dsk.items())[:3]:
    print(f"=== TASK: {key} ===")
    print(f"  type(task_spec) : {type(task_spec)}")

    # Get .func attribute (user callable)
    func = getattr(task_spec, 'func', None)
    if func is None and hasattr(task_spec, '__iter__'):
        try:
            items = list(task_spec)
            func = items[0] if items else None
        except Exception:
            pass

    if func is not None:
        print(f"  func type       : {type(func)}")
        print(f"  func.__qualname__: {getattr(func, '__qualname__', 'N/A')!r}")
        print(f"  func.__module__ : {getattr(func, '__module__', 'N/A')!r}")
        code = getattr(func, '__code__', None)
        if code:
            print(f"  co_code sha256  : {hash_code_obj(code)}")
            print(f"  co_firstlineno  : {code.co_firstlineno}")
        print(f"  id(func)        : {id(func)}  [EXPECTED UNSTABLE]")
    else:
        print(f"  func            : not found directly")
        print(f"  task_spec attrs : {[a for a in dir(task_spec) if not a.startswith('__')]}")

    # task_spec is a tuple: (func, arg1, arg2, ...)
    if isinstance(task_spec, tuple) and len(task_spec) > 1:
        raw_args = task_spec[1:]
        print(f"  raw_args count  : {len(raw_args)}")
        for i, a in enumerate(raw_args):
            print(f"  raw_args[{i}] type={type(a).__name__!r} value={repr(a)[:120]!r}")
            if isinstance(a, (list, tuple)):
                for j, item in enumerate(a):
                    print(f"    [{j}] type={type(item).__name__!r} value={repr(item)[:80]!r}")
    print()
