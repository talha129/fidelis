#!/usr/bin/env python3
"""
Distributed Image Convolution benchmark.

Applies a convolution kernel to a list of images. For each image, all tiles
are submitted to TaskVine workers, collected, and stitched before moving to
the next image.

Usage
-----
  # Default kernel (sharpen) on default image (npp.jpg):
  python image_convolution_benchmark.py

  # Specific kernel, multiple images:
  python image_convolution_benchmark.py --images npp.jpg img2.jpg img3.jpg --kernel gaussian_blur

  # All kernels applied to each image (one kernel at a time):
  python image_convolution_benchmark.py --images npp.jpg --kernels sharpen gaussian_blur edge_sobel

  # Custom manager:
  python image_convolution_benchmark.py --name dconv --ports 9123 9150

  # Enable warm-pool transparently via env var:
  TASKVINE_WARM_POOL=1 python image_convolution_benchmark.py

Data
----
  Download the default NASA image:
    wget https://svs.gsfc.nasa.gov/vis/a030000/a030000/a030002/npp.jpg
"""

import argparse
import math
import os
import time
from pathlib import Path

import numpy as np
import ndcctools.taskvine as vine
from PIL import Image
from tqdm import tqdm


# ─────────────────────────────────────────────────────────────────────────────
# Convolution function (serialized into PythonTask — keep self-contained)
# ─────────────────────────────────────────────────────────────────────────────

def apply_convolution(tile_np, kernel_np):
    """
    Convolve a single grayscale tile with a 2-D kernel via im2col × GEMM.
    Sent to remote TaskVine workers — must be self-contained.
    """
    import numpy as np

    k = kernel_np.shape[0]
    pad = k // 2
    padded = np.pad(tile_np, pad, mode="reflect")

    H, W = tile_np.shape
    shape = (H, W, k, k)
    strides = (
        padded.strides[0], padded.strides[1],
        padded.strides[0], padded.strides[1],
    )
    patches = np.lib.stride_tricks.as_strided(
        padded, shape=shape, strides=strides, writeable=False
    )
    col = patches.reshape(H * W, -1)
    return (col @ kernel_np.reshape(-1, 1)).reshape(H, W)


# ─────────────────────────────────────────────────────────────────────────────
# Available kernels
# ─────────────────────────────────────────────────────────────────────────────

ALL_KERNELS = {
    "sharpen": np.array([
        [ 0, -1,  0],
        [-1,  5, -1],
        [ 0, -1,  0],
    ], dtype=np.float32),

    "gaussian_blur": (1 / 16) * np.array([
        [1, 2, 1],
        [2, 4, 2],
        [1, 2, 1],
    ], dtype=np.float32),

    "edge_sobel": np.array([
        [-1, 0, 1],
        [-2, 0, 2],
        [-1, 0, 1],
    ], dtype=np.float32),
}


# ─────────────────────────────────────────────────────────────────────────────
# Process one (image, kernel) pair
# ─────────────────────────────────────────────────────────────────────────────

def process_image(m, img_path, kernel_np, tile_size, output_dir):
    """Submit all tiles for one image+kernel, collect, stitch, save."""
    img_gray = Image.open(img_path).convert("L")
    img_np   = np.asarray(img_gray, dtype=np.float32)
    H, W     = img_np.shape
    tiles_x  = math.ceil(W / tile_size)
    tiles_y  = math.ceil(H / tile_size)
    total    = tiles_x * tiles_y

    # Submit
    task_map = {}
    for iy in range(tiles_y):
        for ix in range(tiles_x):
            y0 = iy * tile_size;  y1 = min(y0 + tile_size, H)
            x0 = ix * tile_size;  x1 = min(x0 + tile_size, W)
            tile_np = img_np[y0:y1, x0:x1].copy()
            task = vine.PythonTask(apply_convolution, tile_np, kernel_np)
            task.set_cores(1)
            task_map[m.submit(task)] = (ix, iy)

    # Collect
    results = [[None] * tiles_x for _ in range(tiles_y)]
    failed  = 0
    with tqdm(total=total, desc=f"  tiles") as pbar:
        while task_map:
            t = m.wait(5)
            if t is None or t.id not in task_map:
                continue
            ix, iy = task_map.pop(t.id)
            try:
                results[iy][ix] = t.output
            except Exception as exc:
                print(f"    FAIL tile ({ix},{iy}): {exc}")
                failed += 1
            pbar.update(1)

    # Stitch
    filtered = np.zeros_like(img_np)
    for iy in range(tiles_y):
        for ix in range(tiles_x):
            r = results[iy][ix]
            if r is None:
                continue
            y0 = iy * tile_size;  x0 = ix * tile_size
            filtered[y0:y0 + r.shape[0], x0:x0 + r.shape[1]] = r

    return filtered, total, failed, H, W


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Distributed Image Convolution benchmark")
    parser.add_argument("--images", nargs="+", default=["npp.jpg"],
                        help="Input image paths (default: npp.jpg)")
    parser.add_argument("--kernels", nargs="+", default=["sharpen"],
                        choices=list(ALL_KERNELS.keys()),
                        help="Kernels to apply in order (default: sharpen). "
                             "Each kernel is applied to all images before moving to the next.")
    parser.add_argument("--tile-size", type=int, default=256,
                        help="Tile width/height in pixels (default: 256)")
    parser.add_argument("--output-dir", default="output",
                        help="Directory for filtered images (default: output)")
    parser.add_argument("--name", default=os.environ.get("VINE_MANAGER_NAME", "dconv"),
                        help="TaskVine manager name (default: $VINE_MANAGER_NAME or 'dconv')")
    parser.add_argument("--ports", type=int, nargs="+",
                        default=[int(p.strip()) for p in
                                 os.environ.get("VINE_MANAGER_PORTS", "9123,9150").split(",")],
                        help="TaskVine manager ports (default: $VINE_MANAGER_PORTS or 9123 9150)")
    args = parser.parse_args()

    # Validate inputs
    missing = [p for p in args.images if not Path(p).exists()]
    if missing:
        for p in missing:
            print(f"ERROR: image not found: {p}")
        print("Download default image with:")
        print("  wget https://svs.gsfc.nasa.gov/vis/a030000/a030000/a030002/npp.jpg")
        raise SystemExit(1)

    Path(args.output_dir).mkdir(parents=True, exist_ok=True)

    m = vine.Manager(port=args.ports, name=args.name)
    m.tune("watch-library-logfiles", 1)
    print(f"TaskVine manager: name={args.name!r}, ports={args.ports}")
    print(f"Images:  {args.images}")
    print(f"Kernels: {args.kernels}")
    print(f"Tile:    {args.tile_size}×{args.tile_size} px")

    start_total = time.time()
    summary = []

    # Process image by image, kernel by kernel
    for kernel_name in args.kernels:
        kernel_np = ALL_KERNELS[kernel_name]
        print(f"\n── Kernel: {kernel_name} ──────────────────────────────────")

        for img_path in args.images:
            print(f"  Image: {img_path}")
            t0 = time.time()

            filtered, total, failed, H, W = process_image(
                m, img_path, kernel_np, args.tile_size, args.output_dir
            )

            stem     = Path(img_path).stem
            out_path = Path(args.output_dir) / f"{stem}_{kernel_name}.jpg"
            Image.fromarray(np.clip(filtered, 0, 255).astype("uint8")).save(out_path)

            elapsed = time.time() - t0
            print(f"  Done in {elapsed:.2f}s — saved {out_path}")
            summary.append({
                "image": img_path, "kernel": kernel_name,
                "tiles": total, "failed": failed, "elapsed": elapsed,
                "output": str(out_path),
            })

    total_elapsed = time.time() - start_total

    print("\n" + "=" * 60)
    print(f"All done in {total_elapsed:.2f}s ({total_elapsed/60:.2f} min)")
    print(f"{'Image':<30} {'Kernel':<14} {'Tiles':>6} {'Failed':>6} {'Time':>8}")
    print("-" * 60)
    for s in summary:
        print(f"{s['image']:<30} {s['kernel']:<14} {s['tiles']:>6} {s['failed']:>6} {s['elapsed']:>7.1f}s")

    m.workflow_summary()


if __name__ == "__main__":
    main()
