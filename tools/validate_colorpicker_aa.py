#!/usr/bin/env python3
"""Validate the authored Figma ColorPicker checker/alpha ramp against 16x area integration.

This fixture-specific check supplements, rather than replaces, the Slug GPU
pixel tests. Requires Pillow and NumPy. It does not use or claim Figma's native
rasterizer and never modifies the .slugui source.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import re
from PIL import Image, ImageDraw, ImageChops
import numpy as np

def main() -> int:
    root=Path(__file__).resolve().parent.parent
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture",type=Path,default=root/"tests/fixtures/colorpicker_v5.slugui")
    parser.add_argument("--actual",type=Path,required=True,help="GPU capture (PPM or PNG)")
    parser.add_argument("--sample-rate",type=int,default=16)
    parser.add_argument("--max-absolute-error",type=float,default=6.0)
    args=parser.parse_args()
    if not 4 <= args.sample_rate <= 64:
        parser.error("--sample-rate must be from 4 through 64")
    src=args.fixture.read_text(encoding="utf8")
    match=re.search(r"Shape Vector_1_75 \{.*?path-data: \[(.*?)\];",src,re.S)
    if not match:
        raise RuntimeError("Figma checker vector node 1:75 not found in fixture")
    paths=re.findall(r'"([^"]+)"',match.group(1))
    if len(paths)!=8:
        raise RuntimeError(f"Expected 8 authored checker SVG paths, got {len(paths)}")
    # Interior of the actual 29x388 px transparent Frame, excluding border AA.
    left,top,right,bottom=493,33,516,402
    rate=args.sample_rate
    width,height=(right-left)*rate,(bottom-top)*rate
    checker=Image.new("1",(width,height),0)
    contours=0
    for path in paths:
        for section in re.findall(r"M([^M]+?)Z",path):
            points=[(float(x),float(y)) for x,y in
                    re.findall(r"(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)",section)]
            if len(points)<3:
                continue
            # Figma node 1:75 is placed at (458,-189) in this fixture.
            polygon=[((458+x-left)*rate,(-189+y-top)*rate) for x,y in points]
            mask=Image.new("1",(width,height),0)
            ImageDraw.Draw(mask).polygon(polygon,fill=1)
            checker=ImageChops.logical_xor(checker,mask)
            contours+=1
    if contours!=96:
        raise RuntimeError(f"Expected 96 closed vector contours, got {contours}")
    area=np.asarray(checker.convert("L").resize((right-left,bottom-top),Image.Resampling.BOX),
                    dtype=np.float64)/255.0
    rows=np.arange(top,bottom,dtype=np.float64)+0.5
    alpha=np.clip(1.0-(rows-23.0)/388.0,0.0,1.0)[:,None,None]
    gray=(255.0-42.0*area)[:,:,None]
    expected=gray*(1.0-alpha)+np.array([255.0,0.0,0.0])[None,None,:]*alpha
    actual=np.asarray(Image.open(args.actual).convert("RGB"),dtype=np.float64)[top:bottom,left:right]
    if actual.shape!=expected.shape:
        raise RuntimeError(f"Captured image size insufficient: {actual.shape}")
    differences=np.abs(actual-expected)
    maximum=float(differences.max())
    mean=float(differences.mean())
    over5=int(np.count_nonzero(differences>5.0))
    print(f"Checker contours={contours}; 16x-reference-style area integration"
          f" at {rate}x samples")
    print(f"RGB absolute error/255: max={maximum:.4f}; "
          f"mean={mean:.4f}; channels >5={over5}; pixels={(right-left)*(bottom-top)}")
    return int(maximum>args.max_absolute_error)

if __name__=="__main__":
    raise SystemExit(main())
