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
        float(row["file_size"]) / 1e6,
        float(row["throughput_mean"]) / 1e6,
        float(row["throughput_stddev"]) / 1e6,
    )


def block_size(filepath):
    return int(
        re.search(
            r"_[a-z]{2}(\d+)$", os.path.splitext(os.path.basename(filepath))[0]
        ).group(1)
    )


def label(filepath):
    return re.sub(
        r"_([a-z]{2})\d+$", "", os.path.splitext(os.path.basename(filepath))[0]
    ).replace("_", " ")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="*")
    parser.add_argument("-o", "--output", default="results.svg")
    args = parser.parse_args()

    files = sorted(args.files) if args.files else sorted(glob.glob("results/*.csv"))
    if not files:
        print("No files found.")
        return

    groups = {}
    for fp in files:
        groups.setdefault(label(fp), []).append((fp, block_size(fp)))
    for g in groups.values():
        g.sort(key=lambda x: x[1])

    all_x = []
    all_y = []
    for flist in groups.values():
        for fp, _ in flist:
            x, y, _ = load_csv(fp)
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
        xs, ys, yerrs = zip(*[load_csv(fp) for fp, _ in flist])
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
        ax.errorbar(xs, ys, yerr=yerrs, fmt="none", color=c, alpha=0.5, capsize=3)

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
