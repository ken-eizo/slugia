#!/usr/bin/env python3
"""Inspect the exact Figma->SlugUI payload, without modifying either file."""
import argparse
import hashlib
import json
import re
from pathlib import Path

REVISION = "2026-11-22-stroke-v6"
ELEMENT = re.compile(r"^(\s*)(Absolute|Rectangle|Shape|Text|Row|Column)\s+([\w]+)\s+\{$")
ATTRIBUTE = re.compile(r"^\s+([\w-]+):\s+(.*);$")


def inspect(path: Path) -> tuple[bool, int]:
    raw = path.read_bytes()
    lines = raw.decode("utf-8-sig").splitlines()
    revision = next((line.split("exporter-revision:", 1)[1].strip()
                     for line in lines[:8] if "// exporter-revision:" in line), "")
    valid = revision == REVISION
    print(f"FILE: {path}")
    print(f"SHA-256: {hashlib.sha256(raw).hexdigest()}")
    print(f"EXPORTER REVISION: {revision or '(MISSING - old plugin)'}")
    print(f"EXPECTED REVISION: {REVISION}")
    nodes, stack = [], []
    for line_number, line in enumerate(lines, 1):
        decl = ELEMENT.match(line)
        if decl:
            depth = len(decl.group(1))
            while stack and stack[-1][0] >= depth:
                stack.pop()
            node = {"name": decl.group(3), "type": decl.group(2),
                    "depth":depth, "line":line_number, "attributes":{}}
            nodes.append(node)
            stack.append((depth,node))
            continue
        if not stack:
            continue
        matched = ATTRIBUTE.match(line)
        if not matched:
            continue
        key,value=matched.group(1),matched.group(2)
        if key in ("fill","fill-layers","stroke","stroke-align",
                   "transform","x","y","width","height","source-provider-data"):
            if key == "source-provider-data":
                try:
                    stack[-1][1]["metadata"] = json.loads(json.loads(value))
                except (ValueError, TypeError):
                    print(f"WARNING: invalid provider-data on line {line_number}")
            else:
                stack[-1][1]["attributes"][key] = value
    count,flat,mirror_issue=0,0,0
    for node in nodes:
        data=node["attributes"]
        metadata=node.get("metadata",{})
        fill=data.get("fill","")
        audit=metadata.get("gradientAudit",[])
        if ("linear(" in fill or "radial(" in fill or "diamond(" in fill or
            "-stops(" in fill or "fill-layers" in data or audit):
            count+=1
            simple=re.fullmatch(r"(?:linear|radial|diamond)\(\s*(#[\da-fA-F]+),\s*(#[\da-fA-F]+)\s*\)",fill)
            is_flat=bool(simple and simple.group(1).lower()==simple.group(2).lower())
            if is_flat: flat+=1
            print(f"GRADIENT [{node['line']:4}] {node['name']}: {fill[:150] or data.get('fill-layers','')[:150]}")
            if audit:
                for item in audit:
                    print(f"        FIGMA RAW: {item.get('type')}  stops={item.get('stopCount')} "
                          f"positions={item.get('positions')}")
            if is_flat:
                print("        WARNING: paint is mathematically flat: identical endpoints")
        transform=metadata.get("relativeTransform")
        if (isinstance(transform,list) and len(transform)==2 and
            all(isinstance(row,list) and len(row)>=2 for row in transform)):
            a,b=transform[0][0:2]
            c,d=transform[1][0:2]
            if a*d-b*c<0 and "transform" not in data:
                mirror_issue+=1
                print(f"MISSING MIRROR [{node['line']:4}] {node['name']}: "
                      f"source Figma matrix determinant negative, but no SlugUI transform")
    white=[node["name"] for node in nodes if node["attributes"].get("fill")=="#ffffff"]
    print(f"NODE COUNT: {len(nodes)} | GRADIENT NODES: {count} | FLAT: {flat} | "
          f"LOST MIRROR MATRICES: {mirror_issue}")
    if white:print("SOLID WHITE FILLS: "+", ".join(white[:12]))
    if not valid:
        print("FAIL: OLD EXPORTER. Close its Figma plugin window, import")
        print("D:/Developer/slugia/dist/share/slugia/figma-slugui-v6/manifest.json")
        print("using Plugins > Development > Import plugin from manifest..., then run")
        print("'SlugUI Exporter - Stroke v6' and DOWNLOAD A NEW .slugui.")
    return valid, (flat + mirror_issue)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("source",type=Path)
    p.add_argument("--allow-stale",action="store_true")
    args=p.parse_args()
    ok,problems=inspect(args.source)
    if not ok and not args.allow_stale:return 2
    return 0


if __name__=="__main__":
    raise SystemExit(main())
