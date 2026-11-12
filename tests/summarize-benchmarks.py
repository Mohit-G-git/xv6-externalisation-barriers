#!/usr/bin/env python3
"""Summarize repeated xbench CSV measurements without third-party packages."""

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: summarize-benchmarks.py RAW.csv OUTPUT_DIRECTORY", file=sys.stderr)
        return 2

    raw_path = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    if not raw_path.is_file():
        print(f"raw benchmark CSV not found: {raw_path}", file=sys.stderr)
        return 2
    output_dir.mkdir(parents=True, exist_ok=True)

    groups = defaultdict(list)
    with raw_path.open(newline="", encoding="utf-8") as raw_file:
        for row in csv.DictReader(raw_file):
            key = (
                row["experiment"],
                row["mode"],
                row["processes"],
                row["blocks"],
                row["iterations"],
                row["bytes"],
                row["cpu_count"],
                row["lock_strategy"],
            )
            groups[key].append(row)

    summary_rows = []
    for key, rows in sorted(groups.items()):
        completed = [
            int(row["ticks"])
            for row in rows
            if row["status"] == "OK" and row["ticks"].isdigit()
        ]
        statuses = sorted({row["status"] for row in rows})
        summary_rows.append(
            {
                "experiment": key[0],
                "mode": key[1],
                "processes": key[2],
                "blocks": key[3],
                "iterations": key[4],
                "bytes": key[5],
                "cpu_count": key[6],
                "lock_strategy": key[7],
                "trials": len(rows),
                "valid_trials": len(completed),
                "mean_ticks": f"{statistics.mean(completed):.3f}" if completed else "",
                "median_ticks": f"{statistics.median(completed):.3f}" if completed else "",
                "stdev_ticks": (
                    f"{statistics.stdev(completed):.3f}"
                    if len(completed) > 1
                    else ("0.000" if completed else "")
                ),
                "min_ticks": str(min(completed)) if completed else "",
                "max_ticks": str(max(completed)) if completed else "",
                "statuses": ";".join(statuses),
            }
        )

    summary_path = output_dir / "summary.csv"
    fields = [
        "experiment",
        "mode",
        "processes",
        "blocks",
        "iterations",
        "bytes",
        "cpu_count",
        "lock_strategy",
        "trials",
        "valid_trials",
        "mean_ticks",
        "median_ticks",
        "stdev_ticks",
        "min_ticks",
        "max_ticks",
        "statuses",
    ]
    with summary_path.open("w", newline="", encoding="utf-8") as summary_file:
        writer = csv.DictWriter(summary_file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary_rows)

    all_statuses = [row["status"] for rows in groups.values() for row in rows]
    passed = sum(status == "OK" for status in all_statuses)
    non_ok = len(all_statuses) - passed
    report_path = output_dir / "summary.md"
    with report_path.open("w", encoding="utf-8") as report:
        report.write("# Benchmark summary\n\n")
        report.write(f"- Raw rows: {len(all_statuses)}\n")
        report.write(f"- Successful rows: {passed}\n")
        report.write(f"- Non-OK rows (including expected conflicts): {non_ok}\n")
        report.write("- Timer: xv6 `uptime()` ticks; one tick is the kernel timer interval.\n")
        report.write("- Statistics include only rows with status `OK`; all raw statuses remain in the CSV.\n")
        report.write("- Values are batch elapsed ticks. Divide by `iterations` only for workloads whose `iterations` field denotes repeated operations.\n\n")
        report.write("See `summary.csv` for mean, median, sample standard deviation, minimum, maximum, and trial counts.\n")
        if not all_statuses:
            report.write("\nNo measurements were present in the input file.\n")
    print(f"Wrote {summary_path} and {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
