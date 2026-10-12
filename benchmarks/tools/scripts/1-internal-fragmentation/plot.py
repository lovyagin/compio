import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.ticker import FixedLocator, ScalarFormatter


def load_aggregate(directory, method="median"):
    files = sorted(Path(directory).glob("seed_*.csv"))
    if not files:
        raise FileNotFoundError(f"No seed files in {directory}")

    all_runs = [pd.read_csv(f) for f in files]

    lengths = {len(df) for df in all_runs}
    if len(lengths) != 1:
        raise ValueError(f"Inconsistent lengths in {directory}: {lengths}")

    cols = ["n_blocks", "insert_bps", "erase_bps"]
    stacks = {col: np.array([df[col].values for df in all_runs]) for col in cols}

    agg = pd.DataFrame()
    for col in cols:
        agg[col] = (
            np.median(stacks[col], axis=0)
            if method == "median"
            else np.mean(stacks[col], axis=0)
        )
    return agg


def prepare_plot_data(df, median_window, bin_size):
    """
    1. Convert bps -> mbps.
    2. Apply rolling median (window = median_window) to throughput.
    3. Bin‑average all columns (n_blocks, insert_mbps, erase_mbps) into chunks of size bin_size.
       The final index is the middle of each bin.
    """
    df = df.copy()
    df["insert_mbps"] = df["insert_bps"] / 1e6
    df["erase_mbps"] = df["erase_bps"] / 1e6

    # Step 1: median filter on throughput
    df["insert_med"] = (
        df["insert_mbps"].rolling(median_window, center=True, min_periods=1).median()
    )
    df["erase_med"] = (
        df["erase_mbps"].rolling(median_window, center=True, min_periods=1).median()
    )

    # Step 2: bin-average on ALL columns (including n_blocks)
    n = len(df)
    num_bins = n // bin_size
    if num_bins == 0:
        raise ValueError(f"bin_size {bin_size} > data length {n}")

    df = df.iloc[: num_bins * bin_size]

    # Reshape and mean for each bin
    n_blocks_binned = df["n_blocks"].values.reshape(num_bins, bin_size).mean(axis=1)
    insert_binned = df["insert_med"].values.reshape(num_bins, bin_size).mean(axis=1)
    erase_binned = df["erase_med"].values.reshape(num_bins, bin_size).mean(axis=1)

    bin_centers = np.arange(bin_size // 2, num_bins * bin_size, bin_size)
    binned = pd.DataFrame(
        {
            "n_blocks": n_blocks_binned,
            "insert_smooth": insert_binned,
            "erase_smooth": erase_binned,
        },
        index=bin_centers,
    )
    return binned


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--before-dir", required=True)
    p.add_argument("--after-dir", required=True)
    p.add_argument("--method", choices=["median", "mean"], default="median")
    p.add_argument("--median-window", type=int, default=101)
    p.add_argument("--bin-size", type=int, default=100)
    p.add_argument("--output", default="blocks.svg")
    args = p.parse_args()

    before = load_aggregate(args.before_dir, args.method)
    after = load_aggregate(args.after_dir, args.method)

    before = prepare_plot_data(before, args.median_window, args.bin_size)
    after = prepare_plot_data(after, args.median_window, args.bin_size)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    ax1.plot(
        before.index,
        before["n_blocks"],
        color="#1f77b4",
        alpha=0.5,
        linestyle="--",
        label="До",
    )
    ax1.plot(after.index, after["n_blocks"], color="#1f77b4", alpha=1.0, label="После")
    ax1.set_xlabel("Операции")
    ax1.set_ylabel("Количество блоков")
    ax1.set_title("Количество блоков на операцию (лог. шкала)")
    ax1.set_yscale("log")
    ax1.grid(True, linestyle="--", alpha=0.7)
    ax1.legend()

    ax2.plot(
        before.index,
        before["insert_smooth"],
        color="#2ca02c",
        alpha=0.5,
        label="Вставка (до)",
    )
    ax2.plot(
        before.index,
        before["erase_smooth"],
        color="#d62728",
        alpha=0.5,
        label="Удаление (до)",
    )
    ax2.plot(
        after.index,
        after["insert_smooth"],
        color="#2ca02c",
        alpha=1.0,
        label="Вставка (после)",
    )
    ax2.plot(
        after.index,
        after["erase_smooth"],
        color="#d62728",
        alpha=1.0,
        label="Удаление (после)",
    )
    ax2.set_xlabel("Операции")
    ax2.set_ylabel("Пропускная способность (МБ/с)")
    ax2.set_title("Пропускная способность вставки и удаления\n(лог. шкала, сглажено)")
    ax2.set_yscale("log")
    ax2.set_ylim(1e0, 1e3)
    ax2.grid(True, linestyle="--", alpha=0.7)
    ax2.legend()

    extra_ticks = [after["insert_smooth"].iloc[-1], before["insert_smooth"].iloc[-1]]
    existing_ticks = list(ax2.get_yticks())
    all_ticks = sorted(set(existing_ticks + extra_ticks))
    ax2.yaxis.set_major_locator(FixedLocator(all_ticks))
    ax2.yaxis.set_major_formatter(ScalarFormatter())

    plt.tight_layout()
    plt.savefig(args.output, bbox_inches="tight")
    print(f"Plot saved → {args.output}")


if __name__ == "__main__":
    main()
