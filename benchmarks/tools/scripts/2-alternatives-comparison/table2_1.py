import argparse
import glob
import os
import re
import sys

import pandas as pd


def parse_filename(filepath):
    basename = os.path.splitext(os.path.basename(filepath))[0]
    m = re.match(r"^(.+)_(\d+)$", basename)
    if not m:
        raise ValueError(f"Cannot parse filename: {basename}")
    return m.group(1), int(m.group(2))


def fmt_throughput(val_mbs):
    """Format a throughput value in MB/s to a nice string with comma as decimal separator."""
    if val_mbs >= 100:
        return f"{val_mbs:.0f}"
    elif val_mbs >= 10:
        return f"{val_mbs:.1f}".replace(".", ",")
    else:
        return f"{val_mbs:.2f}".replace(".", ",")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="*")
    parser.add_argument("-k", "--keys", nargs="*", type=int,
                        help="Filter by file_offset values (e.g. -k 503316480 469762048 33554432)")
    args = parser.parse_args()

    files = sorted(args.files) if args.files else sorted(glob.glob("results/*.csv"))
    if not files:
        print("No files found.", file=sys.stderr)
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
    n_libs = len(libraries)

    # Collect all unique file_offsets across all libraries
    all_offsets = sorted(set(
        offset
        for lib in libraries
        for bs in all_data[lib]
        for offset in all_data[lib][bs]["file_offset"].unique()
    ))

    # Filter offsets if -k is given
    if args.keys is not None:
        offsets = sorted(set(all_offsets) & set(args.keys))
    else:
        offsets = all_offsets

    if not offsets:
        print("No data to display after filtering.", file=sys.stderr)
        return

    # Collect all unique block sizes across all libraries
    all_block_sizes = sorted(set(
        bs
        for lib in libraries
        for bs in all_data[lib].keys()
    ))

    # Build the table data: for each (lib, file_offset) -> dict of bs -> (mean, std)
    table = {}
    for lib in libraries:
        for ds in offsets:
            key = (lib, ds)
            table[key] = {}
            for bs in all_block_sizes:
                if bs not in all_data[lib]:
                    continue
                df = all_data[lib][bs]
                sub = df[df["file_offset"] == ds]
                if sub.empty:
                    continue
                vals = sub["throughput"] / 1e6
                table[key][bs] = (vals.mean(), vals.std())

    n_regions = len(offsets)
    # Column spec: one "l" for block size, then for each region: 2 columns (one per library)
    col_spec = "l" + ("cc") * n_regions
    print("\\begin{tabular}{" + col_spec + "}")
    print("\\toprule")

    # --- Header row 1: multirow block size + multicolumn throughput ---
    total_data_cols = n_regions * n_libs
    print(
        r"  \multirow{2}{*}{\shortstack[l]{Размер блока\\[3pt](байт)}} "
        + f"& \\multicolumn{{{total_data_cols}}}{{c}}{{Пропускная способность (МБ/с)}} \\\\"
    )
    print(f"  \\cmidrule(lr){{2-{total_data_cols + 1}}}")

    # --- Header row 2: region names spanning 2 columns each ---
    header2 = [""]
    for ri, ds in enumerate(offsets, start=1):
        header2.append(f"\\multicolumn{{2}}{{c}}{{Регион {ri}}}")
    print("  " + " & ".join(header2) + " \\\\")

    # --- Header row 3: library names under each region ---
    lib_labels = [lib.replace("_", " ") for lib in libraries]
    def shorten(lib):
        if lib == "seekable zstd":
            return "s. zstd"
        elif "compio" in lib:
            return "compio"
        return lib
    lib_labels = [shorten(lb) for lb in lib_labels]
    header3 = [""]
    for _ in offsets:
        for lbl in lib_labels:
            header3.append(f"\\textbf{{{lbl}}}")
    print("  " + " & ".join(header3) + " \\\\")
    print("  \\midrule")

    # --- Data rows: one per block size ---
    for bs in all_block_sizes:
        row = [f"  {bs}"]
        for ds in offsets:
            # Collect values for this (bs, ds) across libraries to find the best
            vals_for_region = {}
            for lib in libraries:
                key = (lib, ds)
                if bs in table.get(key, {}):
                    mean, std = table[key][bs]
                    vals_for_region[lib] = (mean, std)

            # Find the library with the highest mean
            best_lib = None
            best_mean = -1
            for lib, (mean, _) in vals_for_region.items():
                if mean > best_mean:
                    best_mean = mean
                    best_lib = lib

            for lib in libraries:
                if lib in vals_for_region:
                    mean, std = vals_for_region[lib]
                    mean_str = fmt_throughput(mean)
                    std_str = fmt_throughput(std)
                    if lib == best_lib:
                        mean_str = f"\\textbf{{{mean_str}}}"
                    cell = f"{mean_str}{{\\small$\\,\\pm\\,{std_str}$}}"
                else:
                    cell = "--"
                row.append(cell)
        print("  " + " & ".join(row) + " \\\\")

    print("  \\bottomrule")
    print("\\end{tabular}")


if __name__ == "__main__":
    main()
