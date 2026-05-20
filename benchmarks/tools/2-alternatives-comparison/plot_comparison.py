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
    parser.add_argument("-k", "--keys", nargs="*", type=int,
                        help="Filter by file_offset values (e.g. -k 503316480 469762048 33554432)")
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
    line_styles = ["-", "--", ":", "-."]

    # Collect all points for axis limits
    all_x = []
    all_y = []
    for lib in libraries:
        for bs in sorted(all_data[lib].keys()):
            df = all_data[lib][bs]
            for ds in sorted(df["file_offset"].unique()):
                # Skip if -k filter is active and this offset is not selected
                if args.keys is not None and ds not in args.keys:
                    continue
                sub = df[df["file_offset"] == ds]
                file_size_mb = sub["file_size"].iloc[0] / 1e6
                vals = sub["throughput"] / 1e6
                all_x.append(file_size_mb)
                all_y.append(vals.mean())

    if not all_x:
        print("No data to plot after filtering.")
        return

    x_min, x_max = min(all_x), max(all_x)
    y_min, y_max = min(all_y), max(all_y)

    x_left = x_min * 0.9
    x_right = x_max * 1.1
    y_bottom = y_min * 0.9
    y_top = y_max * 1.1

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
            # Skip if -k filter is active and this offset is not selected
            if args.keys is not None and ds not in args.keys:
                continue
            xs = []
            ys = []
            yerrs = []
            for bs in block_sizes:
                df = all_data[lib][bs]
                sub = df[df["file_offset"] == ds]
                if sub.empty:
                    continue
                file_size_mb = sub["file_size"].iloc[0] / 1e6
                vals = sub["throughput"] / 1e6
                xs.append(file_size_mb)
                ys.append(vals.mean())
                yerrs.append(vals.std())

            # Only label the first line of each library to keep legend clean
            label_str = lib.replace("_", " ") if si == 0 else None
            ax.loglog(
                xs,
                ys,
                marker="",
                linestyle=line_styles[si % len(line_styles)],
                linewidth=1.5,
                color=color,
                alpha=1,
                label=label_str,
            )
            ax.errorbar(
                xs, ys, yerr=yerrs, fmt="none", color=color, alpha=1, capsize=5
            )

            # Mark the point with the highest throughput/file_size ratio
            ratios = [y / x for x, y in zip(xs, ys)]
            best_idx = ratios.index(max(ratios))
            ax.plot(
                xs[best_idx], ys[best_idx],
                marker="o", markersize=12,
                markerfacecolor="none",
                markeredgecolor="red", markeredgewidth=1.5, alpha=0.5,
                linestyle="None",
            )
            # Annotate with the block size
            best_bs = block_sizes[best_idx]

            # Manually place labels to avoid overlapping
            xytext = (-35, -3)
            if best_idx == 4 and ds == 33554432 and lib == "compio_zstd1":
                xytext = (-28, 8)
            if best_idx == 4 and ds == 469762048 and lib == "compio_zstd1":
                xytext = (-28, 8)
            if best_idx == 4 and ds == 33554432 and lib == "seekable_zstd":
                xytext = (-30, -8)
            if best_idx == 4 and ds == 469762048 and lib == "seekable_zstd":
                xytext = (-30, -3)

            ax.annotate(
                f"{best_bs}",
                (xs[best_idx], ys[best_idx]),
                textcoords="offset points",
                xytext=xytext,
                fontsize=8,
                alpha=0.7,
                color="red",
            )

    ax.set(
        xlabel="File Size (MB)",
        ylabel="Throughput (MB/s)",
        title="Read · Throughput vs File Size (no block cache)",
    )
    ax.grid(True, which="both", linestyle=":", alpha=0.7)

    # Add a proxy artist for the best-ratio marker to the legend
    from matplotlib.lines import Line2D
    best_marker = Line2D(
        [0], [0], marker="o", markersize=8,
        markerfacecolor="none", markeredgecolor="red",
        markeredgewidth=1.5, linestyle="None", alpha=0.7,
        label=r"best $\frac{\text{throughput}}{\text{file size}}$",
    )
    handles, labels = ax.get_legend_handles_labels()
    handles.append(best_marker)
    ax.legend(handles=handles, loc="lower right")
    plt.tight_layout()
    plt.savefig(args.output, bbox_inches="tight")
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
