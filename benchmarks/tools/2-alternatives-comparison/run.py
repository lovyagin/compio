import argparse
import os
import subprocess
import sys

# COMPIO_BLOCK_SIZES = [2**i for i in range(9, 17)]
# SEEKABLE_ZSTD_FRAME_SIZES = [2**i for i in range(9, 17)]
# ZRAN_FLUSH_INTERVALS = []

# COMPIO_BLOCK_SIZES = [2**i for i in range(8, 22)]
# SEEKABLE_ZSTD_FRAME_SIZES = [2**i for i in range(9, 22)]
# ZRAN_FLUSH_INTERVALS = [2**i for i in range(15, 22)]

COMPIO_BLOCK_SIZES = [2**13]
SEEKABLE_ZSTD_FRAME_SIZES = [2**13]
ZRAN_FLUSH_INTERVALS = []

COMPIO_CONFIGS = [
    ("zstd", 1, "zstd1"),
    # ("zlib", 1, "zlib1"),
]


def run_benchmark(bin_path: str, args: list[str]) -> str | None:
    result = subprocess.run([bin_path] + args, capture_output=True, text=True)
    if result.returncode != 0:
        print(
            f"  ERROR (exit {result.returncode}): {result.stderr.strip()}",
            file=sys.stderr,
        )
        return None
    return result.stdout.strip()


def save_output(output_path: str, content: str):
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w") as f:
        f.write(content)
        f.write("\n")


def run_one(
    label: str, bin_path: str, output_dir: str, params: list[tuple[str, list[str]]]
):
    print(f"=== {label} ===")
    for tag, cli_args in params:
        out_path = os.path.join(output_dir, f"{label}_{tag}.csv")
        if os.path.exists(out_path):
            print(f"  [{tag}] SKIP (exists): {out_path}")
            continue
        print(f"  [{tag}] Running: {bin_path} {' '.join(cli_args)}")
        stdout = run_benchmark(bin_path, cli_args)
        if stdout is None:
            continue
        save_output(out_path, stdout)
        with open(out_path) as f:
            row_count = sum(1 for _ in f) - 1
        print(f"  [{tag}] Saved -> {out_path} ({row_count} data rows)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compio-bin", required=True)
    parser.add_argument("--seekable-zstd-bin", required=True)
    parser.add_argument("--zran-bin", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    for compressor, level, label in COMPIO_CONFIGS:
        compio_params = [
            (f"{bs}", [compressor, str(level), str(bs)]) for bs in COMPIO_BLOCK_SIZES
        ]
        run_one(f"compio_{label}", args.compio_bin, args.output_dir, compio_params)

    seekable_params = [(f"{fs}", [str(fs)]) for fs in SEEKABLE_ZSTD_FRAME_SIZES]
    run_one("seekable_zstd", args.seekable_zstd_bin, args.output_dir, seekable_params)

    # zran_params = [(f"{fi}", [str(fi)]) for fi in ZRAN_FLUSH_INTERVALS]
    # run_one("zran", args.zran_bin, args.output_dir, zran_params)

    print("\nAll experiments completed.")


if __name__ == "__main__":
    main()
