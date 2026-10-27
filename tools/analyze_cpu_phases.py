"""Attribute slow CPU frames to measured intervals, never to unpaired GPU p95."""
import argparse
import json
from pathlib import Path
import statistics

from check_benchmark import CPU_PHASES, quantile, validate_document


def analyze(measurement):
    if measurement.get("cpu_phase_timing_enabled") is not True:
        raise ValueError("CPU phase analysis requires --profile-phases")
    output = []
    for stage in measurement["stages"]:
        wall, phases = stage["raw_wall_ms"], stage["cpu_phases_ms"]
        tail = [i for i, value in enumerate(wall) if value >= quantile(wall, .95)]
        worst = max(range(len(wall)), key=wall.__getitem__)
        serials = stage["raw_submitted_serial"]
        gpu = dict(zip(stage["raw_gpu_sample_serial"], stage["raw_gpu_ms"])) if stage["raw_gpu_ms"] is not None else {}
        tail_sample = stage.get("gpu_completed_tail_sample")
        if tail_sample is not None:
            gpu[tail_sample["submission_serial"]] = tail_sample["milliseconds"]
        tail_means = {name: statistics.mean(phases[name][i] for i in tail) for name in CPU_PHASES}
        outer = {name: tail_means[name] for name in CPU_PHASES[:4]}
        output.append({"copies": stage["copies"], "pass": stage["pass"],
            "wall_p95_ms": stage["wall_p95_ms"], "wall_p99_ms": stage["wall_p99_ms"],
            "gpu_p95_ms": stage["gpu_p95_ms"], "tail_sample_indices": tail,
            "gpu_serial_joined_p95_ms": stage.get("gpu_serial_joined_p95_ms"),
            "tail_cpu_phase_mean_ms": tail_means,
            "dominant_outer_interval": max(outer, key=outer.get),
            "worst_cpu_sample": {"index": worst, "submission_serial": serials[worst], "wall_ms": wall[worst],
                "cpu_intervals_ms": {name: phases[name][worst] for name in CPU_PHASES},
                "resolved_gpu_source_serial_at_cpu_sample": stage["raw_gpu_sample_serial"][worst]
                    if stage["raw_gpu_sample_serial"] is not None else None,
                "resolved_gpu_ms_at_cpu_sample": stage["raw_gpu_ms"][worst]
                    if stage["raw_gpu_ms"] is not None else None,
                "gpu_ms_same_submission_if_collected": gpu.get(serials[worst])},
            "serial_joined_gpu_samples": sum(serial in gpu for serial in serials),
            "missing_same_submission_gpu_samples": sum(serial not in gpu for serial in serials),
            "gpu_wall_p95_difference_used_for_attribution": False})
    return {"scope": "API wall intervals include host descheduling; not active CPU cycles or proof of driver causality",
            "present_mode": measurement.get("present_mode"), "frames_in_flight": measurement.get("frames_in_flight"),
            "stages": output}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.report.stat().st_size > 32 << 20:
        raise ValueError("Report exceeds analysis byte budget")
    document = json.loads(args.report.read_text(encoding="utf-8"))
    validate_document(document)
    measurements = ({name: run["measurement"] for name, run in document["runs"].items()}
                    if "runs" in document else {"measurement": document})
    result = {name: analyze(measurement) for name, measurement in measurements.items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for name, item in result.items():
        stage = item["stages"][-1]
        print(name, "copies", stage["copies"], "wall_p95", stage["wall_p95_ms"],
              "tail intervals", {key: round(value, 4) for key, value in stage["tail_cpu_phase_mean_ms"].items()})


if __name__ == "__main__":
    main()
