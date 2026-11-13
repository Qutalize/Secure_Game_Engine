"""Compare every simulation tick, independently of renderer/frame timing."""
import argparse
import csv
import json
import math
from pathlib import Path

FIELDS = "step,tick,x,z,yaw,pitch,hits,shots,mask,status".split(",")
FLOATS = {"x", "z", "yaw", "pitch"}


def read(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != FIELDS:
            raise ValueError(f"{path}: unexpected schema")
        rows = list(reader)
    if not rows:
        raise ValueError("Empty trace")
    for index, row in enumerate(rows):
        for field in FIELDS:
            row[field] = float(row[field]) if field in FLOATS else int(row[field])
            if not math.isfinite(row[field]):
                raise ValueError("Non-finite state")
        if row["step"] != index:
            raise ValueError("Missing/out-of-order tick")
    return rows


def compare(left, right, tolerance=1e-8):
    if len(left) != len(right):
        raise ValueError(f"Trace length differs: {len(left)} != {len(right)}")
    maximum = dict.fromkeys(FLOATS, 0.0)
    for a, b in zip(left, right):
        for field in FIELDS:
            delta = abs(a[field] - b[field])
            if field in FLOATS:
                maximum[field] = max(maximum[field], delta)
            if delta > (tolerance if field in FLOATS else 0):
                raise ValueError(f"Step {a['step']} {field}: {a[field]} != {b[field]}")
    return {"ticks": len(left), "max_abs_difference": maximum, "tolerance": tolerance}


def verify_scenarios(directory):
    """Independent expected outcomes; matching two equally wrong ports is insufficient."""
    report = {}
    for name in ("validation", "victory", "tour"):
        native = read(directory / f"native_{name}.csv")
        unity = read(directory / f"unity_{name}.csv")
        report[name] = compare(native, unity)
    for prefix in ("native", "unity"):
        validation = read(directory / f"{prefix}_validation.csv")
        assert validation[119]["hits"] == 0 and validation[119]["shots"] == 5, "wall occlusion/cooldown"
        assert -.60 < validation[339]["z"] <= -.55, "must stop before wall"
        assert validation[441]["tick"] == 0 and validation[441]["shots"] == 0, "restart resets state"
        assert validation[-1]["tick"] == 7200 and validation[-1]["status"] == 2, "60-second timeout"
        victory = read(directory / f"{prefix}_victory.csv")
        assert victory[-1]["status"] == 1 and victory[-1]["hits"] == 10 and victory[-1]["mask"] == 1023, "all targets clear"
        assert victory[-1]["shots"] == 10, "one shot per target"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path, nargs="?")
    parser.add_argument("right", type=Path, nargs="?")
    parser.add_argument("--scenarios", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.scenarios:
        report = verify_scenarios(args.scenarios)
    else:
        if args.left is None or args.right is None:
            parser.error("provide two traces or --scenarios DIRECTORY")
        report = compare(read(args.left), read(args.right))
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        args.out.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
