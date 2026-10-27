"""Run the bounded vector campaign while recording unpaired device telemetry."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--telemetry", required=True, type=Path)
    args, campaign_args = parser.parse_known_args()
    args.telemetry.mkdir(parents=True, exist_ok=True)
    command = shutil.which("nvidia-smi")
    monitor = None
    metadata = {"scope": "whole_device_unpaired_500ms", "available": False,
                "per_frame_alignment": False, "includes_other_processes": True}
    with (args.telemetry / "nvidia-smi.csv").open("w", encoding="utf-8") as log:
        try:
            if command:
                monitor = subprocess.Popen(
                    [command, "--query-gpu=timestamp,name,clocks.current.graphics,clocks.current.memory,temperature.gpu,pstate,power.draw,utilization.gpu",
                     "--format=csv", "--loop-ms=500"], stdout=log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            result = subprocess.run([sys.executable, str(Path(__file__).with_name("run_vector_campaign.py")),
                                     *campaign_args], check=False)
        finally:
            if monitor:
                # Stop only the monitor created above. Never select a process
                # by name or terminate another application's GPU work.
                metadata["available"] = monitor.poll() is None
                if monitor.poll() is None:
                    monitor.terminate()
                try:
                    monitor.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    monitor.kill()
                    monitor.wait()
                metadata["monitor_exit_code"] = monitor.returncode
            (args.telemetry / "metadata.json").write_text(
                json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
