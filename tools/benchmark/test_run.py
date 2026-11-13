import csv
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, call, patch

from run import aggregate, child_error_mode, diagnose_timeout, format_exit_code, percentile, read_trace, validate_case_use


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "trace.csv"
        self.rows = [
            ["1", "event", "scene_ready", 2000, 1000, ""],
            ["1", "event", "first_update_begin", 2100, 1000, ""],
            ["1", "event", "sample_begin", 2200, 1000, ""],
            ["1", "frame", "update_interval", 2220, 1000, 20],
            ["1", "frame", "update_interval", 2260, 1000, 40],
            ["1", "event", "sample_end", 2260, 1000, ""],
        ]

    def read(self):
        with self.path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(["schema", "kind", "name", "qpc_ticks", "qpc_frequency", "value_ms"])
            writer.writerows(self.rows)
        return read_trace(self.path, 1000, 1000, 0.06)

    def test_external_origin_and_raw_frame_tails(self):
        result = self.read()
        self.assertEqual(result["launch_to_scene_ready_ms"], 1000)
        self.assertEqual(result["launch_to_first_update_ms"], 1100)
        self.assertEqual(result["frame_p50_ms"], 30)
        self.assertEqual(result["frame_p95_ms"], 39)
        self.assertEqual(result["frames_over_33_333ms"], 1)

    def test_missing_ready_cannot_pass(self):
        del self.rows[0]
        with self.assertRaises(ValueError): self.read()

    def test_early_exit_cannot_pass(self):
        self.rows.pop()
        with self.assertRaises(ValueError): self.read()

    def test_wrong_frequency_cannot_pass(self):
        self.rows[0][4] = 10000
        with self.assertRaises(ValueError): self.read()

    def test_clamped_interval_cannot_pass(self):
        self.rows[4][5] = 16.67
        with self.assertRaises(ValueError): self.read()

    def test_nan_cannot_pass(self):
        self.rows[4][5] = "nan"
        with self.assertRaises(ValueError): self.read()

    def test_buffer_overflow_cannot_pass(self):
        self.rows.append(["1", "event", "buffer_overflow", 2300, 1000, ""])
        with self.assertRaises(ValueError): self.read()

    def test_reversed_time_cannot_pass(self):
        self.rows[1][3] = 1500
        with self.assertRaises(ValueError): self.read()

    def test_duplicate_marker_cannot_pass(self):
        self.rows.append(self.rows[-1])
        with self.assertRaises(ValueError): self.read()

    def test_short_sample_cannot_pass(self):
        self.rows[4][3] = self.rows[5][3] = 2250
        self.rows[4][5] = 30
        with self.assertRaises(ValueError): self.read()

    def test_aggregate_keeps_failures_excludes_warmups(self):
        valid = dict(self.read(), case="native", warmup_run=False, status="ok")
        rows = [valid, dict(valid, status="failed"), dict(valid, warmup_run=True, launch_to_first_update_ms=999999)]
        result = aggregate(rows)["native"]
        self.assertEqual(result["attempts"], 2)
        self.assertEqual(result["failed"], 1)
        self.assertEqual(result["launch_to_first_update_ms"]["p50"], 1100)

    def test_percentile_small_sample(self):
        self.assertEqual(percentile([7], 95), 7)
        self.assertEqual(percentile([0, 100], 95), 95)

    def test_timeout_distinguishes_shutdown_from_incomplete_measurement(self):
        self.read()
        self.assertEqual(diagnose_timeout(self.path, 1000, 1000, .06)["timeout_stage"], "after_sample_end")
        self.rows.pop()
        with self.assertRaises(ValueError): self.read()
        self.assertEqual(diagnose_timeout(self.path, 1000, 1000, .06)["timeout_stage"], "before_complete_trace")

    def test_error_mode_preserves_flags_and_restores_after_spawn_failure(self):
        kernel = Mock()
        kernel.GetErrorMode.return_value = 0x8001
        with patch("run.ctypes.WinDLL", return_value=kernel, create=True):
            with self.assertRaises(OSError):
                with child_error_mode():
                    raise OSError("Popen failed")
        self.assertEqual(kernel.SetErrorMode.call_args_list, [call(0x8003), call(0x8001)])

    def test_exception_exit_code_signed_and_unsigned(self):
        self.assertEqual(format_exit_code(3221225477), "0xC0000005")
        self.assertEqual(format_exit_code(-1073741819), "0xC0000005")

    def test_unstable_case_requires_explicit_diagnostic_option(self):
        case = {"name": "unity", "known_issue": "shutdown crash"}
        with self.assertRaisesRegex(ValueError, "shutdown crash"):
            validate_case_use([case])
        validate_case_use([case], allow_unstable=True)
        validate_case_use([{"name": "native"}])


if __name__ == "__main__":
    unittest.main()
