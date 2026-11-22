# Reproduce actual Figma ColorPicker's checker and clip at multiple zoom levels
# using independent high-resolution even-odd polygon rasterization.
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageChops
import re, json, math, sys
root=Path(__file__).resolve().parent.parent
out=Path(sys.argv[1]) if len(sys.argv)>1 else root/"build"
src=(root/"tests/fixtures/colorpicker_v5.slugui").read_text("utf8")
m=re.search(r'Shape Vector_1_75 \{.*?path-data: \[(.*?)\];',src,re.S)
assert m,"checker path missing"
paths=re.findall(r'"([^"]+)"',m.group(1))
poly=[]
for path in paths:
    for section in re.findall(r'M([^M]+?)Z',path):
        pts=[(float(x),float(y)) for x,y in re.findall(r'(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)',section)]
        if len(pts)>=3: poly.append(pts)
assert len(poly)==96
print("checker contours:",len(poly),flush=True)
results=[]
S=int(sys.argv[2]) if len(sys.argv)>2 else 16
for z in (0.5,0.75,0.9,1.,1.125,1.5,2.):
    gpu=np.asarray(Image.open(out/f"zoom_{z}.ppm").convert("RGB"),dtype=np.float32)
    x0=217; x1=int(math.ceil(220+29*z+3))
    y0=int(math.ceil(20+15*z)); y1=int(min(math.floor(20+365*z),430))
    w=x1-x0; h=y1-y0
    ck=Image.new("1",(w*S,h*S),0)
    for pts in poly:
        # Figma checker local->authored world->zoomed world->crop
        polygon=[((220+(458+x-490)*z-x0)*S,(20+(-189+y-23)*z-y0)*S) for x,y in pts]
        temp=Image.new("1",ck.size,0)
        ImageDraw.Draw(temp).polygon(polygon,fill=1)
        ck=ImageChops.logical_xor(ck,temp)
    checker=np.asarray(ck,dtype=bool)
    highx=x0+(np.arange(w*S,dtype=np.float32)+.5)/S
    highy=y0+(np.arange(h*S,dtype=np.float32)+.5)/S
    fx=(highx>=220) & (highx<220+29*z)
    fy=(highy>=20) & (highy<20+388*z)
    frame=fy[:,None]&fx[None,:]
    alpha=np.clip(1-(highy-20)/(388*z),0,1)
    residual=(1-alpha)[:,None]
    def reduce(v):return np.asarray(v).reshape(h,S,w,S).mean(axis=(1,3))
    cov=reduce(frame)
    resid=reduce(frame*residual)
    checkResid=reduce((checker&frame)*residual)
    expected=np.empty((h,w,3),dtype=np.float32)
    expected[:,:,0]=29+226*cov-42*checkResid
    expected[:,:,1]=29*(1-cov)+255*resid-42*checkResid
    expected[:,:,2]=expected[:,:,1]
    actual=gpu[y0:y1,x0:x1]
    error=np.abs(actual-expected)
    # Ignore the rounded Frame's top and bottom 15 authored px. Remaining is
    # flat vertical sides, and valid for the rectangular clip reference.
    insideX=(np.arange(x0,x1)>=220+2*z)&(np.arange(x0,x1)<=220+29*z-2*z)
    interior=error[:,insideX] if insideX.any() else error
    metric={"zoom":z,"max_error":float(error.max()),"mean_error":float(error.mean()),
      "over5":int((error>5).sum()),"interior_max":float(interior.max()),
      "interior_mean":float(interior.mean()),
      "worst_pixel":tuple(map(int,np.unravel_index(error.argmax(),error.shape)))}
    results.append(metric)
    print(json.dumps(metric),flush=True)
    Image.fromarray(np.uint8(np.clip(expected,0,255))).save(out/f"zoom_reference_{z}.png")
    Image.fromarray(np.uint8(np.clip(error*12,0,255))).save(out/f"zoom_error_{z}.png")
(out/"zoom_metrics.json").write_text(json.dumps(results,indent=2),"utf8")

# This is an independent polygon-area reference, not a native Figma screenshot.
if any(m["max_error"]>8.0 for m in results):
    raise SystemExit("Zoom AA color error exceeds 8/255")
