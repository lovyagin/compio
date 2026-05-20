import argparse
import glob
import os
import re

import matplotlib.pyplot as plt
import pandas as pd


def parse_filename(filepath):
    basename = os.path.splitext(os.path.basename(filepath))[0]
    m = re.match(r"^(.+)_(\d+)$", basename)
    if not m:
        raise ValueError(f"Cannot parse filename: {basename}")
    return m.group(1), int(m.group(2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="*")
    parser.add_argument("-o", "--output", default="comparison.svg")
    args = parser.parse_args()

    files = sorted(args.files) if args.files else sorted(glob.glob("results/*.csv"))
    if not files:
        print("No files found.")
        return

    # Group files by library
    groups = {}
    for fp in files:
        lib, bs = parse_filename(fp)
        groups.setdefault(lib, []).append((fp, bs))
    for g in groups.values():
        g.sort(key=lambda x: x[1])

    # Load all data
    all_data = {}
    for lib, flist in groups.items():
        all_data[lib] = {}
        for fp, bs in flist:
            df = pd.read_csv(fp)
            all_data[lib][bs] = df

    libraries = sorted(all_data.keys())
    colors = ["#1f77b4", "#ff7f0e"]
    line_styles = ["-", "--", ":"]

    # Collect all points for axis limits
    all_x = []
    all_y = []
    for lib in libraries:
        for bs in sorted(all_data[lib].keys()):
            df = all_data[lib][bs]
            file_size_mb = df["file_size"].iloc[0] / 1e6
            for ds in sorted(df["file_offset"].unique()):
                sub = df[df["file_offset"] == ds]
                vals = sub["throughput"] / 1e6
                all_x.append(file_size_mb)
                all_y.append(vals.mean())

    x_min, x_max = min(all_x), max(all_x)
    y_min, y_max = min(all_y), max(all_y)

    x_left = x_min * 0.9
    x_right = x_max * 1.1
    y_bottom = y_min * 0.8
    y_top = y_max * 1.2

    plt.figure(figsize=(10, 7))
    ax = plt.gca()
    ax.set_xlim(left=x_left, right=x_right)
    ax.set_ylim(bottom=y_bottom, top=y_top)

    for li, lib in enumerate(libraries):
        color = colors[li % len(colors)]
        block_sizes = sorted(all_data[lib].keys())
        file_offsets = sorted(
            set().union(*[set(all_data[lib][bs]["file_offset"].unique()) for bs in block_sizes])
        )

        for si, ds in enumerate(file_offsets):
            xs = []
            ys = []
            yerrs = []
            for bs in block_sizes:
                df = all_data[lib][bs]
                sub = df[df["file_offset"] == ds]
                if sub.empty:
                    continue
                file_size_mb = df["file_size"].iloc[0] / 1e6
                vals = sub["throughput"] / 1e6
                xs.append(file_size_mb)
                ys.append(vals.mean())
                yerrs.append(vals.std())

            label_str = f"{lib} file_offset={ds}"
            ax.loglog(
                xs,
                ys,
                marker="o",
                linestyle=line_styles[si % len(line_styles)],
                linewidth=2,
                markersize=4,
                color=color,
                alpha=0.75,
                label=label_str,
            )
            ax.errorbar(
                xs, ys, yerr=yerrs, fmt="none", color=color, alpha=0.5, capsize=3
            )

    ax.set(
        xlabel="File Size (MB)",
        ylabel="Throughput (MB/s)",
        title="Read · Throughput vs File Size (no block cache)",
    )
    ax.grid(True, which="both", linestyle=":", alpha=0.7)
    ax.legend(loc="lower right")
    plt.tight_layout()
    plt.savefig(args.output, bbox_inches="tight")
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
