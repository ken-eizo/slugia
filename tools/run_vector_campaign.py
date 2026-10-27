"""Bounded, resumable doubling experiments. Failed stages remain failed in JSON."""
from __future__ import annotations
import argparse
import ctypes
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import time


def read_measurement(output: Path) -> tuple[dict, bool]:
    """Read a finished report or strictly recover its complete stage objects.

    A budget stop can interrupt the C++ stream before its closing brackets.
    Recovering a stage never changes its pass flag or completes the run.
    """
    if output.stat().st_size > 1048576:
        raise ValueError("Measurement report exceeds 1 MiB")
    data = output.read_text(encoding="utf-8")
    decoder = json.JSONDecoder()
    complete = True
    try:
        report = json.loads(data)
    except json.JSONDecodeError:
        complete = False
        position = 0

        def whitespace() -> None:
            nonlocal position
            while position < len(data) and data[position] in " \t\r\n":
                position += 1

        def token(expected: str) -> None:
            nonlocal position
            whitespace()
            if position >= len(data) or data[position] != expected:
                raise ValueError("Incomplete measurement metadata")
            position += 1

        report = {}
        token("{")
        while True:
            whitespace()
            key, position = decoder.raw_decode(data, position)
            if not isinstance(key, str) or key in report:
                raise ValueError("Invalid measurement metadata key")
            token(":")
            whitespace()
            if key == "stages":
                token("[")
                stages = []
                while True:
                    whitespace()
                    if position >= len(data) or data[position] == "]":
                        break
                    try:
                        stage, end = decoder.raw_decode(data, position)
                    except json.JSONDecodeError:
                        break  # A truncated object is never a completed stage.
                    stages.append(stage)
                    position = end
                    whitespace()
                    if position >= len(data) or data[position] == "]":
                        break
                    token(",")
                report["stages"] = stages
                report["stop_reason"] = "incomplete_report"
                break
            report[key], position = decoder.raw_decode(data, position)
            token(",")

    if not isinstance(report, dict) or report.get("schema") != 1:
        raise ValueError("Invalid measurement schema")
    budget = report.get("frame_budget_ms")
    if type(budget) not in (int, float) or not math.isfinite(budget) or budget <= 0:
        raise ValueError("Invalid measurement frame budget")
    stages = report.get("stages")
    if not isinstance(stages, list):
        raise ValueError("Invalid measurement stages")
    for stage in stages:
        if not isinstance(stage, dict) or type(stage.get("pass")) is not bool:
            raise ValueError("Incomplete measurement stage")
        copies = stage.get("copies")
        if type(copies) is not int or copies <= 0 or copies & (copies - 1):
            raise ValueError("Invalid measurement copy count")
        wall, gpu = stage.get("wall_p95_ms"), stage.get("gpu_p95_ms")
        if type(wall) not in (int, float) or not math.isfinite(wall) or wall < 0:
            raise ValueError("Invalid measurement wall time")
        if gpu is not None and (type(gpu) not in (int, float) or not math.isfinite(gpu) or gpu < 0):
            raise ValueError("Invalid measurement GPU time")
        if stage["pass"] and (gpu is None or wall > budget or gpu > budget):
            raise ValueError("Passing stage contradicts recorded timings")
    return report, complete and isinstance(report.get("stop_reason"), str)

def process_rss(process: subprocess.Popen) -> int | None:
    if os.name == "nt":
        from ctypes import wintypes
        class Counters(ctypes.Structure):
            _fields_ = [("cb",wintypes.DWORD),("PageFaultCount",wintypes.DWORD)] + [
                (name,ctypes.c_size_t) for name in ("PeakWorkingSetSize","WorkingSetSize",
                "QuotaPeakPagedPoolUsage","QuotaPagedPoolUsage","QuotaPeakNonPagedPoolUsage",
                "QuotaNonPagedPoolUsage","PagefileUsage","PeakPagefileUsage","PrivateUsage")]
        counters= Counters()
        counters.cb=ctypes.sizeof(counters)
        query=ctypes.WinDLL("psapi").GetProcessMemoryInfo
        query.argtypes=[wintypes.HANDLE,ctypes.c_void_p,wintypes.DWORD]
        if query(int(process._handle),ctypes.byref(counters),counters.cb):
            return int(counters.PeakWorkingSetSize)
    status=Path(f"/proc/{process.pid}/status")
    if status.exists():
        for line in status.read_text().splitlines():
            if line.startswith("VmHWM:"): return int(line.split()[1])*1024
    return None

def run(command: list[str], output: Path, seconds: float, memory_mib: float) -> dict:
    if output.exists():
        raise ValueError("Refusing to reuse an existing measurement output")
    log=output.with_suffix(".log")
    start=time.monotonic()
    peak=None
    status="finished"
    # Launch the executable directly so memory belongs to the tested process,
    # rather than to a proxy process. No command shell is involved.
    with log.open("w",encoding="utf-8") as stream:
        process=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
        while process.poll() is None:
            rss=process_rss(process)
            if rss is not None: peak=max(peak or 0,rss)
            if time.monotonic()-start>seconds: status="time_budget"
            elif peak is not None and peak>memory_mib*1048576: status="memory_budget"
            if status!="finished":
                process.terminate()
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired: process.kill();process.wait()
                break
            time.sleep(.05)
    result={"status":status,"exit_code":process.returncode,"wall_seconds":time.monotonic()-start,
            "peak_rss_bytes":peak,"rss_budget_mib":memory_mib,"log":str(log)}
    result["measurement_complete"] = False
    if output.is_file():
        try:
            measurement, complete = read_measurement(output)
            result["measurement"] = measurement
            result["measurement_complete"] = complete and status == "finished" and process.returncode == 0
            result["recovered_partial_report"] = not complete
        except (OSError, UnicodeError, ValueError) as error:
            result["measurement_error"] = str(error)
    return result

def main() -> None:
    parser=argparse.ArgumentParser()
    parser.add_argument("--build",type=Path,default=Path("build"))
    parser.add_argument("--binary",type=Path,
                        help="Measure another locally built viewer with the same campaign budgets")
    parser.add_argument("--results",type=Path,default=Path("build/campaign"))
    parser.add_argument("--doublings",type=int,default=12)
    parser.add_argument("--samples",type=int,default=40)
    parser.add_argument("--seconds",type=float,default=120)
    parser.add_argument("--rss-mib",type=float,default=512)
    parser.add_argument("--path-tiles",action="store_true",
                        help="Measure the disabled tile prototype separately from the standard path")
    parser.add_argument("--baseline",action="store_true",
                        help="Reset a viewer's performance preset before applying explicit campaign options")
    parser.add_argument("--atlas-storage",action="store_true")
    parser.add_argument("--atlas-bands",action="store_true")
    parser.add_argument("--indexed-quads",action="store_true")
    parser.add_argument("--compact-curves",action="store_true")
    parser.add_argument("--packed-bands",action="store_true")
    parser.add_argument("--profile-phases",action="store_true")
    parser.add_argument("--atlas-band-cells",type=int,choices=(32,64,128,256,512,1024,2048),
                        help="Enable conservative atlas refinement with this query size")
    args=parser.parse_args()
    if args.atlas_band_cells is not None:
        args.atlas_bands=True
    else:
        args.atlas_band_cells=128
    if not 0<=args.doublings<=24 or not 8<=args.samples<=200 or not 10<=args.seconds<=600 or not 128<=args.rss_mib<=1024:
        parser.error("Campaign options exceed the work budget")
    exe=args.binary or (args.build/"Release/slugia_svg_benchmarks.exe" if os.name=="nt" else args.build/"slugia_svg_benchmarks")
    if not exe.is_file(): raise SystemExit(f"Build the SVG benchmark first: {exe}")
    args.results.mkdir(parents=True,exist_ok=True)
    stamp=dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    folder=args.results/stamp
    folder.mkdir()
    report={"schema":1,"created_utc":stamp,"host":platform.platform(),"machine":platform.machine(),
            "binary":str(exe.resolve()),
            "binary_sha256":hashlib.sha256(exe.read_bytes()).hexdigest(),
            "experimental_path_tiles":args.path_tiles,
            "experimental_atlas_storage":args.atlas_storage or args.atlas_bands,"runs":{}}
    report["experimental_atlas_bands"]=args.atlas_bands
    report["atlas_band_cells"]=args.atlas_band_cells
    report["experimental_indexed_quads"]=args.indexed_quads
    report["experimental_compact_curves"]=args.compact_curves
    report["experimental_packed_bands"]=args.packed_bands
    report["cpu_phase_timing_enabled"]=args.profile_phases
    report["frontier_eligible"]=not args.profile_phases
    report["requested_configuration"]={key:report.get(key) for key in (
        "experimental_path_tiles", "experimental_atlas_storage", "experimental_atlas_bands",
        "atlas_band_cells", "experimental_indexed_quads", "experimental_compact_curves",
        "experimental_packed_bands", "cpu_phase_timing_enabled")}
    report["requested_configuration"]["baseline"]=args.baseline
    cases=[("tiger",[]),("tiger-low-spec",["--low-spec"]),("tiger-overdraw",["--overdraw"])]
    paris=args.build/"paris-30k.svg"
    if paris.is_file(): cases.append(("paris",["--svg",str(paris)]))
    else: report["paris_status"]="corpus_not_downloaded"
    for name,extra in cases:
        output=folder/f"{name}.json"
        command=[str(exe.resolve()),"--benchmark","--doublings",str(args.doublings),"--samples",str(args.samples),
                 "--seconds",str(args.seconds),"--memory-mib",str(min(args.rss_mib,1024)),"--output",str(output),*extra]
        if args.baseline: command.append("--baseline")
        if args.path_tiles: command.append("--path-tiles")
        if args.atlas_storage: command.append("--atlas-storage")
        if args.indexed_quads: command.append("--indexed-quads")
        if args.compact_curves: command.append("--compact-curves")
        if args.packed_bands: command.append("--packed-bands")
        if args.profile_phases: command.append("--profile-phases")
        if args.atlas_bands:
            command.extend(("--atlas-bands","--atlas-band-cells",str(args.atlas_band_cells)))
        result=run(command,output,args.seconds+45,args.rss_mib)
        report["runs"][name]=result
        # Viewer binaries may provide defaults of their own. Top-level actual
        # settings come from measurements, while CLI requests remain separate.
        for key in report["requested_configuration"]:
            if key == "baseline": continue
            values=[r.get("measurement",{}).get(key) for r in report["runs"].values()]
            report[key]=values[0] if values and values[0] is not None and all(v==values[0] for v in values) else None
        (folder/"summary.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
        stages=result.get("measurement",{}).get("stages",[])
        passed=[s["copies"] for s in stages if s["pass"]]
        failed=[s["copies"] for s in stages if not s["pass"]]
        print(f"{name}: passed through {max(passed,default=0)} copies; failed at {failed[:1]}; {result['status']}; peak RSS {result['peak_rss_bytes']}",flush=True)
        for label,selected in (("last pass",[s for s in stages if s["pass"]][-1:]),
                               ("first fail",[s for s in stages if not s["pass"]][:1])):
            for stage in selected:
                print(f"  {label}: {stage['copies']} copies, {stage.get('quads', 'unknown')} paths; "
                      f"frame p95 {stage['wall_p95_ms']} ms, GPU p95 {stage['gpu_p95_ms']} ms",flush=True)
        print(f"  stop: {result.get('measurement',{}).get('stop_reason','unavailable')}; "
              f"complete: {result.get('measurement_complete',False)}",flush=True)
    latest=args.results/"latest.json"
    latest.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print(latest)

if __name__=="__main__": main()
