import argparse
import os
import subprocess
import sys

COMPIO_BLOCK_SIZES = [2**i for i in range(11, 15)]
SEEKABLE_ZSTD_FRAME_SIZES = [2**i for i in range(12, 22)]
ZRAN_FLUSH_INTERVALS = [2**i for i in range(15, 19)]

COMPIO_CONFIGS = [
    ("zstd", 1, "zstd1"),
    ("zlib", 1, "zlib1"),
]


def run_benchmark(bin_path: str, args: list[str]) -> str | None:
    """Run a benchmark binary and return its stdout, or None on failure."""
    result = subprocess.run(
        [bin_path] + args,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  ERROR (exit {result.returncode}): {result.stderr.strip()}", file=sys.stderr)
        return None
    return result.stdout.strip()


def save_output(output_path: str, content: str):
    """Save raw stdout content to a file."""
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w") as f:
        f.write(content)
        f.write("\n")


def run_compio(bin_path: str, output_dir: str):
    """Run optimal_compio for each compressor config and block_size."""
    for compressor, level, label in COMPIO_CONFIGS:
        print(f"=== optimal_compio ({compressor}, level {level}) ===")
        for bs in COMPIO_BLOCK_SIZES:
            out_path = os.path.join(output_dir, f"compio_{label}_bs{bs}.txt")
            if os.path.exists(out_path):
                print(f"  [{bs}] SKIP (exists): {out_path}")
                continue

            print(f"  [{bs}] Running: {bin_path} {compressor} {level} {bs}")
            stdout = run_benchmark(bin_path, [compressor, str(level), str(bs)])
            if stdout is None:
                continue

            save_output(out_path, stdout)
            print(f"  [{bs}] Result: {stdout}")
            print(f"  [{bs}] Saved -> {out_path}")


def run_seekable_zstd(bin_path: str, output_dir: str):
    """Run optimal_seekable_zstd for each max_frame_size."""
    print("=== optimal_seekable_zstd ===")
    for fs in SEEKABLE_ZSTD_FRAME_SIZES:
        out_path = os.path.join(output_dir, f"seekable_zstd_fs{fs}.txt")
        if os.path.exists(out_path):
            print(f"  [{fs}] SKIP (exists): {out_path}")
            continue

        print(f"  [{fs}] Running: {bin_path} {fs}")
        stdout = run_benchmark(bin_path, [str(fs)])
        if stdout is None:
            continue

        save_output(out_path, stdout)
        print(f"  [{fs}] Result: {stdout}")
        print(f"  [{fs}] Saved -> {out_path}")


def run_zran(bin_path: str, output_dir: str):
    """Run optimal_zran for each flush_interval."""
    print("=== optimal_zran ===")
    for fi in ZRAN_FLUSH_INTERVALS:
        out_path = os.path.join(output_dir, f"zran_fi{fi}.txt")
        if os.path.exists(out_path):
            print(f"  [{fi}] SKIP (exists): {out_path}")
            continue

        print(f"  [{fi}] Running: {bin_path} {fi}")
        stdout = run_benchmark(bin_path, [str(fi)])
        if stdout is None:
            continue

        save_output(out_path, stdout)
        print(f"  [{fi}] Result: {stdout}")
        print(f"  [{fi}] Saved -> {out_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Run custom optimal benchmarks with varying block sizes."
    )
    parser.add_argument(
        "--compio-bin",
        required=True,
        help="Path to the optimal_compio binary",
    )
    parser.add_argument(
        "--seekable-zstd-bin",
        required=True,
        help="Path to the optimal_seekable_zstd binary",
    )
    parser.add_argument(
        "--zran-bin",
        required=True,
        help="Path to the optimal_zran binary",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory to write results into",
    )
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    run_compio(args.compio_bin, args.output_dir)
    run_seekable_zstd(args.seekable_zstd_bin, args.output_dir)
    run_zran(args.zran_bin, args.output_dir)

    print("\nAll experiments completed.")


if __name__ == "__main__":
    main()
