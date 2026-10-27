"""Report-integrity gates, including a real interrupted child process."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "vector_campaign", Path(__file__).resolve().parents[1] / "tools/run_vector_campaign.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class VectorCampaignTest(unittest.TestCase):
    def setUp(self):
        self.temp_root = Path(tempfile.gettempdir()).resolve()
        self.directory = tempfile.TemporaryDirectory(prefix="vector-report-", dir=self.temp_root)
        self.folder = Path(self.directory.name).resolve()
        self.folder.relative_to(self.temp_root)  # Check the owned cleanup target.
        self.addCleanup(self.directory.cleanup)
        self.output = self.folder / "measurement.json"
        self.header = {"schema": 1, "frame_budget_ms": 16.667,
                       "asset": 'unicode-虎-and-"stages".svg'}
        self.passed = {"copies": 1, "wall_p95_ms": 3.0, "gpu_p95_ms": 2.0, "pass": True}
        self.failed = {"copies": 2, "wall_p95_ms": 25.0, "gpu_p95_ms": 24.0, "pass": False}

    def write(self, text):
        self.output.write_text(text, encoding="utf-8")
        return runner.read_measurement(self.output)

    def prefix(self, stages):
        return json.dumps(self.header, ensure_ascii=False)[:-1] + ',"stages":[' + ",".join(
            json.dumps(stage) for stage in stages)

    def test_complete_failure_remains_failure(self):
        report, complete = self.write(json.dumps(
            dict(self.header, stages=[self.passed, self.failed], stop_reason="frame_budget")))
        self.assertTrue(complete)
        self.assertFalse(report["stages"][-1]["pass"])

    def test_truncated_stage_is_not_recovered_as_success(self):
        report, complete = self.write(self.prefix([self.passed]) + ',{"copies":2,"pass":true')
        self.assertFalse(complete)
        self.assertEqual(report["stages"], [self.passed])
        self.assertEqual(report["stop_reason"], "incomplete_report")

    def test_failure_survives_missing_footer(self):
        report, complete = self.write(self.prefix([self.passed, self.failed]) + "]")
        self.assertFalse(complete)
        self.assertEqual(report["stages"], [self.passed, self.failed])

    def test_contradictory_success_and_partial_metadata_are_rejected(self):
        bad = dict(self.failed, **{"pass": True})
        with self.assertRaises(ValueError):
            self.write(self.prefix([bad]))
        with self.assertRaises(ValueError):
            self.write('{"schema":1,"frame_budget_ms":')

    def test_stale_output_is_not_used(self):
        self.output.write_text("old measurement", encoding="utf-8")
        with self.assertRaises(ValueError):
            runner.run([sys.executable, "-c", "pass"], self.output, 1, 512)

    def test_real_budget_stop_keeps_finished_stages_but_is_incomplete(self):
        script = "from pathlib import Path; import sys,time; Path(sys.argv[1]).write_text(sys.argv[2],encoding='utf-8'); time.sleep(30)"
        command = [sys.executable, "-c", script, str(self.output),
                   self.prefix([self.passed]) + ',{"copies":2']
        real_clock = time.monotonic
        # Advance only after the child has saved its report. This keeps the
        # budget boundary deterministic even on a slow CI worker.
        def clock():
            return real_clock() + (100 if self.output.exists() else 0)
        with patch.object(runner.time, "monotonic", clock):
            result = runner.run(command, self.output, 5, 512)
        self.assertEqual(result["status"], "time_budget")
        self.assertFalse(result["measurement_complete"])
        self.assertTrue(result["recovered_partial_report"])
        self.assertEqual(result["measurement"]["stages"], [self.passed])
        self.assertNotEqual(result["exit_code"], 0)


if __name__ == "__main__":
    unittest.main()
