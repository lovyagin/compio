#!/usr/bin/env python3
"""
Analysis of compression library trade-offs.

Reads CSV files from a directory, computes loss L = file_size / throughput,
finds the optimal parameter for each library and seed, and compares
a chosen "own" library against competitors via paired ΔL = L_competitor - L_own.
Plots results and optionally performs a robustness sweep over the size/speed
weight.
"""

import argparse
import glob
import os
import re
from typing import Dict, List, Tuple

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from scipy import stats


# ----------------------------- Helper functions -----------------------------

def parse_filename(filepath: str) -> Tuple[str, str]:
    """
    Extract library name and parameter from a filename like
    'compio_zstd1_16384.csv' -> ('compio_zstd1', '16384')
    """
    basename = os.path.splitext(os.path.basename(filepath))[0]
    # Pattern: everything up to the last underscore is the library name,
    # the remainder is the parameter.
    # But library names themselves may contain underscores (e.g., compio_zlib1).
    # We split at the last underscore.
    parts = basename.rsplit('_', 1)
    if len(parts) == 2:
        lib, param = parts
        return lib, param
    else:
        raise ValueError(f"Unexpected filename format: {basename}")


def load_all_csvs(directory: str, max_seeds: int = 30) -> pd.DataFrame:
    all_files = glob.glob(os.path.join(directory, "*.csv"))
    if not all_files:
        raise FileNotFoundError(f"No CSV files found in {directory}")

    frames = []
    for fp in all_files:
        lib, param = parse_filename(fp)
        df = pd.read_csv(fp)
        needed = ['seed', 'throughput_bytes_per_sec', 'file_size']
        if not all(c in df.columns for c in needed):
            print(f"Warning: {fp} lacks required columns, skipping.")
            continue
        df = df[needed].copy()
        # Ensure seed is numeric immediately
        df['seed'] = pd.to_numeric(df['seed'], errors='coerce')
        # Drop rows where seed couldn't be parsed (if any)
        df = df.dropna(subset=['seed'])
        df['seed'] = df['seed'].astype(int)
        df['library'] = lib
        df['param'] = param
        # Now filter seeds < max_seeds safely
        df = df[df['seed'] < max_seeds]
        frames.append(df)

    full = pd.concat(frames, ignore_index=True)
    # Convert other columns to float
    full['throughput_bytes_per_sec'] = pd.to_numeric(full['throughput_bytes_per_sec'], errors='coerce')
    full['file_size'] = pd.to_numeric(full['file_size'], errors='coerce')
    full = full.dropna(subset=['throughput_bytes_per_sec', 'file_size'])
    return full


def compute_loss(data: pd.DataFrame, alpha: float = 1.0) -> pd.Series:
    """
    Loss function: L = file_size / (throughput ** alpha)
    alpha = 1 gives the original time-to-transfer loss.
    """
    return data['file_size'] / (data['throughput_bytes_per_sec'] ** alpha)


def select_optimal_per_seed(data: pd.DataFrame, alpha: float = 1.0
                            ) -> pd.DataFrame:
    """
    For each (seed, library) select the parameter that minimises loss.
    Returns DataFrame with columns: seed, library, optimal_param, optimal_loss
    """
    data = data.copy()
    data['loss'] = compute_loss(data, alpha)
    idx = data.groupby(['seed', 'library'])['loss'].idxmin()
    optimal = data.loc[idx, ['seed', 'library', 'param', 'loss']]
    optimal = optimal.rename(columns={'param': 'optimal_param', 'loss': 'optimal_loss'})
    return optimal.reset_index(drop=True)


def paired_comparison(optimal: pd.DataFrame, own_lib: str,
                      competitors: List[str]) -> Dict[str, pd.DataFrame]:
    """
    For each competitor compute ΔL = L_competitor - L_own per seed.
    Returns a dict mapping competitor name to a DataFrame with columns:
    seed, ΔL
    """
    own_data = optimal[optimal['library'] == own_lib].set_index('seed')
    comparisons = {}
    for comp in competitors:
        comp_data = optimal[optimal['library'] == comp].set_index('seed')
        # Inner join on seed to keep only seeds present in both
        common = own_data.index.intersection(comp_data.index)
        if len(common) == 0:
            print(f"Warning: no common seeds for {own_lib} vs {comp}, skipping.")
            continue
        delta = comp_data.loc[common, 'optimal_loss'] - own_data.loc[common, 'optimal_loss']
        comparisons[comp] = pd.DataFrame({'seed': common, 'ΔL': delta.values})
    return comparisons


def summary_stats(delta_df: pd.DataFrame) -> Dict[str, float]:
    """Mean, standard error, 95% CI for ΔL"""
    vals = delta_df['ΔL']
    n = len(vals)
    mean = vals.mean()
    sem = vals.sem()  # standard error of the mean
    ci = stats.t.interval(0.95, df=n-1, loc=mean, scale=sem)
    return {
        'mean': mean,
        'std_err': sem,
        'ci_lower': ci[0],
        'ci_upper': ci[1],
        'n': n
    }


# ----------------------------- Plotting functions ---------------------------

def plot_loss_curves(data: pd.DataFrame, alpha: float = 1.0,
                     output: str = "loss_curves.png"):
    """
    For each library, plot mean loss ± SEM as a function of parameter.
    """
    data = data.copy()
    data['loss'] = compute_loss(data, alpha)
    summary = data.groupby(['library', 'param'])['loss'].agg(['mean', 'sem']).reset_index()
    # Attempt to sort parameters numerically
    try:
        summary['param_num'] = summary['param'].astype(int)
        summary = summary.sort_values('param_num')
    except (ValueError, TypeError):
        pass

    libs = summary['library'].unique()
    n_libs = len(libs)
    fig, axes = plt.subplots(n_libs, 1, figsize=(10, 3 * n_libs), sharex=False)
    if n_libs == 1:
        axes = [axes]

    for ax, lib in zip(axes, libs):
        lib_data = summary[summary['library'] == lib].copy()
        x = np.arange(len(lib_data))
        ax.errorbar(x, lib_data['mean'], yerr=lib_data['sem'],
                    fmt='-o', capsize=5, label=lib)
        ax.set_title(f"Library: {lib}")
        ax.set_ylabel("Mean loss ± SEM")
        ax.set_xticks(x)
        ax.set_xticklabels(lib_data['param'], rotation=45)
        # Find minimum using positional index
        min_pos = lib_data['mean'].values.argmin()
        ax.plot(x[min_pos], lib_data['mean'].iloc[min_pos], 'r*', markersize=15,
                label=f"Min: param={lib_data['param'].iloc[min_pos]}")
        ax.legend()
        ax.grid(True, alpha=0.3)

    fig.suptitle(f"Loss (L = file_size / throughput^{alpha}) vs. parameter")
    plt.tight_layout()
    plt.savefig(output, dpi=150)
    plt.close()
    print(f"Saved loss curves to {output}")


def plot_delta_distribution(comparisons: Dict[str, pd.DataFrame],
                            output: str = "delta_distribution.png"):
    """Histogram and/or boxplot of ΔL for each competitor."""
    n = len(comparisons)
    if n == 0:
        return
    fig, axes = plt.subplots(1, n, figsize=(4 * n, 4))
    if n == 1:
        axes = [axes]

    for ax, (comp, df) in zip(axes, comparisons.items()):
        ax.hist(df['ΔL'], bins='auto', edgecolor='black', alpha=0.7)
        mean_val = df['ΔL'].mean()
        ax.axvline(mean_val, color='red', linestyle='--', label=f"Mean = {mean_val:.2e}")
        ax.set_title(f"ΔL = L({comp}) - L(ours)")
        ax.set_xlabel("ΔL")
        ax.legend()
        ax.grid(True, alpha=0.3)

    fig.suptitle("Distribution of paired loss differences")
    plt.tight_layout()
    plt.savefig(output, dpi=150)
    plt.close()
    print(f"Saved ΔL distributions to {output}")


def plot_robustness(robustness_results: pd.DataFrame,
                    output: str = "robustness.png"):
    """
    Plot mean ΔL and 95% CI as a function of α (weight exponent).
    `robustness_results` has columns: competitor, alpha, mean, ci_lower, ci_upper
    """
    if robustness_results.empty:
        return
    competitors = robustness_results['competitor'].unique()
    fig, ax = plt.subplots(figsize=(10, 5))
    for comp in competitors:
        comp_data = robustness_results[robustness_results['competitor'] == comp]
        ax.plot(comp_data['alpha'], comp_data['mean'], marker='o', label=comp)
        ax.fill_between(comp_data['alpha'],
                        comp_data['ci_lower'],
                        comp_data['ci_upper'],
                        alpha=0.2)
    ax.axhline(0, color='black', linestyle=':')
    ax.set_xlabel("Weight exponent α (L = file_size / throughput^α)")
    ax.set_ylabel("Mean ΔL with 95% CI")
    ax.set_title("Robustness of advantage to size/speed trade-off")
    ax.legend()
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output, dpi=150)
    plt.close()
    print(f"Saved robustness plot to {output}")


# ----------------------------- Main script ----------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Analyse compression library benchmarks."
    )
    parser.add_argument("directory", help="Directory containing the result CSV files")
    parser.add_argument("--own-library", default="compio_zstd1",
                        help="Name of your own library (default: compio_zstd1)")
    parser.add_argument("--competitors", nargs="+",
                        default=["seekable_zstd", "zran", "compio_zlib1"],
                        help="List of competitor libraries to compare against")
    parser.add_argument("--max-seeds", type=int, default=30,
                        help="Number of seeds to use (default: 30)")
    parser.add_argument("--robustness", action="store_true",
                        help="Perform robustness analysis over alpha values")
    parser.add_argument("--alpha-range", type=float, nargs=3,
                        default=[0.5, 1.5, 0.1],
                        help="Start, stop, step for alpha sweep (default: 0.5 1.5 0.1)")
    args = parser.parse_args()

    # 1. Load data
    print("Loading data ...")
    df = load_all_csvs(args.directory, args.max_seeds)
    print(f"Loaded {len(df)} rows. Libraries found: {df['library'].unique()}")
    if args.own_library not in df['library'].unique():
        raise ValueError(f"Own library '{args.own_library}' not found in data.")
    for comp in args.competitors:
        if comp not in df['library'].unique():
            print(f"Warning: competitor '{comp}' not found in data, removing from list.")
    competitors = [c for c in args.competitors if c in df['library'].unique()]

    # 2. Core analysis with alpha = 1 (original loss)
    print("\n--- Core analysis (α = 1) ---")
    optimal = select_optimal_per_seed(df, alpha=1.0)
    comparisons = paired_comparison(optimal, args.own_library, competitors)

    print("\nPaired differences ΔL = L(competitor) - L(ours) :\n")
    for comp, delta_df in comparisons.items():
        stats_res = summary_stats(delta_df)
        print(f"  {comp}:")
        print(f"    Mean ΔL = {stats_res['mean']:.4e}")
        print(f"    95% CI = [{stats_res['ci_lower']:.4e}, {stats_res['ci_upper']:.4e}]")
        if stats_res['ci_lower'] > 0:
            print("    -> Advantage is statistically significant (entire CI > 0)")
        elif stats_res['ci_upper'] < 0:
            print("    -> Disadvantage is statistically significant (entire CI < 0)")
        else:
            print("    -> Difference not statistically significant (CI includes 0)")
        print(f"    Based on {stats_res['n']} seeds.\n")

    # 3. Plots for alpha = 1
    plot_loss_curves(df, alpha=1.0, output="loss_curves.png")
    plot_delta_distribution(comparisons, output="delta_distribution.png")

    # 4. Optional robustness sweep
    if args.robustness:
        print("\n--- Robustness analysis ---")
        start, stop, step = args.alpha_range
        alphas = np.arange(start, stop + 0.5*step, step)
        robustness_rows = []

        for alpha in alphas:
            opt = select_optimal_per_seed(df, alpha=alpha)
            comps = paired_comparison(opt, args.own_library, competitors)
            for comp, delta_df in comps.items():
                s = summary_stats(delta_df)
                robustness_rows.append({
                    'alpha': alpha,
                    'competitor': comp,
                    'mean': s['mean'],
                    'ci_lower': s['ci_lower'],
                    'ci_upper': s['ci_upper'],
                    'n': s['n']
                })

        rob_df = pd.DataFrame(robustness_rows)
        print("\nRobustness summary (first / last rows):")
        print(rob_df.head())
        print("...")
        print(rob_df.tail())
        plot_robustness(rob_df, output="robustness.png")

    print("\nDone.")


if __name__ == "__main__":
    main()