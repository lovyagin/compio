import argparse
import csv
import glob
import os
import re

import matplotlib.pyplot as plt


def load_csv(filepath):
    with open(filepath) as f:
        row = next(csv.DictReader(f))
    return (
        float(row["mean_file_size"]) / 1e6,
        float(row["mean_throughput"]) / 1e6,
        float(row["stddev_throughput"]) / 1e6,
        float(row["stddev_file_size"]) / 1e6,
    )


def param_value(filepath):
    """Extract the numeric parameter value from the filename (block size, frame size, or flush interval)."""
    basename = os.path.splitext(os.path.basename(filepath))[0]
    return int(re.search(r"_([a-z]{2})(\d+)$", basename).group(2))


def label(filepath):
    """Extract a human-readable label from the filename, stripping the numeric parameter suffix."""
    basename = os.path.splitext(os.path.basename(filepath))[0]
    return re.sub(r"_([a-z]{2})\d+$", "", basename).replace("_", " ")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="*")
    parser.add_argument("-o", "--output", default="results.svg")
    args = parser.parse_args()

    files = sorted(args.files) if args.files else sorted(glob.glob("results/*.txt"))
    if not files:
        print("No files found.")
        return

    groups = {}
    for fp in files:
        groups.setdefault(label(fp), []).append((fp, param_value(fp)))
    for g in groups.values():
        g.sort(key=lambda x: x[1])

    all_x = []
    all_y = []
    for flist in groups.values():
        for fp, _ in flist:
            x, y, _, _ = load_csv(fp)
            all_x.append(x)
            all_y.append(y)

    x_min, x_max = min(all_x), max(all_x)
    y_min, y_max = min(all_y), max(all_y)

    x_left = x_min * 0.8
    x_right = x_max * 1.2
    y_bottom = y_min * 0.8
    y_top = y_max * 1.2

    plt.figure(figsize=(10, 7))
    ax = plt.gca()
    cmap = plt.cm.get_cmap("tab10", len(groups))

    ax.set_xlim(left=x_left, right=x_right)
    ax.set_ylim(bottom=y_bottom, top=y_top)

    for idx, (lbl, flist) in enumerate(sorted(groups.items())):
        xs, ys, yerrs, xerrs = zip(*[load_csv(fp) for fp, _ in flist])
        c = cmap(idx)
        ax.loglog(
            xs,
            ys,
            marker="o",
            linestyle="-",
            linewidth=2,
            markersize=4,
            color=c,
            alpha=0.75,
            label=lbl,
        )
        ax.errorbar(xs, ys, xerr=xerrs, yerr=yerrs, fmt="none", color=c, alpha=0.5, capsize=3)

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
