#!/usr/bin/env python3
"""Generate dependency-free SVG graphs from repeated xbench measurements."""

import csv
import html
import statistics
import sys
from collections import defaultdict
from pathlib import Path


WIDTH = 960
HEIGHT = 560
LEFT = 90
RIGHT = 35
TOP = 70
BOTTOM = 120


def read_measurements(path: Path):
    groups = defaultdict(list)
    with path.open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            if row["status"] != "OK" or not row["ticks"].isdigit():
                continue
            key = (
                row["experiment"],
                row["mode"],
                row["processes"],
                row["blocks"],
                row["cpu_count"],
                row["lock_strategy"],
            )
            groups[key].append(int(row["ticks"]))
    return groups


def write_chart(path: Path, title: str, y_label: str, values):
    values = [(label, value, color) for label, value, color in values if value is not None]
    if not values:
        return False
    plot_width = WIDTH - LEFT - RIGHT
    plot_height = HEIGHT - TOP - BOTTOM
    maximum = max(value for _, value, _ in values)
    scale_max = max(1, maximum * 1.15)
    bar_gap = 14
    bar_width = max(8, (plot_width - bar_gap * (len(values) - 1)) / len(values))
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<text x="{WIDTH / 2}" y="35" text-anchor="middle" font-family="sans-serif" font-size="21">{html.escape(title)}</text>',
        f'<text x="22" y="{TOP + plot_height / 2}" transform="rotate(-90 22 {TOP + plot_height / 2})" text-anchor="middle" font-family="sans-serif" font-size="15">{html.escape(y_label)}</text>',
    ]
    for tick in range(5):
        y = TOP + plot_height - plot_height * tick / 4
        value = scale_max * tick / 4
        svg.append(f'<line x1="{LEFT}" y1="{y:.1f}" x2="{WIDTH - RIGHT}" y2="{y:.1f}" stroke="#dddddd"/>')
        svg.append(f'<text x="{LEFT - 10}" y="{y + 5:.1f}" text-anchor="end" font-family="sans-serif" font-size="12">{value:.1f}</text>')
    for index, (label, value, color) in enumerate(values):
        x = LEFT + index * (bar_width + bar_gap)
        bar_height = plot_height * value / scale_max
        y = TOP + plot_height - bar_height
        svg.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_width:.1f}" height="{bar_height:.1f}" fill="{color}"/>')
        svg.append(f'<text x="{x + bar_width / 2:.1f}" y="{y - 7:.1f}" text-anchor="middle" font-family="sans-serif" font-size="11">{value:.1f}</text>')
        svg.append(f'<text x="{x + bar_width / 2:.1f}" y="{TOP + plot_height + 18}" transform="rotate(35 {x + bar_width / 2:.1f} {TOP + plot_height + 18})" text-anchor="start" font-family="sans-serif" font-size="11">{html.escape(label)}</text>')
    svg.extend(["</svg>", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(svg), encoding="utf-8")
    return True


def mean_for(
    groups, experiment, mode=None, processes=None, blocks=None,
    cpu_count=None, lock_strategy=None
):
    values = [
        value
        for (
            name, row_mode, row_processes, row_blocks, row_cpu, row_strategy
        ), samples in groups.items()
        if name == experiment
        and (mode is None or row_mode == mode)
        and (processes is None or row_processes == str(processes))
        and (blocks is None or row_blocks == str(blocks))
        and (cpu_count is None or row_cpu == str(cpu_count))
        and (lock_strategy is None or row_strategy == lock_strategy)
        for value in samples
    ]
    return statistics.mean(values) if values else None


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: plot-benchmarks.py RAW.csv OUTPUT_DIRECTORY", file=sys.stderr)
        return 2
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    if not source.is_file():
        print(f"raw benchmark CSV not found: {source}", file=sys.stderr)
        return 2
    groups = read_measurements(source)
    output.mkdir(parents=True, exist_ok=True)
    generated = []

    configurations = sorted({(key[4], key[5]) for key in groups})
    for cpu_count, strategy in configurations:
        suffix = f"cpu{cpu_count}-{strategy}"
        values = []
        for ordinary, speculative, label in (
            ("file_crwrd", "spec_write_commit", "file write/commit"),
            ("console_write", "spec_console_commit", "console output"),
            ("pipe_wrrd", "spec_pipe_commit", "pipe communication"),
            ("pipe_full_ordinary", "spec_pipe_full_abort", "full-pipe write"),
        ):
            for name, mode, color, series_suffix in (
                (ordinary, "baseline", "#4472c4", " ordinary"),
                (speculative, "spec", "#ed7d31", " speculative"),
            ):
                result = mean_for(
                    groups, name, mode=mode, cpu_count=cpu_count,
                    lock_strategy=strategy
                )
                if result is not None:
                    values.append((label + series_suffix, result, color))
        path = output / f"baseline-vs-speculative-{suffix}.svg"
        if write_chart(
            path, f"Ordinary and speculative batch cost ({suffix})",
            "Mean elapsed ticks per measured batch", values
        ):
            generated.append(path.name)

        values = []
        for experiment, label, color in (
            ("spec_write_commit", "commit", "#4472c4"),
            ("spec_write_abort", "abort", "#ed7d31"),
        ):
            block_counts = sorted(
                {
                    int(key[3])
                    for key in groups
                    if key[0] == experiment and key[1] == "spec"
                    and key[4] == cpu_count and key[5] == strategy
                }
            )
            for block_count in block_counts:
                result = mean_for(
                    groups, experiment, mode="spec", blocks=block_count,
                    cpu_count=cpu_count, lock_strategy=strategy
                )
                if result is not None:
                    values.append((f"{label}, {block_count} blocks", result, color))
        path = output / f"commit-abort-write-set-{suffix}.svg"
        if write_chart(
            path, f"Commit and abort cost by write-set ({suffix})",
            "Mean elapsed ticks per measured batch", values
        ):
            generated.append(path.name)

        values = []
        for experiment, label, color in (
            ("shared_ctx_commit", "shared context", "#4472c4"),
            ("indep_ctx_commit", "independent contexts", "#ed7d31"),
        ):
            process_counts = sorted(
                {
                    int(key[2])
                    for key in groups
                    if key[0] == experiment and key[1] == "concurrency"
                    and key[4] == cpu_count and key[5] == strategy
                }
            )
            for count in process_counts:
                result = mean_for(
                    groups, experiment, mode="concurrency", processes=count,
                    cpu_count=cpu_count, lock_strategy=strategy
                )
                if result is not None:
                    values.append((f"{label}, {count} procs", result, color))
        path = output / f"concurrency-process-count-{suffix}.svg"
        if write_chart(
            path, f"Concurrent context cost ({suffix})",
            "Mean elapsed ticks per measured batch", values
        ):
            generated.append(path.name)

        values = []
        for measurement, label, color in (
            ("baseline", "ordinary write", "#4472c4"),
            ("spec-write", "speculative write", "#ed7d31"),
            ("spec-commit", "commit", "#70ad47"),
            ("spec-abort-write", "abort-path write", "#a5a5a5"),
            ("spec-abort-cleanup", "abort cleanup", "#ffc000"),
        ):
            blocks = sorted(
                {
                    int(key[3])
                    for key in groups
                    if key[0] == "write_sweep" and key[1] == measurement
                    and key[4] == cpu_count and key[5] == strategy
                }
            )
            for block_count in blocks:
                result = mean_for(
                    groups, "write_sweep", mode=measurement, blocks=block_count,
                    cpu_count=cpu_count, lock_strategy=strategy
                )
                if result is not None:
                    values.append((f"{label}, {block_count * 512} B", result, color))
        path = output / f"write-size-sweep-{suffix}.svg"
        if write_chart(
            path, f"Write-size latency ({suffix})", "Mean elapsed ticks",
            values
        ):
            generated.append(path.name)

        values = []
        for measurement, label, color in (
            ("baseline", "ordinary", "#4472c4"),
            ("spec-write", "speculative", "#ed7d31"),
        ):
            blocks = sorted(
                {
                    int(key[3])
                    for key in groups
                    if key[0] == "write_sweep" and key[1] == measurement
                    and key[4] == cpu_count and key[5] == strategy
                }
            )
            for block_count in blocks:
                ticks = mean_for(
                    groups, "write_sweep", mode=measurement, blocks=block_count,
                    cpu_count=cpu_count, lock_strategy=strategy
                )
                if ticks is not None and ticks > 0:
                    throughput = block_count * 512 / ticks
                    values.append(
                        (f"{label}, {block_count * 512} B", throughput, color)
                    )
        path = output / f"write-throughput-{suffix}.svg"
        if write_chart(
            path, f"Successful write throughput ({suffix})",
            "Bytes per xv6 timer tick", values
        ):
            generated.append(path.name)

    if not generated:
        print("No successful measurements found; no graphs were generated.", file=sys.stderr)
        return 1
    (output / "README.txt").write_text(
        "Generated from the raw CSV supplied to plot-benchmarks.py.\n"
        "Only rows with status OK are graphed. No values are embedded in this script.\n"
        + "\n".join(generated)
        + "\n",
        encoding="utf-8",
    )
    print(f"Generated {len(generated)} SVG graph(s) in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
