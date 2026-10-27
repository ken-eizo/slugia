"""Double deterministic robustness inputs; retain the first failure and budgets.

This exercises project invariants, not ASAN/TSAN or external conformance suites.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import time

from run_vector_campaign import process_rss


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--doublings", type=int, default=10)
    parser.add_argument("--binary", type=Path, default=Path("build/Release/slugia_fuzz_tests.exe"))
    args = parser.parse_args()
    if not 0 <= args.doublings <= 20:
        parser.error("doublings must be 0..20")
    binary = args.binary.resolve(strict=True)
    root = Path(__file__).resolve().parents[1]
    folder = root / "build/reliability" / dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    folder.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "kind": "deterministic_project_invariants", "seed": "0x51A6B00B",
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "sanitizers_enabled": False, "external_conformance_measured": False,
              "time_budget_seconds_per_stage": 120, "rss_budget_mib": 512, "stages": []}
    for exponent in range(args.doublings + 1):
        scale = 1 << exponent
        start = time.monotonic()
        peak = None
        status = "finished"
        with (folder / f"scale-{scale}.log").open("wb") as log:
            process = subprocess.Popen([str(binary), "--scale", str(scale)], cwd=root,
                                       stdout=log, stderr=subprocess.STDOUT)
            while process.poll() is None:
                rss = process_rss(process)
                if rss is not None:
                    peak = max(peak or 0, rss)
                if time.monotonic() - start > 120:
                    status = "time_budget"
                elif rss is not None and rss > 512 * 1024 * 1024:
                    status = "memory_budget"
                if status != "finished":
                    process.kill()
                    break
                time.sleep(.01)
            code = process.wait()
        stage = {"scale": scale, "utf8_inputs": 20000 * scale,
                 "ime_updates": 5000 * scale, "property_updates": 10000 * scale,
                 "requested_operations": 35000 * scale,
                 "completed_operations": 35000 * scale if status == "finished" and code == 0 else None,
                 "status": status, "exit_code": code, "wall_seconds": time.monotonic() - start,
                 "peak_rss_bytes": peak, "pass": status == "finished" and code == 0}
        report["stages"].append(stage)
        (folder / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"scale={scale} requested={stage['requested_operations']} pass={stage['pass']} "
              f"seconds={stage['wall_seconds']:.3f} rss={peak}", flush=True)
        if not stage["pass"]:
            break
    report["requested_range_complete"] = (
        len(report["stages"]) == args.doublings + 1 and
        all(s["status"] == "finished" for s in report["stages"]))
    report["first_invariant_failure_reached"] = any(s["status"] == "finished" and s["exit_code"] != 0 for s in report["stages"])
    report["stop_reason"] = ("range_end" if report["stages"][-1]["pass"] else
                             report["stages"][-1]["status"] if report["stages"][-1]["status"] != "finished" else "invariant_failure")
    (folder / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (folder.parent / "latest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(folder / "summary.json")


if __name__ == "__main__":
    main()
