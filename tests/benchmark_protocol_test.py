"""Prevent false performance/dynamic claims from corrupted or partial reports."""
import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "sota_protocol", Path(__file__).resolve().parents[1] / "tools/check_benchmark.py")
protocol = importlib.util.module_from_spec(spec)
spec.loader.exec_module(protocol)
structure_spec = importlib.util.spec_from_file_location(
    "spirv_structure", Path(__file__).resolve().parents[1] / "tools/compare_spirv_structure.py")
structure = importlib.util.module_from_spec(structure_spec)
structure_spec.loader.exec_module(structure)
analysis_spec = importlib.util.spec_from_file_location(
    "cpu_phase_analysis", Path(__file__).resolve().parents[1] / "tools/analyze_cpu_phases.py")
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
analysis = importlib.util.module_from_spec(analysis_spec)
analysis_spec.loader.exec_module(analysis)


class SotaProtocolTest(unittest.TestCase):
    def setUp(self):
        # Hand-chosen distribution: nearest rank differs from interpolation.
        self.stage = {"copies": 1, "pass": True, "measured_samples": 5,
                      "raw_wall_ms": [1, 2, 3, 4, 10], "wall_p50_ms": 3,
                      "wall_p95_ms": 10, "wall_p99_ms": 10,
                      "raw_cpu_build_ms": [.1] * 5, "cpu_build_p95_ms": .1,
                      "raw_gpu_ms": [0, 1, 2, 3, 9], "gpu_p50_ms": 2,
                      "gpu_p95_ms": 9, "gpu_p99_ms": 9}
        self.report = {"samples": 5, "frame_budget_ms": 16.667, "validation_errors": 0,
                       "workload_class": "static_retained", "geometry_updates_per_frame": 0,
                       "timer_scope": "prepare_poll_record_submit_present",
                       "gpu_timestamp_frame_alignment": "asynchronous_unpaired",
                       "preprocessing_in_timed_interval": False,
                       "readback_in_timed_interval": False, "percentile_estimator": "nearest_rank",
                       "stages": [self.stage]}

    def test_valid_data_is_not_a_sota_certificate(self):
        result = protocol.validate_measurement(self.report)
        self.assertTrue(result["protocol_valid"])
        self.assertFalse(result["global_sota_verified"])
        self.assertFalse(result["paired_gpu_wall_samples"])

    def test_embedded_input_discloses_content_hash(self):
        self.report.update(asset_source="embedded", embedded_asset_sha256="ab" * 32)
        self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_embedded_input_cannot_omit_or_misform_hash(self):
        for value in (None, "", "0" * 63, "z" * 64, True):
            self.report.update(asset_source="embedded", embedded_asset_sha256=value)
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                protocol.validate_measurement(self.report)

    def test_falsified_percentile_rejected(self):
        self.stage["wall_p95_ms"] = 4
        with self.assertRaisesRegex(ValueError, "disagrees"):
            protocol.validate_measurement(self.report)

    def test_nonfinite_and_negative_samples_rejected(self):
        for value in (float("nan"), float("inf"), -1, True):
            with self.subTest(value=value):
                report = copy.deepcopy(self.report)
                report["stages"][0]["raw_wall_ms"][0] = value
                with self.assertRaises(ValueError):
                    protocol.validate_measurement(report)

    def test_missing_or_truncated_raw_data_rejected(self):
        for value in (None, [1, 2, 3, 4]):
            self.stage["raw_wall_ms"] = value
            with self.assertRaisesRegex(ValueError, "absent or truncated"):
                protocol.validate_measurement(self.report)

    def test_short_run_cannot_pass(self):
        self.report["samples"] = 6
        with self.assertRaisesRegex(ValueError, "completeness"):
            protocol.validate_measurement(self.report)
        self.stage["pass"] = False
        self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_unavailable_gpu_is_not_zero_or_success(self):
        self.stage["raw_gpu_ms"] = None
        with self.assertRaisesRegex(ValueError, "Unavailable"):
            protocol.validate_measurement(self.report)
        self.stage.update({"pass": False, "gpu_p50_ms": None, "gpu_p95_ms": None, "gpu_p99_ms": None})
        self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_static_report_cannot_claim_dynamic_geometry(self):
        self.report["workload_class"] = "dynamic_geometry"
        with self.assertRaisesRegex(ValueError, "dynamic updates"):
            protocol.validate_measurement(self.report)

    def test_latency_and_validation_failures_cannot_pass(self):
        self.report["frame_budget_ms"] = 8
        with self.assertRaisesRegex(ValueError, "latency"):
            protocol.validate_measurement(self.report)
        self.report["frame_budget_ms"] = 16.667
        self.report["validation_errors"] = 1
        with self.assertRaisesRegex(ValueError, "validation"):
            protocol.validate_measurement(self.report)

    def test_interrupted_campaign_is_incomplete(self):
        document = {"runs": {"tiger": {"status": "time_budget", "exit_code": 1,
                    "measurement_complete": False, "measurement": self.report}}}
        with self.assertRaisesRegex(ValueError, "interrupted"):
            protocol.validate_document(document)

    def test_compact_layout_claim_must_disclose_actual_stride(self):
        self.report["experimental_compact_curves"] = True
        self.report["persistent_curve_stride_bytes"] = 32
        with self.assertRaisesRegex(ValueError, "stride"):
            protocol.validate_measurement(self.report)
        self.report["persistent_curve_stride_bytes"] = 24
        self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_packed_counter_cannot_exceed_admitted_shapes_or_claim_disabled_work(self):
        self.report.update(experimental_packed_bands=True, atlas_storage_shapes=2, atlas_packed_band_shapes=3)
        with self.assertRaisesRegex(ValueError, "shape count"):
            protocol.validate_measurement(self.report)
        self.report.update(experimental_packed_bands=False, atlas_packed_band_shapes=1)
        with self.assertRaisesRegex(ValueError, "shape count"):
            protocol.validate_measurement(self.report)
        self.report["atlas_packed_band_shapes"] = 0
        self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_refinement_histogram_accepts_legacy_and_2048_reports(self):
        for cells, counts in ((1024, [0, 0, 0, 0, 1, 1]),
                              (2048, [0, 0, 0, 0, 0, 1, 1])):
            self.report.update(atlas_band_cells=cells, atlas_storage_shapes=3,
                               atlas_refined_shapes=2, atlas_refined_shapes_by_cells=counts)
            self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_refinement_histogram_cannot_invent_shapes(self):
        self.report.update(atlas_band_cells=2048, atlas_storage_shapes=1,
                           atlas_refined_shapes=2, atlas_refined_shapes_by_cells=[0, 0, 0, 0, 0, 1, 1])
        with self.assertRaisesRegex(ValueError, "histogram"):
            protocol.validate_measurement(self.report)
        self.report.update(atlas_storage_shapes=3, atlas_refined_shapes=1)
        with self.assertRaisesRegex(ValueError, "histogram"):
            protocol.validate_measurement(self.report)

    def test_refinement_histogram_cannot_exceed_requested_resolution(self):
        self.report.update(atlas_band_cells=1024, atlas_storage_shapes=3,
                           atlas_refined_shapes=1, atlas_refined_shapes_by_cells=[0, 0, 0, 0, 0, 0, 1])
        with self.assertRaisesRegex(ValueError, "histogram"):
            protocol.validate_measurement(self.report)
        self.report.update(atlas_band_cells=2048, atlas_refined_shapes_by_cells=[0] * 8)
        with self.assertRaisesRegex(ValueError, "histogram"):
            protocol.validate_measurement(self.report)

    def add_cpu_phases(self):
        self.report["cpu_phase_timing_enabled"] = True
        self.report["frontier_eligible"] = False
        phases = {name: [0.] * 5 for name in protocol.CPU_PHASES}
        phases["prepare"] = phases["fence_wait"] = [v / 2 for v in self.stage["raw_wall_ms"]]
        phases["draw"] = phases["queue_present"] = [v / 2 for v in self.stage["raw_wall_ms"]]
        self.stage.update(cpu_phases_ms=phases,
            cpu_phase_p95_ms={name: protocol.quantile(values, .95) for name, values in phases.items()},
            raw_submitted_serial=[7, 8, 9, 10, 11], raw_gpu_sample_serial=[6, 7, 8, 9, 10])

    def test_valid_cpu_phases_do_not_claim_paired_gpu_rows(self):
        self.add_cpu_phases()
        result = protocol.validate_measurement(self.report)
        self.assertTrue(result["protocol_valid"])
        self.assertFalse(result["paired_gpu_wall_samples"])

    def test_instrumented_profile_cannot_claim_frontier_eligibility(self):
        self.add_cpu_phases()
        self.report["frontier_eligible"] = True
        with self.assertRaisesRegex(ValueError, "frontier"):
            protocol.validate_measurement(self.report)

    def test_cpu_phase_sum_cannot_change_the_wall_timer(self):
        self.add_cpu_phases()
        self.stage["cpu_phases_ms"]["draw"] = [v + 1 for v in self.stage["cpu_phases_ms"]["draw"]]
        self.stage["cpu_phase_p95_ms"]["draw"] += 1
        with self.assertRaisesRegex(ValueError, "phase sum"):
            protocol.validate_measurement(self.report)

    def test_nested_api_intervals_cannot_exceed_their_parent(self):
        self.add_cpu_phases()
        self.stage["cpu_phases_ms"]["query_read"] = [1.] * 5
        self.stage["cpu_phase_p95_ms"]["query_read"] = 1
        with self.assertRaisesRegex(ValueError, "Nested"):
            protocol.validate_measurement(self.report)

    def test_future_repeated_and_skipped_serials_are_rejected(self):
        for field, values in (("raw_submitted_serial", [7, 8, 10, 11, 12]),
                              ("raw_gpu_sample_serial", [7, 8, 9, 10, 11]),
                              ("raw_gpu_sample_serial", [6, 6, 8, 9, 10])):
            self.add_cpu_phases()
            self.stage[field] = values
            with self.subTest(field=field, values=values), self.assertRaisesRegex(ValueError, "serial"):
                protocol.validate_measurement(self.report)

    def test_missing_phase_data_and_disabled_claim_are_rejected(self):
        self.add_cpu_phases()
        self.stage["cpu_phases_ms"]["events"] = []
        with self.assertRaisesRegex(ValueError, "truncated"):
            protocol.validate_measurement(self.report)
        self.add_cpu_phases()
        self.report["cpu_phase_timing_enabled"] = False
        with self.assertRaisesRegex(ValueError, "disabled"):
            protocol.validate_measurement(self.report)

    def test_unavailable_gpu_has_no_fake_source_serial(self):
        self.add_cpu_phases()
        self.stage.update({"pass": False, "raw_gpu_ms": None, "gpu_p50_ms": None,
                           "gpu_p95_ms": None, "gpu_p99_ms": None})
        with self.assertRaisesRegex(ValueError, "Unavailable GPU"):
            protocol.validate_measurement(self.report)
        self.stage["raw_gpu_sample_serial"] = None
        self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_cpu_tail_analysis_uses_same_indices_and_does_not_fill_missing_gpu(self):
        self.add_cpu_phases()
        protocol.validate_measurement(self.report)
        result = analysis.analyze(self.report)["stages"][0]
        self.assertEqual(result["tail_sample_indices"], [4])
        self.assertEqual(result["serial_joined_gpu_samples"], 4)
        self.assertEqual(result["missing_same_submission_gpu_samples"], 1)
        self.assertIsNone(result["worst_cpu_sample"]["gpu_ms_same_submission_if_collected"])
        self.assertEqual(result["worst_cpu_sample"]["resolved_gpu_source_serial_at_cpu_sample"], 10)
        self.assertEqual(result["worst_cpu_sample"]["resolved_gpu_ms_at_cpu_sample"], 9)
        self.assertFalse(result["gpu_wall_p95_difference_used_for_attribution"])

    def add_gpu_tail(self, milliseconds=12.):
        self.add_cpu_phases()
        self.report["gpu_tail_drained"] = True
        self.stage.update(gpu_completed_tail_sample={"submission_serial": 11, "milliseconds": milliseconds},
                          gpu_serial_joined_samples=5, gpu_serial_joined_p95_ms=milliseconds)

    def test_gpu_tail_completes_serial_coverage_without_a_fake_frame(self):
        self.add_gpu_tail()
        protocol.validate_measurement(self.report)
        result = analysis.analyze(self.report)["stages"][0]
        self.assertEqual(result["serial_joined_gpu_samples"], 5)
        self.assertEqual(result["missing_same_submission_gpu_samples"], 0)
        self.assertEqual(result["worst_cpu_sample"]["gpu_ms_same_submission_if_collected"], 12)

    def test_wrong_gpu_tail_serial_is_rejected(self):
        self.add_gpu_tail()
        self.stage["gpu_completed_tail_sample"]["submission_serial"] = 10
        with self.assertRaisesRegex(ValueError, "last submitted"):
            protocol.validate_measurement(self.report)

    def test_slow_final_gpu_sample_cannot_disappear_from_the_gate(self):
        self.add_gpu_tail(20)
        with self.assertRaisesRegex(ValueError, "GPU budget"):
            protocol.validate_measurement(self.report)
        self.stage["pass"] = False
        self.assertTrue(protocol.validate_measurement(self.report)["protocol_valid"])

    def test_gpu_join_cannot_invent_coverage_or_percentiles(self):
        self.add_gpu_tail()
        self.stage["gpu_serial_joined_samples"] = 6
        with self.assertRaisesRegex(ValueError, "coverage"):
            protocol.validate_measurement(self.report)
        self.add_gpu_tail()
        self.stage["gpu_serial_joined_p95_ms"] = 1
        with self.assertRaisesRegex(ValueError, "percentile"):
            protocol.validate_measurement(self.report)


class SpirvStructureTest(unittest.TestCase):
    source = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %1 "main"
OpExecutionMode %1 OriginUpperLeft
%2 = OpTypeVoid
%3 = OpTypeFunction %2
%5 = OpTypeInt 32 0
%6 = OpConstant %5 16
%1 = OpFunction %2 None %3
%4 = OpLabel
OpReturn
OpFunctionEnd
'''

    def normalized(self, text):
        with tempfile.TemporaryDirectory(prefix="spirv-compare-") as directory:
            path = Path(directory) / "module.txt"
            path.write_text(text, encoding="utf-8")
            return structure.normalize(path)[0]

    def test_id_renaming_spacing_and_comments(self):
        renamed = self.source.replace("%", "%9").replace(" = ", "   =   ")
        self.assertEqual(self.normalized(self.source), self.normalized("; header\n" + renamed))

    def test_quoted_ids_are_not_alpha_renamed(self):
        a = self.source.replace('"main"', '"%1"')
        b = self.source.replace('"main"', '"%9"')
        self.assertNotEqual(self.normalized(a), self.normalized(b))

    def test_numeric_literals_are_not_ids(self):
        self.assertNotEqual(self.normalized(self.source), self.normalized(self.source.replace(" 16\n", " 17\n")))

    def test_different_instruction_is_not_a_match(self):
        self.assertNotEqual(self.normalized(self.source), self.normalized(self.source.replace("OpReturn", "OpKill")))

    def test_empty_or_nonshader_is_rejected(self):
        with self.assertRaises(ValueError):
            self.normalized("; only a comment\n")


if __name__ == "__main__":
    unittest.main()
