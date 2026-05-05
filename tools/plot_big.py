import json
import glob
import re
import os
from collections import defaultdict

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
from matplotlib.lines import Line2D
from matplotlib.collections import LineCollection


# ----------------------------------------------------------------------
# Colour & marker helpers (same as before, with LZ4 fix)
# ----------------------------------------------------------------------
def get_base_color(algo: str, algo_list: list):
    distinct_colors = [
        '#d62728',   # red
        '#ff7f0e',   # orange
        '#2ca02c',   # green
        '#1f77b4',   # blue
    ]
    if len(algo_list) <= len(distinct_colors):
        idx = algo_list.index(algo)
        return distinct_colors[idx]
    else:
        cmap = plt.cm.get_cmap('tab10', len(algo_list))
        idx = algo_list.index(algo)
        return cmap(idx)


def get_marker(algo: str):
    markers = {
        'brotli': 'o',
        'lz4':    's',
        'zlib':   '^',
        'zstd':   'D',
    }
    return markers.get(algo, 'o')


def adjust_brightness(color, brightness):
    brightness = np.clip(brightness, 0.5, 1.0)
    rgb = mcolors.to_rgb(color)
    hsv = mcolors.rgb_to_hsv(rgb)
    hsv[2] = brightness
    return tuple(mcolors.hsv_to_rgb(hsv))


def get_point_colour(algo, level, algo_levels_map, algo_list, highlight_levels):
    base_col = get_base_color(algo, algo_list)
    if (algo, level) in highlight_levels:
        return base_col

    effective_level = level if algo != 'lz4' else -level
    levels = sorted(algo_levels_map[algo])
    if algo == 'lz4':
        levels = sorted([-lvl for lvl in levels])

    if len(levels) == 1:
        brightness = 0.75
    else:
        min_lvl, max_lvl = levels[0], levels[-1]
        norm = (effective_level - min_lvl) / (max_lvl - min_lvl)
        brightness = 0.6 + 0.4 * norm
    return adjust_brightness(base_col, brightness)


# ----------------------------------------------------------------------
# Benchmark name parser
# ----------------------------------------------------------------------
def parse_benchmark_name(name):
    if name.endswith("/real_time"):
        name = name[:-len("/real_time")]
    parts = name.split('/')
    if len(parts) != 10:
        return None
    prefix = parts[0]
    if not prefix.startswith("BM_") or not prefix.endswith("_OptimalUsage"):
        return None
    try:
        params = {
            'is_write': int(parts[1]),
            'n_operations': int(parts[2]),
            'file_size_param': int(parts[3]),
            'n_switch': int(parts[4]),
            'gamma_shape': float(parts[5]),
            'gamma_scale': float(parts[6]),
            'region_size': float(parts[7]),
            'disable_cache': int(parts[8]),
            'block_size': int(parts[9]),
        }
    except ValueError:
        return None
    return params


# ----------------------------------------------------------------------
# EXACT AXIS SETUP FROM YOUR WORKING SCRIPT
# ----------------------------------------------------------------------
def setup_cache_hit_axis(ax, cache_hits_pct):
    """
    Set up X-axis to emphasize high cache hit rates (near 100%).
    This is copied verbatim from your working script.
    """
    def transform_cache_hit(x):
        if x >= 100:
            return 2.0
        else:
            return -np.log10(100 - x)

    transformed_hits = [transform_cache_hit(h) for h in cache_hits_pct]

    tick_percentages = [0, 50, 90, 95, 98, 99, 99.5, 99.9, 100]
    tick_positions = [transform_cache_hit(p) for p in tick_percentages]
    tick_labels = [f'{p:.2f}%' if p >= 99 else f'{p:.0f}%' for p in tick_percentages]

    ax.set_xticks(tick_positions)
    ax.set_xticklabels(tick_labels, rotation=45, ha='right')
    ax.set_xlabel('Cache Hit Rate (%)')

    min_transformed = min(transformed_hits) if transformed_hits else transform_cache_hit(0)
    max_transformed = max(transformed_hits) if transformed_hits else transform_cache_hit(100)
    ax.set_xlim(min_transformed - 0.1, max_transformed + 0.1)

    return transformed_hits


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------
def main():
    data_dir = "results_fuck"
    stdio_path = os.path.join(data_dir, "stdio.json")
    compio_pattern = os.path.join(data_dir, "compio_*.json")

    # Load stdio
    stdio_throughput = {}
    stdio_actual_file_size = None
    if not os.path.exists(stdio_path):
        print("Error: stdio.json not found.")
        return
    with open(stdio_path, 'r') as f:
        stdio_data = json.load(f)
    for bench in stdio_data.get("benchmarks", []):
        name = bench.get("name")
        params = parse_benchmark_name(name)
        if params is None:
            continue
        key = (params['is_write'], params['n_operations'], params['file_size_param'],
               params['n_switch'], params['gamma_shape'], params['gamma_scale'],
               params['region_size'], params['disable_cache'], params['block_size'])
        if key not in stdio_throughput:
            stdio_throughput[key] = bench['bytes_per_second']
            if stdio_actual_file_size is None:
                stdio_actual_file_size = bench['file_size']

    # Load compio
    compio_files = glob.glob(compio_pattern)
    compio_benchmarks = []
    all_algo_levels = defaultdict(set)

    for filepath in compio_files:
        basename = os.path.basename(filepath)
        m = re.match(r"compio_(\w+)_(-?\d+)\.json", basename)
        if not m:
            continue
        algo = m.group(1)
        level = int(m.group(2))
        with open(filepath, 'r') as f:
            data = json.load(f)
        for bench in data.get("benchmarks", []):
            name = bench.get("name")
            params = parse_benchmark_name(name)
            if params is None:
                continue
            key = (params['is_write'], params['n_operations'], params['file_size_param'],
                   params['n_switch'], params['gamma_shape'], params['gamma_scale'],
                   params['region_size'], params['disable_cache'], params['block_size'])
            if key not in stdio_throughput:
                continue
            compio_benchmarks.append({
                'key': key,
                'algo': algo,
                'level': level,
                'bytes_per_sec': bench['bytes_per_second'],
                'block_cache_hit': bench.get('block_cache_hit'),
                'actual_file_size': bench['file_size'],
                'is_write': params['is_write'],
                'n_switch': params['n_switch'],
            })
            all_algo_levels[algo].add(level)

    if not compio_benchmarks:
        print("No compio data found.")
        return

    algo_levels_map = {algo: sorted(levels) for algo, levels in all_algo_levels.items()}
    algo_list = sorted(algo_levels_map.keys())

    highlight_levels = {
        ('zlib', 1),
        ('zstd', 1),
        ('brotli', 3),
        ('lz4', 1),
    }

    target_n_switch = 128
    cache_hits_target = []
    for b in compio_benchmarks:
        if b['n_switch'] == target_n_switch and b['block_cache_hit'] is not None:
            cache_hits_target.append(b['block_cache_hit'])
    mean_cache_hit_pct = np.mean(cache_hits_target) * 100 if cache_hits_target else 50.0

    # Prepare data per orientation
    def prepare_data(is_write_val):
        left_groups = defaultdict(list)   # (algo,level) -> [(hit_pct, gbps)]
        right_groups = defaultdict(list)  # algo -> [(file_mb, gbps, level)]
        for b in compio_benchmarks:
            if b['is_write'] != is_write_val:
                continue
            gbps = b['bytes_per_sec'] / 1e9
            if b['block_cache_hit'] is not None:
                hit_pct = b['block_cache_hit'] * 100
                left_groups[(b['algo'], b['level'])].append((hit_pct, gbps))
            if b['n_switch'] == target_n_switch:
                right_groups[b['algo']].append((b['actual_file_size'] / 1e6, gbps, b['level']))
        for k in left_groups:
            left_groups[k].sort(key=lambda x: x[0])
        for algo in right_groups:
            right_groups[algo].sort(key=lambda x: x[0])
        return left_groups, right_groups

    # Prepare stdio data per orientation for left plot (actual GB/s line)
    stdio_gbps_by_key = {}
    for key, bps in stdio_throughput.items():
        stdio_gbps_by_key[key] = bps / 1e9

    stdio_file_mb = stdio_actual_file_size / 1e6
    stdio_colour = 'grey'

    for is_write_val, label in [(0, "read"), (1, "write")]:
        left_groups, right_groups = prepare_data(is_write_val)
        if not left_groups and not right_groups:
            print(f"No data for {label}, skipping.")
            continue

        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

        # ---------- LEFT PLOT: actual GB/s, stdio drawn as a line ----------
        # Collect all hit percentages to compute transformed coordinates
        all_hits_pct = []
        for points in left_groups.values():
            for hit_pct, _ in points:
                all_hits_pct.append(hit_pct)
        # Set up the axis (this automatically sets xlim, ticks, labels)
        transformed_hits = setup_cache_hit_axis(ax1, all_hits_pct)

        # Plot each compio line (actual GB/s)
        for (algo, level), points in left_groups.items():
            if not points:
                continue
            hit_pcts, gbps_vals = zip(*points)
            # Transform hit percentages to axis coordinates
            xs = []
            for h in hit_pcts:
                if h >= 100:
                    xs.append(2.0)
                else:
                    xs.append(-np.log10(100 - h))
            colour = get_point_colour(algo, level, algo_levels_map, algo_list, highlight_levels)
            marker = get_marker(algo)
            if (algo, level) in highlight_levels:
                lw, alpha = 2, 0.75
            else:
                lw, alpha = 1, 0.15
            ax1.plot(xs, gbps_vals, marker=marker, linestyle='-', linewidth=lw,
                     markersize=5, color=colour, alpha=alpha)

        # Draw stdio as a line of actual GB/s values (not constant)
        stdio_hit_pairs = []
        for b in compio_benchmarks:
            if b['is_write'] != is_write_val:
                continue
            if b['block_cache_hit'] is not None:
                key = b['key']
                if key in stdio_gbps_by_key:
                    hit_pct = b['block_cache_hit'] * 100
                    stdio_hit_pairs.append((hit_pct, stdio_gbps_by_key[key]))
        if stdio_hit_pairs:
            stdio_hit_pairs.sort(key=lambda x: x[0])
            stdio_hit_pcts, stdio_gbps_vals = zip(*stdio_hit_pairs)
            stdio_xs = []
            for h in stdio_hit_pcts:
                if h >= 100:
                    stdio_xs.append(2.0)
                else:
                    stdio_xs.append(-np.log10(100 - h))
            ax1.plot(stdio_xs, stdio_gbps_vals, color=stdio_colour, linestyle='--',
                     linewidth=1.5, alpha=0.8, label='stdio')

        # Add vertical line at mean cache hit for n_switch=128
        mean_x = -np.log10(100 - mean_cache_hit_pct) if mean_cache_hit_pct < 100 else 2.0
        ax1.axvline(x=mean_x, color='black', linestyle=':', linewidth=1, zorder=0)

        ax1.set_ylabel("Throughput (GB/s)")
        ax1.grid(True, which="both", linestyle=':', alpha=0.6)
        ax1.set_title(f"{'Write' if is_write_val else 'Read'} · Cache hit vs throughput")

        # Legend inside left plot
        legend_handles = []
        for algo in algo_list:
            base_col = get_base_color(algo, algo_list)
            marker = get_marker(algo)
            legend_handles.append(Line2D([0], [0], color=base_col, marker=marker,
                                         linestyle='None', markersize=8, label=algo))
        legend_handles.append(Line2D([0], [0], color=stdio_colour, linestyle='--', linewidth=1.5,
                                     label='stdio'))
        ax1.legend(handles=legend_handles, loc='upper left', fontsize='small')

        # ---------- RIGHT PLOT: log-log, file size in MB, throughput in GB/s ----------
        for algo, points in right_groups.items():
            if len(points) < 2:
                continue
            for i in range(len(points) - 1):
                x1, y1, lvl1 = points[i]
                x2, y2, lvl2 = points[i+1]
                col1 = get_point_colour(algo, lvl1, algo_levels_map, algo_list, highlight_levels)
                col2 = get_point_colour(algo, lvl2, algo_levels_map, algo_list, highlight_levels)
                # Log-space interpolation for segments
                xs = np.logspace(np.log10(x1), np.log10(x2), 51)
                ys = np.logspace(np.log10(y1), np.log10(y2), 51)
                segments = np.array([[[xs[i], ys[i]], [xs[i+1], ys[i+1]]] for i in range(50)])
                c1 = np.array(mcolors.to_rgb(col1))
                c2 = np.array(mcolors.to_rgb(col2))
                colors = [c1 + t_i * (c2 - c1) for t_i in np.linspace(0, 1, 50)]
                lc = LineCollection(segments, colors=colors, linewidth=3, alpha=0.75)
                ax2.add_collection(lc)

        for algo, points in right_groups.items():
            marker = get_marker(algo)
            for fsize_mb, gbps, level in points:
                colour = get_point_colour(algo, level, algo_levels_map, algo_list, highlight_levels)
                ax2.scatter(fsize_mb, gbps, color=colour, marker=marker, s=60,
                            alpha=0.9, edgecolors='none', zorder=3)

        # Stdio point and horizontal line at actual GB/s (match right-plot benchmark params)
        stdio_gbps_right = None
        for b in compio_benchmarks:
            if b['is_write'] == is_write_val and b['n_switch'] == target_n_switch:
                key = b['key']
                if key in stdio_gbps_by_key:
                    stdio_gbps_right = stdio_gbps_by_key[key]
                    break
        if stdio_gbps_right is not None:
            ax2.scatter(stdio_file_mb, stdio_gbps_right, color=stdio_colour, marker='s', s=80, zorder=0)
            ax2.axhline(y=stdio_gbps_right, color=stdio_colour, linestyle='--', linewidth=1.5, zorder=0)

        ax2.set_xscale('log')
        ax2.set_yscale('log')
        ax2.set_xlabel("File size (MB)")
        ax2.set_ylabel("Throughput (GB/s)")
        ax2.grid(True, which="both", linestyle=':', alpha=0.6)
        ax2.set_title(f"{'Write' if is_write_val else 'Read'} · File size vs throughput (cache hit ~ {mean_cache_hit_pct:.1f}%)")

        fig.tight_layout()
        outfile = f"{label}_plots.png"
        fig.savefig(outfile, dpi=500, bbox_inches="tight")
        print(f"Saved {outfile} (500 dpi)")
        plt.close(fig)


if __name__ == "__main__":
    main()