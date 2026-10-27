"""Audit raw measurement integrity; this never certifies SOTA or image quality."""
from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path


def number(value, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label}: expected a number")
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"{label}: non-finite or negative measurement")
    return value


def quantile(values, probability):
    return sorted(values)[math.ceil(probability * len(values)) - 1]


CPU_PHASES = ("prepare", "events", "record_list", "draw", "fence_wait", "query_read", "acquire",
              "record_commands", "queue_submit", "queue_present", "post_present", "build_ir", "upload")


def validate_cpu_phases(report, stage, count):
    enabled = report.get("cpu_phase_timing_enabled", False)
    if type(enabled) is not bool:
        raise ValueError("CPU phase timing flag must be boolean")
    if not enabled:
        if "cpu_phases_ms" in stage:
            raise ValueError("CPU phase samples contradict the disabled profile")
        return
    if report.get("frontier_eligible") is not False:
        raise ValueError("Instrumented diagnostic profile cannot certify a frontier increase")
    phases, estimates = stage.get("cpu_phases_ms"), stage.get("cpu_phase_p95_ms")
    if not isinstance(phases, dict) or not isinstance(estimates, dict):
        raise ValueError("Missing CPU phase profile")
    if set(phases) != set(CPU_PHASES) or set(estimates) != set(CPU_PHASES):
        raise ValueError("CPU phase set changed or is incomplete")
    for name in CPU_PHASES:
        values = phases[name]
        if not isinstance(values, list) or len(values) != count:
            raise ValueError("CPU phase samples absent or truncated")
        for value in values:
            number(value, name)
        if not math.isclose(number(estimates[name], name), quantile(values, .95), abs_tol=1e-6, rel_tol=1e-8):
            raise ValueError("CPU phase percentile disagrees with its raw samples")
    for i, wall in enumerate(stage["raw_wall_ms"]):
        outer = sum(phases[name][i] for name in CPU_PHASES[:4])
        if not math.isclose(outer, wall, abs_tol=1e-6, rel_tol=1e-8):
            raise ValueError("Same-frame CPU phase sum disagrees with wall time")
        prepare = sum(phases[name][i] for name in ("fence_wait", "query_read", "acquire"))
        draw = sum(phases[name][i] for name in CPU_PHASES[7:])
        # Renderer build/upload counters are float32; account for their
        # serialization/rounding only, without altering any latency gate.
        if (prepare > phases["prepare"][i] + max(1e-4, wall * 1e-6) or
                draw > phases["draw"][i] + max(1e-4, wall * 1e-6)):
            raise ValueError("Nested CPU API intervals exceed their parent interval")
    submitted = stage.get("raw_submitted_serial")
    if (not isinstance(submitted, list) or len(submitted) != count or
            any(type(x) is not int or x <= 0 for x in submitted) or
            any(b != a + 1 for a, b in zip(submitted, submitted[1:]))):
        raise ValueError("Invalid or skipped CPU submission serial")
    source = stage.get("raw_gpu_sample_serial")
    if stage["raw_gpu_ms"] is None:
        if source is not None:
            raise ValueError("Unavailable GPU samples cannot have source serials")
    elif (not isinstance(source, list) or len(source) != count or
          any(type(x) is not int or not 0 < x < current for x, current in zip(source, submitted)) or
          any(b <= a for a, b in zip(source, source[1:]))):
        raise ValueError("GPU sample serial is missing, repeated, or belongs to a future CPU frame")
    drained = report.get("gpu_tail_drained", False)
    if type(drained) is not bool:
        raise ValueError("GPU drain flag must be boolean")
    if drained:
        tail = stage.get("gpu_completed_tail_sample")
        if stage["raw_gpu_ms"] is None:
            if tail is not None or stage.get("gpu_serial_joined_samples") != 0 or stage.get("gpu_serial_joined_p95_ms") is not None:
                raise ValueError("Unavailable GPU tail cannot be joined")
            return
        if (not isinstance(tail, dict) or type(tail.get("submission_serial")) is not int or
                tail["submission_serial"] != submitted[-1]):
            raise ValueError("Final GPU timestamp must belong to the last submitted frame")
        gpu = dict(zip(source, stage["raw_gpu_ms"]))
        gpu[tail["submission_serial"]] = number(tail.get("milliseconds"), "gpu_completed_tail_sample")
        joined = [gpu[serial] for serial in submitted if serial in gpu]
        if stage.get("gpu_serial_joined_samples") != len(joined):
            raise ValueError("Joined GPU coverage contradicts the source serials")
        if len(joined) == count:
            value = number(stage.get("gpu_serial_joined_p95_ms"), "gpu_serial_joined_p95_ms")
            if not math.isclose(value, quantile(joined, .95), abs_tol=1e-6, rel_tol=1e-8):
                raise ValueError("Joined GPU percentile disagrees with serial-matched samples")
            if stage["pass"] and value > report["frame_budget_ms"]:
                raise ValueError("Passing stage exceeds the serial-joined GPU budget")
        elif stage["pass"] or stage.get("gpu_serial_joined_p95_ms") is not None:
            raise ValueError("Incomplete GPU join cannot certify success or a full percentile")


def validate_measurement(report):
    if "asset_source" in report:
        source = report["asset_source"]
        if source not in ("embedded", "file"):
            raise ValueError("Unknown asset source")
        if source == "embedded" and (not isinstance(report.get("embedded_asset_sha256"), str) or
                re.fullmatch(r"[0-9a-f]{64}", report["embedded_asset_sha256"]) is None):
            raise ValueError("Embedded input must disclose its SHA-256")
    # Only the currently instrumented retained workload is supported here.
    # A future dynamic adapter must establish its own update/timing contract.
    if report.get("workload_class") != "static_retained":
        raise ValueError("Unsupported workload claim; dynamic updates need an audited adapter")
    if report.get("geometry_updates_per_frame") != 0:
        raise ValueError("Retained static workload claims geometry mutation")
    if report.get("preprocessing_in_timed_interval") is not False:
        raise ValueError("Retained workload must disclose excluded preprocessing")
    if report.get("readback_in_timed_interval") is not False:
        raise ValueError("Readback timing scope is missing or unsupported")
    if report.get("timer_scope") != "prepare_poll_record_submit_present":
        raise ValueError("Unknown wall timer scope")
    if report.get("gpu_timestamp_frame_alignment") != "asynchronous_unpaired":
        raise ValueError("GPU timestamp alignment contract changed")
    if report.get("percentile_estimator") != "nearest_rank":
        raise ValueError("Unknown percentile estimator")
    if "experimental_compact_curves" in report:
        compact = report["experimental_compact_curves"]
        if type(compact) is not bool or report.get("persistent_curve_stride_bytes") != (24 if compact else 32):
            raise ValueError("Persistent curve layout metadata contradicts its stride")
    if "experimental_packed_bands" in report:
        enabled = report["experimental_packed_bands"]
        packed = report.get("atlas_packed_band_shapes")
        stored = report.get("atlas_storage_shapes")
        if (type(enabled) is not bool or type(packed) is not int or type(stored) is not int or
                not 0 <= packed <= stored or (not enabled and packed != 0)):
            raise ValueError("Packed band shape count contradicts storage metadata")
    if "atlas_refined_shapes_by_cells" in report:
        counts = report["atlas_refined_shapes_by_cells"]
        cells = report.get("atlas_band_cells")
        stored, refined = report.get("atlas_storage_shapes"), report.get("atlas_refined_shapes")
        if (not isinstance(counts, list) or len(counts) not in (6, 7) or
                any(type(x) is not int or x < 0 for x in counts) or
                type(cells) is not int or not 32 <= cells <= 2048 or cells & (cells - 1) or
                type(stored) is not int or type(refined) is not int or
                not 0 <= refined <= stored or sum(counts) != refined or
                any(count and (32 << index) > cells for index, count in enumerate(counts))):
            raise ValueError("Refined band histogram contradicts the requested cells or shape count")
    requested = report.get("samples")
    if type(requested) is not int or requested < 1:
        raise ValueError("Invalid requested sample count")
    budget = number(report.get("frame_budget_ms"), "frame_budget_ms")
    if not budget:
        raise ValueError("Zero frame budget")
    stages = report.get("stages")
    if not isinstance(stages, list) or not stages:
        raise ValueError("No completed measurement stage")
    total = 0
    for stage in stages:
        count = stage.get("measured_samples")
        if type(count) is not int or not 0 < count <= requested:
            raise ValueError("Invalid measured sample count")
        if type(stage.get("pass")) is not bool:
            raise ValueError("Missing stage verdict")
        for raw_name, metrics in (
            ("raw_wall_ms", (("wall_p50_ms", .5), ("wall_p95_ms", .95), ("wall_p99_ms", .99))),
            ("raw_cpu_build_ms", (("cpu_build_p95_ms", .95),)),
            ("raw_gpu_ms", (("gpu_p50_ms", .5), ("gpu_p95_ms", .95), ("gpu_p99_ms", .99))),
        ):
            values = stage.get(raw_name)
            if raw_name == "raw_gpu_ms" and raw_name in stage and values is None:
                if stage["pass"] or any(stage.get(key) is not None for key, _ in metrics):
                    raise ValueError("Unavailable GPU timing cannot pass or report a percentile")
                continue
            if not isinstance(values, list) or len(values) != count:
                raise ValueError(f"{raw_name}: raw samples absent or truncated")
            for value in values:
                number(value, raw_name)
            for key, probability in metrics:
                actual = number(stage.get(key), key)
                # Native reports use 9 significant digits, GPU summaries use
                # 6 fractional digits. Account only for serialization rounding.
                if not math.isclose(actual, quantile(values, probability), abs_tol=1e-6, rel_tol=1e-8):
                    raise ValueError(f"{key}: disagrees with raw nearest-rank percentile")
        if stage["pass"] and (count != requested or stage["wall_p95_ms"] > budget or
                              stage["gpu_p95_ms"] > budget or report.get("validation_errors") != 0):
            raise ValueError("Passing stage violates completeness, latency, or validation gate")
        validate_cpu_phases(report, stage, count)
        total += count
    return {"stage_count": len(stages), "wall_samples": total,
            "workload_class": report["workload_class"], "protocol_valid": True,
            "paired_gpu_wall_samples": False, "global_sota_verified": False}


def validate_document(document):
    if "runs" not in document:
        return {"measurement": validate_measurement(document)}
    if not document["runs"]:
        raise ValueError("Empty campaign")
    result = {}
    for name, run in document["runs"].items():
        if (run.get("status") != "finished" or run.get("exit_code") != 0 or
                run.get("measurement_complete") is not True):
            raise ValueError(f"{name}: interrupted campaign cannot be a complete comparison")
        result[name] = validate_measurement(run["measurement"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    results = {}
    for path in args.reports:
        if path.stat().st_size > 32 << 20:
            raise ValueError("Report exceeds the audit byte budget")
        results[str(path)] = validate_document(json.loads(path.read_text(encoding="utf-8")))
    output = json.dumps({"scope": "measurement_integrity_only", "reports": results}, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output + "\n", encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
