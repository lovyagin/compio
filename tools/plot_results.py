#!/usr/bin/env python3
"""
Parse benchmark JSON files and plot throughput vs file size.
Each JSON file becomes one line on the plot, labelled by its filename stem.
X = file size (MB), Y = throughput (MB/s), log-log axes.
Outputs results.png at 300 dpi.

Usage:
    python tools/plot_results.py [results/*.json]
"""

import glob
import json
import os
import sys

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


def load_benchmark_data(filepath: str):
    """Load a JSON file and extract (file_size_mb, throughput_mb_s) points."""
    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"JSON decode error: {e}") from e

    points = []
    for bench in data.get('benchmarks', []):
        file_size_bytes = bench.get('file_size')
        bytes_per_sec = bench.get('bytes_per_second')
        if file_size_bytes is None or bytes_per_sec is None:
            continue
        file_size_mb = file_size_bytes / 1e6
        throughput_mb_s = bytes_per_sec / 1e6
        points.append((file_size_mb, throughput_mb_s))

    if not points:
        raise RuntimeError(f"No valid benchmark entries (with file_size and "
                           f"bytes_per_second) found in {filepath}")

    # points.sort(key=lambda p: p[0])
    return points


def get_label(filepath: str) -> str:
    """Derive a human-readable label from the filename stem."""
    stem = os.path.splitext(os.path.basename(filepath))[0]
    # Replace underscores with spaces for readability
    return stem.replace('_', ' ')


def main():
    # Accept glob pattern from command-line args, default to results/*.json
    if len(sys.argv) > 1:
        pattern = sys.argv[1]
    else:
        pattern = "results/*.json"

    files = sorted(glob.glob(pattern))
    if not files:
        print(f"No files matching '{pattern}' found.")
        return

    # Use a qualitative colormap with enough distinct colours
    cmap = plt.cm.get_cmap('tab10', len(files))

    plt.figure(figsize=(10, 7))
    ax = plt.gca()

    for idx, filepath in enumerate(files):
        try:
            points = load_benchmark_data(filepath)
        except Exception as e:
            print(f"Error processing {filepath}: {e}")
            continue

        label = get_label(filepath)
        color = cmap(idx)

        xs, ys = zip(*points)
        ax.loglog(xs, ys, marker='o', linestyle='-', linewidth=2,
                  markersize=4, color=color, alpha=0.75, label=label)

    if not ax.lines:
        print("No valid data could be plotted.")
        return

    ax.set_xlabel("File Size (MB)")
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title("Read · Throughput vs File Size (no block cache)")
    ax.grid(True, which='both', linestyle=':', alpha=0.7)

    ax.legend(loc='lower right')

    plt.tight_layout()
    out_path = "results.png"
    plt.savefig(out_path, dpi=300, bbox_inches='tight')
    print(f"Plot saved as {out_path}")


if __name__ == "__main__":
    main()
