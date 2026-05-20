import argparse

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


def main():
    parser = argparse.ArgumentParser(
        description="Scatter plot of file_size and throughput ratios between two libraries."
    )
    parser.add_argument("file_a", help="CSV file for library A")
    parser.add_argument("file_b", help="CSV file for library B")
    parser.add_argument("--label-a", default="A", help="Legend label for library A")
    parser.add_argument("--label-b", default="B", help="Legend label for library B")
    parser.add_argument(
        "-o", "--output", default="ratio_scatter.svg", help="Output figure file"
    )
    args = parser.parse_args()

    df_a = pd.read_csv(args.file_a)
    df_b = pd.read_csv(args.file_b)

    required = {"file_offset", "file_size", "throughput"}
    for name, df in [("A", df_a), ("B", df_b)]:
        if not required.issubset(df.columns):
            raise ValueError(
                f"File {name} missing columns. Need {required}, got {set(df.columns)}"
            )

    agg_a = (
        df_a.groupby("file_offset")
        .agg(
            file_size=("file_size", "first"),
            throughput=("throughput", "mean"),
            throughput_std=("throughput", "std"),
        )
        .reset_index()
    )
    agg_b = (
        df_b.groupby("file_offset")
        .agg(
            file_size=("file_size", "first"),
            throughput=("throughput", "mean"),
            throughput_std=("throughput", "std"),
        )
        .reset_index()
    )

    merged = pd.merge(agg_a, agg_b, on="file_offset", suffixes=("_a", "_b"))
    if merged.empty:
        print("No matching file_offset values between the two files.")
        return

    merged["file_size_ratio"] = merged["file_size_a"] / merged["file_size_b"]
    merged["throughput_ratio"] = merged["throughput_a"] / merged["throughput_b"]

    # Propagate stddev through ratio: σ_r / r = sqrt((σ_a/t_a)² + (σ_b/t_b)²)
    rel_var_a = (merged["throughput_std_a"] / merged["throughput_a"]).fillna(0) ** 2
    rel_var_b = (merged["throughput_std_b"] / merged["throughput_b"]).fillna(0) ** 2
    merged["throughput_ratio_err"] = merged["throughput_ratio"] * np.sqrt(
        rel_var_a + rel_var_b
    )

    merged = merged.dropna(subset=["file_size_ratio", "throughput_ratio"])
    merged = merged[(merged["file_size_ratio"] > 0) & (merged["throughput_ratio"] > 0)]

    if merged.empty:
        print("No valid ratio data to plot.")
        return

    plt.figure(figsize=(8, 7))
    ax = plt.gca()

    ax.errorbar(
        merged["file_size_ratio"],
        merged["throughput_ratio"],
        yerr=merged["throughput_ratio_err"],
        fmt="o",
        color="#1f77b4",
        ecolor="#1f77b4",
        capsize=2,
        alpha=0.7,
        markeredgecolor="black",
        markeredgewidth=0.5,
    )

    ax.set_xlabel(
        f"File Size Ratio ($\\frac{{\\text{{{args.label_a}}}}}{{\\text{{{args.label_b}}}}}$)"
    )
    ax.set_ylabel(
        f"Throughput Ratio ($\\frac{{\\text{{{args.label_a}}}}}{{\\text{{{args.label_b}}}}}$)"
    )
    ax.set_title(
        f"Read · Throughput and File size comparison · {args.label_a} vs {args.label_b}\n"
        "block_size (max_frame_size) = 8192"
    )
    ax.grid(True, linestyle=":", alpha=0.7)

    # Pad axes slightly so points aren't clipped at edges
    xlim = ax.get_xlim()
    ylim = ax.get_ylim()
    ax.set_xlim(left=xlim[0] * 0.999, right=xlim[1] * 1.001)
    ax.set_ylim(bottom=ylim[0] * 0.999, top=ylim[1] * 1.001)

    # Fill regions above/below y=x
    xlo, xhi = ax.get_xlim()
    ylo, yhi = ax.get_ylim()
    x_fill = np.linspace(xlo, xhi, 300)

    def _ratio_label(a, b, op):
        return rf"$\text{{{a}}}\ \frac{{throughput}}{{file\ size}} {op} \text{{{b}}}\ \frac{{throughput}}{{file\ size}}$"

    ax.fill_between(
        x_fill,
        x_fill,
        yhi,
        color="green",
        alpha=0.08,
        zorder=0,
        label=_ratio_label(args.label_a, args.label_b, ">"),
    )
    ax.fill_between(
        x_fill,
        ylo,
        x_fill,
        color="red",
        alpha=0.08,
        zorder=0,
        label=_ratio_label(args.label_a, args.label_b, "<"),
    )

    ax.axline(
        [1, 1],
        [2, 2],
        color="red",
        linewidth=1.5,
        alpha=0.5,
        label=_ratio_label(args.label_a, args.label_b, "="),
    )

    ax.legend(loc="best")

    plt.tight_layout()
    plt.savefig(args.output, bbox_inches="tight")
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
