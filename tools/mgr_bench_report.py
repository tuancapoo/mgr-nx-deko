"""Summarize native.csv without treating delayed GPU query results as frame timings."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def normalize_row(row):
    row = {key: int(value) for key, value in row.items()}
    # Earlier captures used names implying full GPU busy/idle coverage. Those
    # columns contain the same partial measurements as the explicit names below.
    for name, legacy in (
        ("gpu_marked_work_completed_cumulative_us", "gpu_busy_completed_cumulative_us"),
        ("gpu_between_submissions_completed_cumulative_us", "gpu_gap_completed_cumulative_us"),
    ):
        if name not in row and legacy in row:
            row[name] = row[legacy]
    return row


def percentile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    low = int(index)
    high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (index - low)


def summarize(rows):
    first, last = rows[0], rows[-1]
    wall = [r["frame_wall_us"] / 1000 for r in rows]
    draws = [r["draws"] for r in rows]
    duration_us = last["elapsed_us"] - first["elapsed_us"]
    frames = len(rows) - 1
    cpu_delta = last["process_cpu_cumulative_us"] - first["process_cpu_cumulative_us"]
    gpu_delta = last["gpu_marked_work_completed_cumulative_us"] - first["gpu_marked_work_completed_cumulative_us"]
    between_delta = (last["gpu_between_submissions_completed_cumulative_us"] -
                     first["gpu_between_submissions_completed_cumulative_us"])
    cpu_available = first["process_cpu_cumulative_us"] >= 0 and last["process_cpu_cumulative_us"] >= 0 and cpu_delta >= 0
    return {
        "fps_limit": first["fps_limit"], "from_s": first["elapsed_us"] / 1e6,
        "to_s": last["elapsed_us"] / 1e6, "samples": len(rows),
        "average_fps": 1e6 * frames / duration_us,
        "frame_ms_p50": statistics.median(wall), "frame_ms_p95": percentile(wall, .95),
        "frame_ms_max": max(wall), "draws_mean": statistics.mean(draws),
        "draws_p95": percentile(draws, .95), "draws_max": max(draws),
        "missing_shader_draws": sum(r["missing_shader_draws"] for r in rows),
        "draw_rejections": sum(r["draw_rejections"] for r in rows),
        "process_cpu_ms_per_swap": cpu_delta / 1000 / frames if cpu_available else None,
        "process_cpu_percent_one_core": 100 * cpu_delta / duration_us if cpu_available else None,
        "gpu_marked_work_completed_ms_per_swap": gpu_delta / 1000 / frames if gpu_delta > 0 else None,
        "gpu_between_submissions_completed_ms_per_swap":
            between_delta / 1000 / frames if gpu_delta > 0 and between_delta >= 0 else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="native.csv or its containing session folder")
    parser.add_argument("--warmup-s", type=float, default=10)
    parser.add_argument("--from-s", type=float, default=0)
    parser.add_argument("--to-s", type=float, default=float("inf"))
    args = parser.parse_args()
    source = args.capture / "native.csv" if args.capture.is_dir() else args.capture
    with source.open(encoding="utf-8-sig", newline="") as stream:
        rows = [normalize_row(row) for row in csv.DictReader(stream)]
    # Retain separate contiguous runs: unrelated scenes at the same cap must not be silently merged.
    runs = []
    for row in rows:
        if not runs or runs[-1][-1]["fps_limit"] != row["fps_limit"]:
            runs.append([])
        runs[-1].append(row)
    summaries = []
    for run in runs:
        start_s = max(args.warmup_s, args.from_s, run[0]["elapsed_us"] / 1e6 + 1)
        stable = [r for r in run if start_s <= r["elapsed_us"] / 1e6 <= args.to_s and r["frame_wall_us"] > 0]
        if len(stable) >= 30 and stable[-1]["elapsed_us"] > stable[0]["elapsed_us"]:
            summaries.append(summarize(stable))
    if not summaries:
        raise SystemExit("No stable segment of at least 30 frames in the requested range")
    notes = [
        "CPU is aggregate process CPU across all host threads; it can exceed 100% of one core.",
        "A process CPU value of -1 is unavailable; use logs/rex/rex_profile.log for Switch thread CPU measurements.",
        "GPU averages use cumulative completed timestamp queries; individual frame GPU latency and GPU percentiles are not measured.",
        "GPU marked work covers only timestamped work commands, not full GPU busy time; upload and output work can fall between submissions.",
        "Between-submission time includes untimestamped GPU work as well as idle time; it is not a pure idle measurement.",
        "Switch uses the inherited empirical NVK timestamp correction of 1.627 (PC: 1.0); calibrate on each new device/driver against an independent wall clock.",
        "If no completed GPU time was recorded, GPU time is unavailable rather than zero.",
        "Ring active wall time includes GPU waits and is not CPU execution time.",
        "Segments can include menus/cutscenes. Match the same combat section before comparing caps.",
        "PC results do not establish Nintendo Switch performance or whether 30 FPS preserves gameplay speed.",
    ]
    report = {"source": str(source.resolve()), "segments": summaries, "notes": notes}
    target = source.parent / "perf_summary.json"
    target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    lines = ["# MGR native performance capture", "", f"Source: `{source.resolve()}`", "",
             "| Cap | Interval (s) | FPS | Frame p50 / p95 (ms) | Draw mean / p95 | Missing draws | CPU ms/swap | GPU marked work ms/swap* | Between submissions ms/swap* |",
             "|---|---|---|---|---|---|---|---|---|"]
    for s in summaries:
        cpu = f'{s["process_cpu_ms_per_swap"]:.2f}' if s["process_cpu_ms_per_swap"] is not None else "unavailable"
        gpu = f'{s["gpu_marked_work_completed_ms_per_swap"]:.2f}' if s["gpu_marked_work_completed_ms_per_swap"] is not None else "unavailable"
        between = f'{s["gpu_between_submissions_completed_ms_per_swap"]:.2f}' if s["gpu_between_submissions_completed_ms_per_swap"] is not None else "unavailable"
        lines.append(f'| {s["fps_limit"] or "default"} | {s["from_s"]:.1f}–{s["to_s"]:.1f} | '
                     f'{s["average_fps"]:.2f} | {s["frame_ms_p50"]:.2f} / {s["frame_ms_p95"]:.2f} | '
                     f'{s["draws_mean"]:.1f} / {s["draws_p95"]:.1f} | {s["missing_shader_draws"]} | '
                     f'{cpu} | {gpu} | {between} |')
    lines += ["", *["- " + note for note in notes]]
    (source.parent / "perf_summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
