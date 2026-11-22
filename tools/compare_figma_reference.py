#!/usr/bin/env python3
"""Compare native Figma PNG with Slugia GPU PPM, without accepting old source snapshots.

Requirements: Pillow, numpy. Both images must have the same authored dimensions.
"""
from __future__ import annotations

import argparse
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as error:
    raise SystemExit("Fidelity comparison requires Pillow and numpy: pip install Pillow numpy") from error

REGIONS = {
    "Color area": (23, 23, 411, 411),
    "Hue slider": (436, 23, 465, 411),
    "Alpha slider": (490, 23, 519, 411),
    "Palette preview": (544, 23, 638, 129),
    "Controls": (544, 143, 774, 411),
}


def aligned_arrays(a: np.ndarray, b: np.ndarray, dx: int, dy: int):
    height, width = a.shape[:2]
    x0, x1 = max(0, dx), min(width, width + dx)
    y0, y1 = max(0, dy), min(height, height + dy)
    if x1 <= x0 or y1 <= y0:
        raise ValueError("shift eliminates the entire image")
    return (a[y0:y1, x0:x1], b[y0-dy:y1-dy, x0-dx:x1-dx],
            (x0, y0, x1, y1))


def metrics(reference: np.ndarray, rendered: np.ndarray, alpha: np.ndarray):
    mask = alpha >= 250
    if not np.any(mask):
        raise ValueError("Figma reference has no opaque pixels")
    diff = np.abs(reference.astype(np.int16) - rendered.astype(np.int16))
    worst = np.max(diff, axis=2)
    return {
        "MAE": float(diff[mask].mean()),
        "Max": int(worst[mask].max()),
        "Mismatch>16": float((worst[mask] > 16).mean()*100),
        "Exact": float((worst[mask] == 0).mean()*100),
        "OpaquePixels": int(np.count_nonzero(mask)),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("figma_png", type=Path)
    parser.add_argument("slugia_ppm", type=Path,
                        help="GPU capture from slugia_figma_import_gpu_tests --capture-ppm")
    parser.add_argument("--max-shift", type=int, default=12)
    parser.add_argument("--heatmap", type=Path,
                        help="optional output PNG; by default <slugia filename>.diff.png")
    args = parser.parse_args()
    if args.max_shift < 0 or args.max_shift > 128:
        parser.error("--max-shift must be between 0 and 128")
    reference_image = Image.open(args.figma_png).convert("RGBA")
    rendered_image = Image.open(args.slugia_ppm).convert("RGB")
    if reference_image.size != rendered_image.size:
        parser.error(
            f"Image sizes differ: Figma {reference_image.size}, Slugia {rendered_image.size}. "
            "Use the original 1x Figma Frame export and the same .slugui preview size; "
            "do not resize images to hide layout errors.")
    figma = np.array(reference_image, dtype=np.uint8)
    slugia = np.array(rendered_image, dtype=np.uint8)
    raw = metrics(figma[:,:,:3], slugia, figma[:,:,3])
    print("NATIVE FIGMA PNG vs SLUGIA GPU")
    print(f"Size: {reference_image.width} x {reference_image.height}, "
          f"opaque compared: {raw['OpaquePixels']}")
    print(f"UNSHIFTED MAE: {raw['MAE']:.3f}/255  "
          f"exact: {raw['Exact']:.2f}%  pixels differing >16: {raw['Mismatch>16']:.2f}%")
    best = (float("inf"), 0, 0)
    # Downsample only for alignment search; exact metrics use every pixel.
    for dy in range(-args.max_shift,args.max_shift+1):
        for dx in range(-args.max_shift,args.max_shift+1):
            a,b,_ = aligned_arrays(figma,slugia,dx,dy)
            patch=a[::4,::4]
            other=b[::4,::4]
            mask=patch[:,:,3]>=250
            if not np.any(mask):continue
            score=np.abs(patch[:,:,:3].astype(np.int16)-other.astype(np.int16))[mask].mean()
            if score<best[0]:best=(float(score),dx,dy)
    print(f"BEST ALIGNMENT: dx={best[1]:+d}px dy={best[2]:+d}px")
    a,b,_=aligned_arrays(figma,slugia,best[1],best[2])
    shifted=metrics(a[:,:,:3],b,a[:,:,3])
    print(f"ALIGNED MAE: {shifted['MAE']:.3f}/255  "
          f"exact: {shifted['Exact']:.2f}%  differing >16: {shifted['Mismatch>16']:.2f}%")
    print("REGION REPORT (unshifted, never corrected to hide coordinate errors):")
    for name,(left,top,right,bottom) in REGIONS.items():
        if right>reference_image.width or bottom>reference_image.height:continue
        area=metrics(figma[top:bottom,left:right,:3],
                     slugia[top:bottom,left:right,:],
                     figma[top:bottom,left:right,3])
        print(f"  {name:18} MAE={area['MAE']:6.2f} "
              f"exact={area['Exact']:6.2f}% differing>16={area['Mismatch>16']:6.2f}%")
    # Heatmap only on fully opaque pixels. Aligning a different frame is not
    # treated as success; visually inspect the unshifted source comparison.
    difference=np.abs(figma[:,:,:3].astype(np.int16)-slugia.astype(np.int16))
    magnitude=np.max(difference,axis=2)
    heat=np.zeros((*magnitude.shape,3),dtype=np.uint8)
    heat[:,:,0]=np.where(figma[:,:,3]>=250,np.minimum(magnitude*3,255),0)
    heat[:,:,1]=np.where(figma[:,:,3]>=250,np.minimum(magnitude,255),0)
    output=args.heatmap or args.slugia_ppm.with_suffix(".diff.png")
    Image.fromarray(heat,"RGB").save(output)
    print("PIXEL DIFF HEATMAP:",output)
    print("Reference alignment is a measurement only; inspect remaining gradient,"
          " glyph, clipping, and edge differences separately.")


if __name__=="__main__":
    main()
