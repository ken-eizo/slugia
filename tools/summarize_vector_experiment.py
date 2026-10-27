"""Archive measured binaries and summarize campaigns without inventing success."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics

from check_benchmark import validate_document


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def telemetry(folder):
    path = folder / "telemetry/nvidia-smi.csv"
    if not path.exists():
        return {"available": False}
    with path.open(encoding="utf-8") as stream:
        rows = [{k.strip(): v.strip() for k, v in row.items()} for row in csv.DictReader(stream)]
    result = {"scope": "whole_device_unpaired", "rows": len(rows), "available": bool(rows)}
    for field, name in (("clocks.current.graphics [MHz]", "graphics_mhz"),
                        ("temperature.gpu", "temperature_c")):
        values = []
        for row in rows:
            try:
                values.append(float(row[field].split()[0]))
            except (KeyError, ValueError):
                pass
        result[name + "_min_median_max"] = [min(values), statistics.median(values), max(values)] if values else None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", action="append", required=True, help="Label=campaign directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = {"schema": 1, "global_sota_verified": False, "campaigns": {}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for item in args.campaign:
        label, folder_text = item.split("=", 1)
        if label in result["campaigns"]:
            raise ValueError("Duplicate experiment label")
        folder = Path(folder_text)
        document = json.loads((folder / "latest.json").read_text(encoding="utf-8"))
        executable = Path(document["binary"])
        expected = document["binary_sha256"]
        snapshot = args.output.parent / (args.output.stem + "-" + expected[:12] + ".exe")
        if snapshot.exists():
            if digest(snapshot) != expected:
                raise ValueError("Refusing to replace a different executable snapshot")
        else:
            if digest(executable) != expected:
                raise ValueError("Measured executable changed and no verified snapshot exists")
            with snapshot.open("xb") as stream:
                stream.write(executable.read_bytes())
        if digest(snapshot) != expected:
            raise ValueError("Executable snapshot failed verification")
        try:
            integrity = validate_document(document)
        except (ValueError, KeyError) as error:
            integrity = {"protocol_valid": False, "error": str(error)}
        runs = {}
        for name, run in document["runs"].items():
            measurement = run.get("measurement") or {}
            stages = measurement.get("stages", [])
            passed = [s for s in stages if s.get("pass") is True]
            failed = [s for s in stages if s.get("pass") is False]
            fields = ("copies", "quads", "wall_p50_ms", "wall_p95_ms", "wall_p99_ms",
                      "gpu_p95_ms", "gpu_p99_ms", "pass", "measured_samples")
            raw = Path(run["log"]).with_suffix(".json")
            asset = Path(measurement["asset"]) if measurement.get("asset") else None
            asset_hash = digest(asset) if asset and asset.is_file() else None
            if measurement.get("asset_source") == "embedded":
                embedded_hash = measurement["embedded_asset_sha256"]
                if asset_hash is not None and asset_hash != embedded_hash:
                    raise ValueError("Source asset changed after it was embedded into the measured binary")
                asset_hash = embedded_hash
            runs[name] = {"status": run["status"], "measurement_complete": run["measurement_complete"],
                          "peak_rss_mib": run["peak_rss_bytes"] / 2**20,
                          "rss_scope": "whole_case_peak_not_stage_specific",
                          "stop_reason": measurement.get("stop_reason"),
                          "measurement_profile": measurement.get("measurement_profile"),
                          "frontier_eligible": measurement.get("frontier_eligible", not measurement.get("cpu_phase_timing_enabled", False)),
                          "last_pass": {k: passed[-1].get(k) for k in fields} if passed else None,
                          "first_fail": {k: failed[0].get(k) for k in fields} if failed else None,
                          "raw_path": str(raw), "raw_sha256": digest(raw) if raw.exists() else None,
                          "asset_sha256": asset_hash, "asset_source": measurement.get("asset_source"),
                          "resources": {k: measurement.get(k) for k in (
                              "atlas_storage_shapes", "atlas_packed_band_shapes", "atlas_refined_shapes",
                              "atlas_refined_shapes_by_cells", "atlas_storage_curve_bytes", "atlas_storage_band_bytes",
                              "renderer_init_ms", "import_ms", "persistent_curve_stride_bytes")}}
        # Resolve the immutable run summary from the log directory, rather than
        # relying only on the mutable convenience pointer latest.json.
        run_summary = Path(next(iter(document["runs"].values()))["log"]).parent / "summary.json"
        result["campaigns"][label] = {"created_utc": document["created_utc"],
            "binary_sha256": expected, "binary_snapshot": str(snapshot), "integrity": integrity,
            "run_summary": str(run_summary), "summary_sha256": digest(run_summary),
            "telemetry": telemetry(folder), "runs": runs}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(args.output)
    for label, campaign in result["campaigns"].items():
        print(label, {name: (run["last_pass"]["copies"] if run["last_pass"] else None,
                            run["first_fail"]["copies"] if run["first_fail"] else None)
                      for name, run in campaign["runs"].items()})


if __name__ == "__main__":
    main()
