import argparse
import glob
import json
import os

import matplotlib.pyplot as plt


def load_benchmark_data(filepath: str):
    try:
        with open(filepath, "r") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"JSON decode error: {e}") from e

    points = []
    for bench in data.get("benchmarks", []):
        file_size_bytes = bench.get("file_size")
        bytes_per_sec = bench.get("bytes_per_second")
        if file_size_bytes is None or bytes_per_sec is None:
            continue
        file_size_mb = file_size_bytes / 1e6
        throughput_mb_s = bytes_per_sec / 1e6
        points.append((file_size_mb, throughput_mb_s))

    if not points:
        raise RuntimeError(
            f"No valid benchmark entries (with file_size and "
            f"bytes_per_second) found in {filepath}"
        )

    return points


def get_label(filepath: str) -> str:
    stem = os.path.splitext(os.path.basename(filepath))[0]
    return stem.replace("_", " ")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "files",
        nargs="*",
        help="JSON result files to plot (if none given, uses results/*.json)",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="results.svg",
        help="Output file path (default: results.svg)",
    )
    args = parser.parse_args()

    if args.files:
        files = sorted(args.files)
    else:
        files = sorted(glob.glob("results/*.json"))
        if not files:
            print("No files matching 'results/*.json' found.")
            return

    cmap = plt.cm.get_cmap("tab10", len(files))

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
        ax.loglog(
            xs,
            ys,
            marker="o",
            linestyle="-",
            linewidth=2,
            markersize=4,
            color=color,
            alpha=0.75,
            label=label,
        )

    if not ax.lines:
        print("No valid data could be plotted.")
        return

    ax.set_xlabel("File Size (MB)")
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title("Read · Throughput vs File Size (no block cache)")
    ax.grid(True, which="both", linestyle=":", alpha=0.7)

    ax.legend(loc="lower right")

    plt.tight_layout()
    plt.savefig(args.output, bbox_inches="tight")
    print(f"Plot saved as {args.output}")


if __name__ == "__main__":
    main()
