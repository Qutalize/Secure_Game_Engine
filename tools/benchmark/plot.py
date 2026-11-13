"""集計 JSON を matplotlib で PNG 化する。無効試行の数も図へ残す。"""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    args = parser.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    results = json.loads(args.summary.read_text(encoding="utf-8"))
    valid = [(name, data) for name, data in results.items() if data["valid"]]
    if not valid:
        parser.error("no valid measurements to plot")
    labels = [f"{name}\nn={data['valid']}; failed={data['failed']}" for name, data in valid]
    fig, axes = plt.subplots(1, 2, figsize=(12, 5), layout="constrained")
    startup = [data["launch_to_first_update_ms"]["p50"] for _, data in valid]
    axes[0].bar(labels, startup, color="#277da1", label="median across launches")
    axes[0].scatter(labels, [data["launch_to_first_update_ms"]["p95"] for _, data in valid],
                    color="#d1495b", marker="_", s=160, label="p95 across launches")
    axes[0].set_title("Launcher call to first update callback")
    axes[0].set_ylabel("ms (not screen-visible readiness)")
    axes[0].legend()
    axes[1].bar(labels, [data["median_of_run_frame_p95_ms"] for _, data in valid], color="#43aa8b")
    axes[1].set_title("Median of each run's p95 frame interval")
    axes[1].set_ylabel("ms (CPU loop cadence, includes waits)")
    for ax in axes:
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", alpha=0.2)
        ax.tick_params(axis="x", labelsize=8)
    fig.suptitle("Instrumented builds | cache uncontrolled | check matched content/settings before comparison", fontsize=10)
    no_valid = [name for name, data in results.items() if not data["valid"]]
    if no_valid:
        # すべて失敗したケースを黙って消さず、値を出せないことを明記する。
        fig.supxlabel("No valid measurements: " + ", ".join(no_valid), fontsize=9, color="#d1495b")
    target = args.summary.with_name("summary.png")
    fig.savefig(target, dpi=160)
    print(target)


if __name__ == "__main__":
    main()
