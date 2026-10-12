#!/usr/bin/env python3
"""
Parse compression benchmark JSON files and plot throughput vs file size.
Files expected: results/compio_ALGO_LEVEL.json where ALGO is algorithm name,
LEVEL is signed integer. Plots: X = file size (MB), Y = throughput (MB/s),
log-log axes. Each file becomes a line colored by algorithm and brightness
by level. Legend shows only algorithms with their base colors (no levels).
Outputs algorithms.png at 300 dpi.
"""

import glob
import json
import os
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
from matplotlib.lines import Line2D


def parse_filename(filepath: str):
    """Extract algorithm and level from filename like compio_brotli_0.json."""
    basename = os.path.basename(filepath)
    match = re.match(r'compio_([A-Za-z0-9]+)_(-?\d+)\.json$', basename)
    if not match:
        raise ValueError(f"Filename {basename} does not match expected pattern")
    algo = match.group(1).upper()
    level = int(match.group(2))
    return algo, level


def load_benchmark_data(filepath: str):
    """Load a JSON file and extract (file_size_mb, throughput_mb_s) points."""
    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"JSON decode error: {e}") from e

    points = []
    for bench in data.get('benchmarks', []):
        name = bench.get('name', '')
        if not (name.startswith('BM_compio_OptimalUsage/') and name.endswith('/real_time')):
            continue
        file_size_bytes = bench.get('file_size')
        bytes_per_sec = bench.get('bytes_per_second')
        if file_size_bytes is None or bytes_per_sec is None:
            continue
        file_size_mb = file_size_bytes / 1e6
        throughput_mb_s = bytes_per_sec / 1e6
        points.append((file_size_mb, throughput_mb_s))

    if not points:
        raise RuntimeError(f"No valid BM_compio_OptimalUsage entries found in {filepath}")

    points.sort(key=lambda p: p[0])
    return points


def get_base_color(algo: str, algo_list: list):
    """Assign a distinct base color to each algorithm."""
    # Well-separated colors for up to 4 algorithms (colorblind-friendly)
    distinct_colors = [
        '#d62728',
        '#ff7f0e',
        '#2ca02c',
        '#1f77b4',
    ]
    if len(algo_list) <= len(distinct_colors):
        idx = algo_list.index(algo)
        return distinct_colors[idx]
    else:
        # Fallback to tab10 colormap for more than 4 algorithms
        cmap = plt.cm.get_cmap('tab10', len(algo_list))
        idx = algo_list.index(algo)
        return cmap(idx)


def adjust_brightness(color, brightness):
    brightness = max(0.5, min(brightness, 1.0))
    rgb = mcolors.to_rgb(color)
    hsv = mcolors.rgb_to_hsv(rgb)
    hsv[2] = brightness
    return tuple(mcolors.hsv_to_rgb(hsv))


def main():
    file_pattern = "results/compio*.json"
    files = glob.glob(file_pattern)
    if not files:
        print(f"No files matching '{file_pattern}' found.")
        return

    data_by_key = {}
    algo_levels = defaultdict(list)
    all_algos = set()

    for filepath in files:
        try:
            algo, level = parse_filename(filepath)
        except ValueError as e:
            print(f"Skipping {filepath}: {e}")
            continue
        try:
            points = load_benchmark_data(filepath)
        except Exception as e:
            print(f"Error processing {filepath}: {e}")
            continue

        key = (algo, level)
        data_by_key[key] = points
        algo_levels[algo].append(level)
        all_algos.add(algo)

    if not data_by_key:
        print("No valid data files found.")
        return

    # Sort algorithm names alphabetically
    algo_list = sorted(all_algos)

    plt.figure(figsize=(10, 7))
    ax = plt.gca()

    for (algo, level), points in data_by_key.items():
        base_color = get_base_color(algo, algo_list)
        # LZ4 levels are inverted: lower level = better compression,
        # so negate to keep brightness ordering consistent with other algos
        effective_level = level if algo.upper() != 'LZ4' else -level
        levels_for_algo = sorted(set(algo_levels[algo]))
        if algo.upper() == 'LZ4':
            levels_for_algo = sorted([-lvl for lvl in levels_for_algo])
        if len(levels_for_algo) == 1:
            brightness = 0.75
        else:
            min_lvl, max_lvl = levels_for_algo[0], levels_for_algo[-1]
            norm = (effective_level - min_lvl) / (max_lvl - min_lvl)
            # Inverted: better compression → darker (lower brightness)
            brightness = 1.0 - 0.4 * norm
        color = adjust_brightness(base_color, brightness)

        xs, ys = zip(*points)
        ax.loglog(xs, ys, marker='o', linestyle='-', linewidth=2,
                  markersize=4, color=color, alpha=0.75, label='_nolegend_')

    # Legend handles with base colors only
    legend_handles = []
    for algo in algo_list:
        base_color = get_base_color(algo, algo_list)
        handle = Line2D([0], [0], color=base_color, linewidth=2, label=algo)
        legend_handles.append(handle)

    ax.set_xlabel("Размер файла (МБ)")
    ax.set_ylabel("Пропускная способность (МБ/с)")
    ax.set_title("Запись · Пропускная способность от размера файла (без кэша блоков)")
    ax.grid(True, which='both', linestyle=':', alpha=0.7)

    ax.legend(handles=legend_handles, loc='lower right', fontsize='small')

    plt.tight_layout()
    plt.savefig("algorithms.svg", bbox_inches='tight')
    print("Plot saved as algorithms.svg")


if __name__ == "__main__":
    main()
