#!/usr/bin/env python3
"""
Stacked bar chart of compression overheads per block size.
Bottom: blocked compression overhead, Top: metadata overhead.
Y-axis: percentage relative to pure Zstd-5 compressed size.
"""

import os
import sys
import json
import matplotlib.pyplot as plt
import numpy as np

# Constants (in bytes)
ORIGINAL_SIZE = 33554432          # 32 MiB
PURE_ZSTD_SIZE = 9051311          # whole‑file Zstd‑5 compressed size

def load_data(directory='.'):
    dummy_path = os.path.join(directory, 'compio_dummy_0.json')
    zstd_path = os.path.join(directory, 'compio_zstd_5.json')
    for path in [dummy_path, zstd_path]:
        if not os.path.isfile(path):
            sys.exit(f"Error: Required file not found: {path}")

    with open(dummy_path) as f:
        dummy_raw = json.load(f)
    with open(zstd_path) as f:
        zstd_raw = json.load(f)

    def extract_metrics(data):
        res = {}
        for bench in data.get('benchmarks', []):
            name = bench.get('name', '')
            if not name.endswith('/real_time'):
                continue
            parts = name.split('/')
            try:
                block_size = int(parts[-2])
            except (ValueError, IndexError):
                continue
            file_size = bench.get('file_size')
            if file_size is None:
                continue
            res[block_size] = file_size
        return res

    dummy_sizes = extract_metrics(dummy_raw)
    zstd_sizes = extract_metrics(zstd_raw)

    common = sorted(set(dummy_sizes.keys()) & set(zstd_sizes.keys()))
    if not common:
        sys.exit("No matching block sizes between the two files.")

    data = []
    for bs in common:
        dummy = dummy_sizes[bs]
        zstd = zstd_sizes[bs]
        # Metadata overhead = dummy - original (should be >=0)
        metadata = dummy - ORIGINAL_SIZE
        # Blocked compression overhead = zstd - pure - metadata
        blocked = zstd - PURE_ZSTD_SIZE - metadata
        data.append((bs, metadata, blocked))
    return data

def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else '.'
    print(f"Loading data from '{results_dir}' ...")
    data = load_data(results_dir)

    blocks = [d[0] for d in data]
    metadata_bytes = [d[1] for d in data]
    blocked_bytes = [d[2] for d in data]

    # Convert to percentages of pure Zstd size
    metadata_pct = [b / PURE_ZSTD_SIZE * 100 for b in metadata_bytes]
    blocked_pct   = [b / PURE_ZSTD_SIZE * 100 for b in blocked_bytes]
    total_pct = [m + b for m, b in zip(metadata_pct, blocked_pct)]

    # Print verification table
    print("\n" + "=" * 80)
    print("Overheads as percentage of pure Zstd‑5 size (pure = 0%)")
    print("=" * 80)
    print(f"Pure Zstd‑5 size: {PURE_ZSTD_SIZE} bytes\n")
    for i, bs in enumerate(blocks):
        print(f"Block size: {bs} bytes")
        print(f"  Blocked compression overhead: {blocked_pct[i]:.1f}%")
        print(f"  Metadata overhead: {metadata_pct[i]:.1f}%")
        print(f"  Total overhead: {total_pct[i]:.1f}%")
        if metadata_bytes[i] < 0:
            print(f"    WARNING: Metadata overhead negative – dummy size smaller than original?")
        if blocked_bytes[i] < 0:
            print(f"    WARNING: Blocked overhead negative – check constants.")
        print()

    # Create stacked bar chart
    fig, ax = plt.subplots(figsize=(12, 6))

    # X positions: block sizes in order
    x_pos = np.arange(len(blocks))
    width = 0.6

    # Bottom: blocked compression overhead
    ax.bar(x_pos, blocked_pct, width, label='Blocked compression overhead', color='salmon', edgecolor='black')
    # Top: metadata overhead (stacked on top of blocked)
    ax.bar(x_pos, metadata_pct, width, bottom=blocked_pct, label='Metadata overhead', color='lightblue', edgecolor='black')

    # No numbers inside rectangles

    # Labels and ticks
    ax.set_xlabel('Block size (bytes)', fontsize=12)
    ax.set_ylabel('File size overhead (relative to pure compression) (%)', fontsize=12)
    ax.set_title('Compression overheads per block size', fontsize=14)
    ax.set_xticks(x_pos)
    ax.set_xticklabels(blocks)
    ax.set_ylim(bottom=0)
    ax.legend(loc='upper right')
    ax.grid(True, axis='y', linestyle='--', alpha=0.5)

    plt.tight_layout()
    output_file = 'stacked_overheads.png'
    plt.savefig(output_file, dpi=300)
    print(f"\nPlot saved as {output_file}")
    plt.show()

if __name__ == '__main__':
    main()