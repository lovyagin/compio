import argparse
import glob
import os
from typing import Tuple

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy import stats


def parse_filename(filepath: str) -> Tuple[str, str]:
    basename = os.path.splitext(os.path.basename(filepath))[0]
    parts = basename.rsplit("_", 1)
    if len(parts) == 2:
        return parts[0], parts[1]
    raise ValueError(f"Unexpected filename format: {basename}")


def load_all_csvs(directory: str, max_seeds: int = 30) -> pd.DataFrame:
    all_files = glob.glob(os.path.join(directory, "*.csv"))
    if not all_files:
        raise FileNotFoundError(f"No CSV files found in {directory}")
    frames = []
    for fp in all_files:
        lib, param = parse_filename(fp)
        df = pd.read_csv(fp)
        needed = ["seed", "throughput_bytes_per_sec", "file_size"]
        if not all(c in df.columns for c in needed):
            continue
        df = df[needed].copy()
        df["seed"] = pd.to_numeric(df["seed"], errors="coerce")
        df = df.dropna(subset=["seed"]).astype({"seed": int})
        df = df[df["seed"] < max_seeds]
        df["library"] = lib
        df["param"] = param
        frames.append(df)
    full = pd.concat(frames, ignore_index=True)
    for col in ["throughput_bytes_per_sec", "file_size"]:
        full[col] = pd.to_numeric(full[col], errors="coerce")
    full = full.dropna(subset=["throughput_bytes_per_sec", "file_size"])
    return full


def compute_loss(data: pd.DataFrame) -> pd.Series:
    return data["file_size"] / data["throughput_bytes_per_sec"]


def select_optimal_per_seed(data: pd.DataFrame) -> pd.DataFrame:
    data = data.copy()
    data["loss"] = compute_loss(data)
    idx = data.groupby(["seed", "library"])["loss"].idxmin()
    optimal = data.loc[idx, ["seed", "library", "param", "loss"]]
    optimal = optimal.rename(columns={"param": "optimal_param", "loss": "optimal_loss"})
    return optimal.reset_index(drop=True)


def paired_comparison(
    optimal: pd.DataFrame, own_lib: str, competitor: str
) -> pd.DataFrame:
    own = optimal[optimal["library"] == own_lib].set_index("seed")
    comp = optimal[optimal["library"] == competitor].set_index("seed")
    common = own.index.intersection(comp.index)
    if len(common) == 0:
        raise ValueError(f"No common seeds for {own_lib} and {competitor}")
    delta = comp.loc[common, "optimal_loss"] - own.loc[common, "optimal_loss"]
    return pd.DataFrame({"seed": common, "ΔL": delta.values})


def plot_delta(delta_df: pd.DataFrame, own_lib: str, competitor: str, output: str):
    vals = delta_df["ΔL"]
    n = len(vals)
    mean = vals.mean()
    sem = vals.sem()
    ci = stats.t.interval(0.95, df=n - 1, loc=mean, scale=sem)

    fig, ax = plt.subplots(figsize=(6, 4))
    counts, bins, patches = ax.hist(vals, bins="auto", edgecolor="black", alpha=0.7)
    y_max = counts.max() * 1.05

    ax.axvline(0, color="black", linestyle=":", linewidth=2, label="0")
    ax.axvline(mean, color="red", linestyle="--", label="mean ± 95%")

    ax.errorbar(
        mean,
        y_max,
        xerr=[[mean - ci[0]], [ci[1] - mean]],
        fmt="o",
        color="red",
        capsize=5,
    )

    from matplotlib.ticker import ScalarFormatter

    ax.xaxis.set_major_formatter(ScalarFormatter(useMathText=True))
    ax.ticklabel_format(axis="x", style="sci", scilimits=(0, 0))

    ax.set_xlabel("ΔL = L(competitor) - L(ours)")
    ax.set_ylabel("Frequency")
    ax.set_title(f"Paired loss difference: {competitor} vs. {own_lib}")
    ax.legend(loc="upper left")
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output, format="svg")
    plt.close()


def main():
    parser = argparse.ArgumentParser(
        description="Simple compression library comparison."
    )
    parser.add_argument("directory", help="Directory containing result CSV files")
    parser.add_argument(
        "own_library",
        help="Name of your library",
    )
    parser.add_argument(
        "competitor",
        help="Competitor library",
    )
    parser.add_argument(
        "--max-seeds",
        type=int,
        default=30,
        help="Max number of seeds to use (default: 30)",
    )
    parser.add_argument(
        "-o",
        "--output",
        required=True,
        help="Output figure file",
    )
    args = parser.parse_args()

    df = load_all_csvs(args.directory, args.max_seeds)
    if args.own_library not in df["library"].unique():
        raise ValueError(f"Own library '{args.own_library}' not found.")
    if args.competitor not in df["library"].unique():
        raise ValueError(f"Competitor '{args.competitor}' not found in data.")

    optimal = select_optimal_per_seed(df)
    delta_df = paired_comparison(optimal, args.own_library, args.competitor)
    plot_delta(delta_df, args.own_library, args.competitor, args.output)


if __name__ == "__main__":
    main()
