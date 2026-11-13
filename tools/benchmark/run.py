"""Windows 配布版の起動/フレーム間隔を測る。Editor 起動や画面到達時刻は測らない。"""
import argparse
from contextlib import contextmanager
import csv
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import time


def percentile(values, percent):
    """線形補間 (n-1)*p。単一試行でも定義するが n を常に併記する。"""
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty percentile input")
    index = (len(ordered) - 1) * percent / 100.0
    lo, hi = math.floor(index), math.ceil(index)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (index - lo)


def read_trace(path, launch_ticks, frequency, seconds):
    events, frames = {}, []
    last_tick = launch_ticks
    previous_frame = None
    with Path(path).open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            tick = int(row["qpc_ticks"])
            if row["schema"] != "1" or int(row["qpc_frequency"]) != frequency:
                raise ValueError("schema or QPC frequency mismatch")
            if tick < last_tick:
                raise ValueError("non-monotonic timestamp or wrong clock")
            last_tick = tick
            name = row["name"]
            if row["kind"] == "event":
                if name in events:
                    raise ValueError("duplicate event: " + name)
                events[name] = tick
                if name == "sample_begin":
                    previous_frame = tick
            elif row["kind"] == "frame" and name == "update_interval":
                value = float(row["value_ms"])
                if previous_frame is None or "sample_end" in events or not math.isfinite(value) or value <= 0:
                    raise ValueError("invalid frame sample")
                expected = (tick - previous_frame) * 1000.0 / frequency
                if not math.isclose(value, expected, rel_tol=1e-7, abs_tol=1e-6):
                    raise ValueError("frame interval disagrees with QPC")
                frames.append(value)
                previous_frame = tick
            else:
                raise ValueError("unknown record")
    required = ["scene_ready", "first_update_begin", "sample_begin", "sample_end"]
    if "buffer_overflow" in events or any(name not in events for name in required) or not frames:
        raise ValueError("incomplete run (missing markers/frames or overflow)")
    ticks = [events[name] for name in required]
    if ticks != sorted(ticks):
        raise ValueError("invalid lifecycle order")
    elapsed = (events["sample_end"] - events["sample_begin"]) / frequency
    if elapsed + 1e-6 < seconds or previous_frame != events["sample_end"]:
        raise ValueError("truncated sampling interval")
    return {
        "launch_to_scene_ready_ms": (events["scene_ready"] - launch_ticks) * 1000.0 / frequency,
        "launch_to_first_update_ms": (events["first_update_begin"] - launch_ticks) * 1000.0 / frequency,
        "frame_count": len(frames), "sample_seconds": elapsed,
        "frame_p50_ms": percentile(frames, 50), "frame_p95_ms": percentile(frames, 95),
        "frame_p99_ms": percentile(frames, 99), "frame_max_ms": max(frames),
        "frames_over_33_333ms": sum(value > 1000.0 / 30.0 for value in frames),
        "events": {key: (value - launch_ticks) * 1000.0 / frequency for key, value in events.items()},
    }


def aggregate(rows):
    output = {}
    for name in sorted({row["case"] for row in rows}):
        case_rows = [row for row in rows if row["case"] == name and not row["warmup_run"]]
        valid = [row for row in case_rows if row["status"] == "ok"]
        result = {"attempts": len(case_rows), "valid": len(valid), "failed": len(case_rows) - len(valid)}
        # 試行ごとの値を集計。全試行のフレームを混ぜて「起動のp95」と取り違えない。
        if valid:
            for key in ("launch_to_scene_ready_ms", "launch_to_first_update_ms"):
                values = [row[key] for row in valid]
                result[key] = {"p50": percentile(values, 50), "p95": percentile(values, 95)}
            for key in ("frame_p50_ms", "frame_p95_ms", "frame_p99_ms"):
                result["median_of_run_" + key] = statistics.median(row[key] for row in valid)
        output[name] = result
    return output


def diagnose_timeout(path, launch_ticks, frequency, seconds):
    """A complete CSV does not make a hung process a successful run."""
    try:
        trace = read_trace(path, launch_ticks, frequency, seconds)
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
        return {"timeout_stage": "before_complete_trace", "trace_error": str(error)}
    return {"timeout_stage": "after_sample_end", "recorded_sample_seconds": trace["sample_seconds"]}


@contextmanager
def child_error_mode():
    """Only our child inherits no crash dialog; retain nonzero exit codes as failures.

    A modal Windows application-error dialog otherwise looks like an exit timeout.
    Restore this runner's mode immediately after Popen. No registry/WER policy changes.
    """
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetErrorMode.restype = ctypes.c_uint
    kernel.SetErrorMode.argtypes = [ctypes.c_uint]
    kernel.SetErrorMode.restype = ctypes.c_uint
    original = kernel.GetErrorMode()
    kernel.SetErrorMode(original | 0x0002)  # SEM_NOGPFAULTERRORBOX
    try:
        yield
    finally:
        kernel.SetErrorMode(original)


def format_exit_code(code):
    return f"0x{code & 0xffffffff:08X}"


def validate_case_use(cases, allow_unstable=False):
    issues = [case["name"] + ": " + case["known_issue"] for case in cases if case.get("known_issue")]
    if issues and not allow_unstable:
        raise ValueError("Known unstable case; use --allow-unstable only for diagnosis. " + "; ".join(issues))


def qpc_clock():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    frequency = ctypes.c_longlong()
    if not kernel.QueryPerformanceFrequency(ctypes.byref(frequency)):
        raise OSError("QueryPerformanceFrequency failed")

    def now():
        value = ctypes.c_longlong()
        if not kernel.QueryPerformanceCounter(ctypes.byref(value)):
            raise OSError("QueryPerformanceCounter failed")
        return value.value

    return now, frequency.value


def load_cases(manifest):
    cases = json.loads(manifest.read_text(encoding="utf-8"))["cases"]
    if not cases:
        raise ValueError("manifest has no cases")
    names = set()
    for case in cases:
        name = case["name"]
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name) or name in names:
            raise ValueError("case names must be unique ASCII labels")
        names.add(name)
        command = case["command"]
        if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
            raise ValueError("command must be an argument array")
        if any(arg in ("--metrics", "--benchmark-seconds", "--benchmark-warmup") for arg in command):
            raise ValueError("benchmark arguments are supplied by runner")
        cwd = (manifest.parent / case["cwd"]).resolve()
        exe = (cwd / command[0]).resolve()
        if not cwd.is_dir() or not exe.is_file():
            raise ValueError("missing cwd/executable for " + name)
        if not isinstance(case.get("conditions"), dict) or not case["conditions"]:
            raise ValueError("record build, scenario, graphics and security conditions")
        case["cwd"] = str(cwd)
        case["command"] = [str(exe), *command[1:]]
        case["exe_sha256"] = hashlib.sha256(exe.read_bytes()).hexdigest()
        if case.get("distribution"):
            distribution = (manifest.parent / case["distribution"]).resolve()
            if not distribution.is_dir():
                raise ValueError("missing distribution folder")
            # 容量走査は起動前に実施（キャッシュに影響するため cold と称さない）。
            case["distribution_bytes"] = sum(p.stat().st_size for p in distribution.rglob("*") if p.is_file())
    return cases


def save_results(output, rows):
    (output / "runs.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    fields = ["case", "round", "warmup_run", "status", "exit_code", "exit_code_hex", "error", "launch_ticks",
              "launch_to_scene_ready_ms", "launch_to_first_update_ms", "frame_count", "sample_seconds",
              "frame_p50_ms", "frame_p95_ms", "frame_p99_ms", "frame_max_ms", "frames_over_33_333ms"]
    with (output / "runs.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    (output / "summary.json").write_text(json.dumps(aggregate(rows), indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new directory; never overwrite a run")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--warmup-seconds", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--seed", type=int, default=20261003)
    parser.add_argument("--cooldown", type=float, default=1.0)
    parser.add_argument("--allow-unstable", action="store_true", help="explicitly run known unstable diagnostic cases")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("runner requires Windows QPC")
    if args.runs < 1 or args.warmup_runs < 0 or not 0 < args.seconds <= 300 or not 0 <= args.warmup_seconds <= 300:
        parser.error("invalid run count/duration")
    if not math.isfinite(args.timeout) or args.timeout <= args.seconds + args.warmup_seconds:
        parser.error("timeout must exceed sampling + settling time")
    if not math.isfinite(args.cooldown) or not 0 <= args.cooldown <= 60:
        parser.error("cooldown must be between 0 and 60 seconds")
    cases = load_cases(args.manifest.resolve())
    try:
        validate_case_use(cases, args.allow_unstable)
    except ValueError as error:
        parser.error(str(error))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    now, frequency = qpc_clock()
    metadata = {"schema": 1, "os": platform.platform(), "cpu": platform.processor(), "qpc_frequency": frequency,
                "cache_policy": "uncontrolled; warmup launches recorded separately; NOT cold-start evidence",
                "cases": cases, "options": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}}
    (output / "manifest.resolved.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    rng = random.Random(args.seed)
    rows = []
    try:
        for round_index in range(args.warmup_runs + args.runs):
            order = list(cases)
            rng.shuffle(order)  # 各ラウンドで全ケースを一度ずつ。順序による偏りを抑える。
            for case in order:
                stem = f"{round_index:03d}_{case['name']}"
                trace = output / (stem + ".csv")
                command = [*case["command"], "--metrics", str(trace), "--benchmark-seconds", str(args.seconds),
                           "--benchmark-warmup", str(args.warmup_seconds)]
                row = {"case": case["name"], "round": round_index, "warmup_run": round_index < args.warmup_runs,
                       "status": "failed", "exit_code": None, "error": "", "trace": trace.name}
                with (output / (stem + ".log")).open("wb") as log:
                    row["launch_ticks"] = now()  # OSのプロセス生成時刻ではなく Popen 呼出し直前。
                    try:
                        with child_error_mode():
                            process = subprocess.Popen(command, cwd=case["cwd"], stdout=log, stderr=subprocess.STDOUT,
                                                       creationflags=subprocess.CREATE_NO_WINDOW)
                        try:
                            row["exit_code"] = process.wait(timeout=args.timeout)
                        except (subprocess.TimeoutExpired, KeyboardInterrupt):
                            process.kill()  # このランチャーが起動したプロセスのみ停止する。
                            process.wait()
                            raise
                        row["exit_code_hex"] = format_exit_code(row["exit_code"])
                        if row["exit_code"] != 0:
                            raise ValueError("process exited with nonzero status " + row["exit_code_hex"])
                        row.update(read_trace(trace, row["launch_ticks"], frequency, args.seconds))
                        row["status"] = "ok"
                    except KeyboardInterrupt:
                        row["status"] = "interrupted"
                        row["error"] = "interrupted by user"
                        rows.append(row)
                        raise
                    except subprocess.TimeoutExpired:
                        row.update(diagnose_timeout(trace, row["launch_ticks"], frequency, args.seconds))
                        row["error"] = f"process exit timed out after {args.timeout:g}s ({row['timeout_stage']})"
                    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
                        row["error"] = str(error)
                rows.append(row)
                save_results(output, rows)
                print(stem + ": " + row["status"], flush=True)
                time.sleep(args.cooldown)
    finally:
        save_results(output, rows)
    return 1 if any(row["status"] != "ok" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
