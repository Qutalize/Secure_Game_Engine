"""Measure the shipped selected-runtime demo. No engine/package mutation or rebuild.
Run serially after other performance tasks. Fixed existing RunMetrics window: 5 s.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
from run import aggregate, child_error_mode, diagnose_timeout, format_exit_code, qpc_clock, read_trace


def measure(package, output, runs):
    package = package.resolve()
    output.mkdir(parents=True, exist_ok=False)
    exe = package / "SelectedGame.exe"
    plan = json.loads((package / "runtime-plan.json").read_text(encoding="utf-8"))
    files = {p.relative_to(package).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in package.rglob("*") if p.is_file()}
    now, frequency = qpc_clock()
    metadata = {"date": "2026-11-01", "timezone": "Asia/Tokyo", "package": str(package),
                "runtime_plan": plan, "file_sha256": files, "qpc_frequency": frequency,
                "runs": runs, "warmup_launches": 1, "sample_seconds": 5,
                "settling_seconds": 0, "input": "no user input, normal real-time loop; no smoke/self-test",
                "limitations": ["VSync ON; CPU update cadence, not GPU execution or input-to-display latency",
                                "Small two-bone fixture, not a large scene or maximum-capacity stress test",
                                "No controlled OS/driver cache, temperature, or background desktop activity"]}
    (output / "manifest.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    rows = []
    for i in range(runs + 1):
        trace = output / f"{i:03d}_selected.csv"
        command = [str(exe), "--metrics", str(trace)]
        row = {"case": "Selected_PBR_RT_Ragdoll_CCD_Signed", "round": i, "warmup_run": i == 0,
               "status": "failed", "trace": trace.name, "exit_code": None, "error": ""}
        with (output / f"{i:03d}_selected.log").open("wb") as log:
            row["launch_ticks"] = now()
            with child_error_mode():
                process = subprocess.Popen(command, cwd=package.parent.parent.parent.parent,
                                           stdout=log, stderr=subprocess.STDOUT,
                                           creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                row["exit_code"] = process.wait(timeout=40)
                row["exit_code_hex"] = format_exit_code(row["exit_code"])
                if row["exit_code"]:
                    raise ValueError("nonzero exit " + row["exit_code_hex"])
                row.update(read_trace(trace, row["launch_ticks"], frequency, 5))
                row["status"] = "ok"
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                row.update(diagnose_timeout(trace, row["launch_ticks"], frequency, 5))
                row["error"] = "timeout 40 s"
            except (OSError, ValueError, KeyError) as error:
                row["error"] = str(error)
            except KeyboardInterrupt:
                process.kill()
                process.wait()
                raise
        rows.append(row)
        (output / "runs.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
        (output / "summary.json").write_text(json.dumps(aggregate(rows), indent=2), encoding="utf-8")
        print(i, row["status"], row["error"], flush=True)
        time.sleep(1)
    after = {p.relative_to(package).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in package.rglob("*") if p.is_file()}
    assert files == after, "Delivered package changed"
    return all(r["status"] == "ok" for r in rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=30)
    args = parser.parse_args()
    if not 1 <= args.runs <= 1000:
        parser.error("runs must be 1..1000")
    raise SystemExit(0 if measure(args.package, args.output, args.runs) else 1)
