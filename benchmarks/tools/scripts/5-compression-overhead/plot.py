import argparse
import json

import matplotlib.pyplot as plt
import numpy as np


def mean(values):
    return sum(values) / len(values)


def load_data(path):
    """Load and aggregate data from either single-run or multi-region JSON."""
    with open(path, "r") as f:
        data = json.load(f)

    # Detect format: multi-region (from run_regions.py) or single-run (from run.py)
    if "regions" in data:
        # Multi-region format: aggregate by taking mean across regions
        region_size = data["region_size"]
        compressor = data["compressor"]
        level = data["level"]
        num_regions = data["num_regions"]

        # block_size -> list of (whole, blocked, archive)
        agg = {}
        for region_data in data["regions"].values():
            for r in region_data["results"]:
                bs = r["block_size"]
                if bs not in agg:
                    agg[bs] = {"whole": [], "blocked": [], "archive": []}
                agg[bs]["whole"].append(r["whole"])
                agg[bs]["blocked"].append(r["blocked"])
                agg[bs]["archive"].append(r["archive"])

        block_sizes = sorted(agg.keys())
        whole_sizes = [mean(agg[bs]["whole"]) for bs in block_sizes]
        blocked_sums = [mean(agg[bs]["blocked"]) for bs in block_sizes]
        archive_sizes = [mean(agg[bs]["archive"]) for bs in block_sizes]
        original_size = region_size

        print(
            f"Loaded multi-region data: {num_regions} regions x "
            f"{region_size // (1024*1024)} MB, compressor={compressor}, level={level}"
        )
    else:
        # Single-run format (original run.py)
        original_size = data["original_size"]
        compressor = data["compressor"]
        level = data["level"]
        results = data["results"]

        block_sizes = [r["block_size"] for r in results]
        whole_sizes = [r["whole"] for r in results]
        blocked_sums = [r["blocked"] for r in results]
        archive_sizes = [r["archive"] for r in results]

    return original_size, compressor, level, block_sizes, whole_sizes, blocked_sums, archive_sizes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input_json", help="JSON file from run.py or run_regions.py")
    parser.add_argument("output_svg", help="output SVG file path")
    args = parser.parse_args()

    original_size, compressor, level, block_sizes, whole_sizes, blocked_sums, archive_sizes = \
        load_data(args.input_json)

    original_mib = original_size / (1024 * 1024)
    whole_mib = [w / (1024 * 1024) for w in whole_sizes]
    blocked_mib = [b / (1024 * 1024) for b in blocked_sums]
    archive_mib = [a / (1024 * 1024) for a in archive_sizes]

    blocked_overhead_mib = [
        blocked_mib[i] - whole_mib[i] for i in range(len(block_sizes))
    ]
    archive_overhead_mib = [
        archive_mib[i] - blocked_mib[i] for i in range(len(block_sizes))
    ]

    blocked_pct = [
        (blocked_overhead_mib[i] / original_mib) * 100 for i in range(len(block_sizes))
    ]
    archive_pct = [
        (archive_overhead_mib[i] / original_mib) * 100 for i in range(len(block_sizes))
    ]

    whole_constant_mib = whole_mib[0]
    y_max = max(original_mib, max(archive_mib)) * 1.05

    x = np.arange(len(block_sizes))
    width = 0.6
    threshold_mib = 0.07 * original_mib

    fig, ax = plt.subplots(figsize=(14, 7))

    ax.bar(
        x,
        whole_mib,
        width,
        label=f"Размер непрерывно сжатого файла ({whole_constant_mib:.1f} МиБ)",
        color="lightgreen",
        edgecolor="none",
    )
    ax.bar(
        x,
        blocked_overhead_mib,
        width,
        bottom=whole_mib,
        label="Издержки блочного сжатия",
        color="salmon",
        edgecolor="none",
    )
    bottom_mid = [
        whole_mib[i] + blocked_overhead_mib[i] for i in range(len(block_sizes))
    ]
    ax.bar(
        x,
        archive_overhead_mib,
        width,
        bottom=bottom_mid,
        label="Издержки метаданных библиотеки",
        color="lightblue",
        edgecolor="none",
    )

    ax.axhline(
        y=original_mib,
        color="red",
        linestyle="--",
        linewidth=1.5,
        label=f"Исходный размер ({original_mib:.1f} МиБ)",
    )

    max_tick = int(np.ceil(y_max / 10)) * 10
    y_ticks = np.arange(0, max_tick + 0.1, 10)
    specials = [original_mib, whole_constant_mib]
    for val in specials:
        y_ticks = [t for t in y_ticks if abs(t - val) >= 0.5]
    y_ticks += specials
    y_ticks.sort()
    ax.set_yticks(y_ticks)
    ax.set_yticklabels([f"{tick:.1f}" for tick in y_ticks])

    fontsize_val = 11
    for i in range(len(block_sizes)):
        blocked_h = blocked_overhead_mib[i]
        if blocked_h > threshold_mib:
            y_pos = whole_mib[i] + blocked_h / 2
            ax.text(
                x[i],
                y_pos,
                f"{blocked_pct[i]:.1f}%",
                ha="center",
                va="center",
                fontsize=fontsize_val,
                color="black",
            )
        else:
            y_pos = whole_mib[i] - 0.5
            ax.text(
                x[i],
                y_pos,
                f"{blocked_pct[i]:.1f}%",
                ha="center",
                va="top",
                fontsize=fontsize_val,
                color="black",
            )

        meta_h = archive_overhead_mib[i]
        if meta_h > threshold_mib:
            y_pos = bottom_mid[i] + meta_h / 2
            ax.text(
                x[i],
                y_pos,
                f"{archive_pct[i]:.1f}%",
                ha="center",
                va="center",
                fontsize=fontsize_val,
                color="black",
            )
        else:
            y_pos = bottom_mid[i] + meta_h + 0.2
            ax.text(
                x[i],
                y_pos,
                f"{archive_pct[i]:.1f}%",
                ha="center",
                va="bottom",
                fontsize=fontsize_val,
                color="black",
            )

    ax.set_xlabel("Размер блока (Б)", fontsize=12)
    ax.set_ylabel("Размер файла-контейнера (МиБ)", fontsize=12)
    ax.set_title(f"Накладные расходы на размер файла-контейнера в зависимости от размера блока ({compressor} уровень {level})", fontsize=14)
    ax.set_xticks(x)
    ax.set_xticklabels(block_sizes)
    ax.set_ylim(0, y_max)
    ax.legend(loc="upper right")
    ax.grid(True, axis="y", linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(args.output_svg, format="svg")
    print(f"Plot saved as {args.output_svg}")


if __name__ == "__main__":
    main()
