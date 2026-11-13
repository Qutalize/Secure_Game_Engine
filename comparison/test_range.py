"""Run after building build-range/Release/range_trace.exe."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compare_traces import compare, read

ROOT = Path(__file__).resolve().parents[1]
TRACE = Path(os.environ.get("RANGE_TRACE_EXE", str(ROOT / "build-range/Release/range_trace.exe")))


class RangeTests(unittest.TestCase):
    def run_sim(self, spec=None, replay=None):
        spec = spec if spec is not None else json.loads((ROOT / "comparison/data/range.json").read_text())
        replay = replay if replay is not None else {"version": 1, "segments": [{"ticks": 1}]}
        replay = copy.deepcopy(replay)
        for segment in replay["segments"]:
            for key, default in dict(moveX=0, moveZ=0, yawDelta=0, pitchDelta=0, fire=False, restart=False).items():
                segment.setdefault(key, default)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "spec.json").write_text(json.dumps(spec))
            (directory / "replay.json").write_text(json.dumps(replay))
            result = subprocess.run([str(TRACE),
                                     str(directory / "spec.json"), str(directory / "replay.json"), str(directory / "state.csv")],
                                    capture_output=True, text=True, timeout=10)
            rows = read(directory / "state.csv") if result.returncode == 0 else None
            return result, rows

    def test_clear(self):
        replay = json.loads((ROOT / "comparison/data/victory.json").read_text())
        result, rows = self.run_sim(replay=replay)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((rows[-1]["status"], rows[-1]["shots"], rows[-1]["mask"]), (1, 10, 1023))

    def test_diagonal_speed_is_normalized(self):
        _, rows = self.run_sim(replay={"version": 1, "segments": [{"ticks": 1, "moveX": 1, "moveZ": 1}]})
        self.assertAlmostEqual(rows[-1]["x"]**2+(rows[-1]["z"]+8)**2, (5/120)**2, places=12)

    def test_wall_blocks_shots_and_movement(self):
        _, rows = self.run_sim(replay={"version": 1, "segments": [{"ticks": 400, "moveZ": 1, "fire": True}]})
        self.assertEqual(rows[-1]["hits"], 0)
        self.assertEqual(rows[-1]["shots"], 17)
        self.assertLessEqual(rows[-1]["z"], -.55)
        self.assertGreater(rows[-1]["z"], -.60)

    def test_invalid_spec_rejected(self):
        spec = json.loads((ROOT / "comparison/data/range.json").read_text())
        spec["tickRate"] = 0
        self.assertNotEqual(self.run_sim(spec=spec)[0].returncode, 0)

    def test_oversized_and_invalid_input_rejected(self):
        for command in ({"ticks": 72001}, {"ticks": 2, "moveX": 2}, {"ticks": 0}):
            self.assertNotEqual(self.run_sim(replay={"version": 1, "segments": [command]})[0].returncode, 0)

    def test_comparison_detects_mismatch_and_truncation(self):
        _, rows = self.run_sim(replay={"version": 1, "segments": [{"ticks": 2}]})
        changed = copy.deepcopy(rows)
        changed[0]["hits"] = 1
        with self.assertRaises(ValueError):
            compare(rows, changed)
        with self.assertRaises(ValueError):
            compare(rows, rows[:-1])


if __name__ == "__main__":
    unittest.main()
