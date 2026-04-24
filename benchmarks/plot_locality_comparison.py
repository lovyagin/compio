#!/usr/bin/env python3

import csv
import json
import re
import statistics
import sys
from collections import defaultdict

import matplotlib.pyplot as plt


def usage(argv0: str) -> None:
    print(f"Usage: {argv0} <input_csv> [output_png] [compio_json]")
    print("Default output_png: locality_access_comparison.png")
    print("Optional compio_json: Google Benchmark JSON with BM_compio_OptimalUsage runs")


def parse_csv_backends(input_csv: str) -> dict[str, list[tuple[int, float]]]:
    grouped: dict[tuple[str, int], list[float]] = defaultdict(list)
    with open(input_csv, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        required = {"backend", "n_switch", "throughput_mib_s"}
        if not required.issubset(reader.fieldnames or set()):
            raise RuntimeError(
                f"CSV {input_csv} must contain columns: {', '.join(sorted(required))}"
            )
        for row in reader:
            key = (row["backend"], int(row["n_switch"]))
            grouped[key].append(float(row["throughput_mib_s"]))

    by_backend: dict[str, list[tuple[int, float]]] = defaultdict(list)
    for (backend, n_switch), values in grouped.items():
        by_backend[backend].append((n_switch, statistics.median(values)))
    return by_backend


def parse_compio_json(compio_json: str) -> list[tuple[int, float]]:
    with open(compio_json, encoding="utf-8") as f:
        data = json.load(f)

    rx = re.compile(
        r"^BM_compio_OptimalUsage/(?P<is_write>\d+)/(?P<n_ops>\d+)/(?P<file_size>\d+)/"
        r"(?P<n_switch>\d+)/(?P<gamma_shape>\d+)/(?P<gamma_scale>\d+)/"
        r"(?P<region_size>\d+)/real_time$"
    )
    grouped: dict[int, list[float]] = defaultdict(list)
    for bench in data.get("benchmarks", []):
        if bench.get("run_type") != "iteration":
            continue
        name = bench.get("run_name") or bench.get("name", "")
        m = rx.match(name)
        if not m:
            continue
        if int(m.group("is_write")) != 0:
            continue
        bps = bench.get("bytes_per_second")
        if bps is None:
            continue
        grouped[int(m.group("n_switch"))].append(float(bps) / (1024.0 * 1024.0))

    if not grouped:
        raise RuntimeError(f"No BM_compio_OptimalUsage read runs found in {compio_json}")
    return sorted((n, statistics.median(vals)) for n, vals in grouped.items())


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] == "--help":
        usage(sys.argv[0])
        return 0

    input_csv = sys.argv[1]
    output_png = sys.argv[2] if len(sys.argv) > 2 else "locality_access_comparison.png"
    compio_json = sys.argv[3] if len(sys.argv) > 3 else None

    by_backend = parse_csv_backends(input_csv)
    if compio_json:
        by_backend["compio"] = parse_compio_json(compio_json)

    plt.figure(figsize=(10, 6))
    for backend in sorted(by_backend.keys()):
        points = sorted(by_backend[backend], key=lambda x: x[0])
        x = [p[0] for p in points]
        y = [p[1] for p in points]
        plt.plot(x, y, marker="o", linewidth=2, label=backend)

    x_ticks = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]
    plt.xscale("log", base=2)
    plt.yscale("log")
    plt.xticks(x_ticks)
    plt.xlabel("n_switch (operations before hotspot switch)")
    plt.ylabel("Throughput (MiB/s, median)")
    title_suffix = " (compio from external JSON)" if compio_json else ""
    plt.title(f"Random-access performance vs locality{title_suffix}")
    plt.grid(True, which="both", alpha=0.3)
    plt.legend()

    plt.tight_layout()
    plt.savefig(output_png, dpi=180)
    print(f"Wrote {output_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
