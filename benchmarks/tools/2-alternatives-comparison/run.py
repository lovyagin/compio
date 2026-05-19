import argparse
import csv
import os
import subprocess
import sys

COMPIO_BLOCK_SIZES = [2**i for i in range(8, 22)]
SEEKABLE_ZSTD_FRAME_SIZES = [2**i for i in range(9, 22)]
ZRAN_FLUSH_INTERVALS = [2**i for i in range(15, 22)]

COMPIO_CONFIGS = [
    ("zstd", 1, "zstd1"),
    ("zlib", 1, "zlib1"),
]


def run_benchmark(bin_path: str, args: list[str]) -> str:
    """Run a benchmark binary and return its stdout, or raise on failure."""
    result = subprocess.run(
        [bin_path] + args,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  ERROR (exit {result.returncode}): {result.stderr.strip()}", file=sys.stderr)
        return None
    return result.stdout.strip()


def parse_csv_line(stdout: str, num_fields: int) -> list[str] | None:
    """Parse a single CSV line from stdout. Returns list of fields or None."""
    line = stdout.strip()
    if not line:
        return None
    parts = line.split(",")
    if len(parts) != num_fields:
        print(f"  WARNING: expected {num_fields} fields, got {len(parts)}: {line}", file=sys.stderr)
        return None
    return parts


def save_csv(output_path: str, row: dict):
    """Append a single row to a CSV file, writing headers if file is new."""
    fieldnames = ["throughput_mean", "throughput_stddev", "file_size"]
    write_header = not os.path.exists(output_path)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def run_compio(bin_path: str, output_dir: str):
    """Run optimal_compio for each compressor config and block_size."""
    for compressor, level, label in COMPIO_CONFIGS:
        print(f"=== optimal_compio ({compressor}, level {level}) ===")
        for bs in COMPIO_BLOCK_SIZES:
            csv_path = os.path.join(output_dir, f"compio_{label}_bs{bs}.csv")
            if os.path.exists(csv_path):
                print(f"  [{bs}] SKIP (exists): {csv_path}")
                continue

            print(f"  [{bs}] Running...", end=" ")
            stdout = run_benchmark(bin_path, [compressor, str(level), str(bs)])
            if stdout is None:
                continue

            parts = parse_csv_line(stdout, num_fields=4)
            if parts is None:
                continue

            # parts: mean, stddev, avg_block_hit, file_size_stored
            row = {
                "throughput_mean": parts[0],
                "throughput_stddev": parts[1],
                "file_size": parts[3],
            }
            save_csv(csv_path, row)
            print(f"OK -> {csv_path}")


def run_seekable_zstd(bin_path: str, output_dir: str):
    """Run optimal_seekable_zstd for each max_frame_size."""
    print("=== optimal_seekable_zstd ===")
    for fs in SEEKABLE_ZSTD_FRAME_SIZES:
        csv_path = os.path.join(output_dir, f"seekable_zstd_fs{fs}.csv")
        if os.path.exists(csv_path):
            print(f"  [{fs}] SKIP (exists): {csv_path}")
            continue

        print(f"  [{fs}] Running...", end=" ")
        stdout = run_benchmark(bin_path, [str(fs)])
        if stdout is None:
            continue

        parts = parse_csv_line(stdout, num_fields=3)
        if parts is None:
            continue

        # parts: mean, stddev, file_size_stored
        row = {
            "throughput_mean": parts[0],
            "throughput_stddev": parts[1],
            "file_size": parts[2],
        }
        save_csv(csv_path, row)
        print(f"OK -> {csv_path}")


def run_zran(bin_path: str, output_dir: str):
    """Run optimal_zran for each flush_interval."""
    print("=== optimal_zran ===")
    for fi in ZRAN_FLUSH_INTERVALS:
        csv_path = os.path.join(output_dir, f"zran_fi{fi}.csv")
        if os.path.exists(csv_path):
            print(f"  [{fi}] SKIP (exists): {csv_path}")
            continue

        print(f"  [{fi}] Running...", end=" ")
        stdout = run_benchmark(bin_path, [str(fi)])
        if stdout is None:
            continue

        parts = parse_csv_line(stdout, num_fields=3)
        if parts is None:
            continue

        # parts: mean, stddev, file_size_stored
        row = {
            "throughput_mean": parts[0],
            "throughput_stddev": parts[1],
            "file_size": parts[2],
        }
        save_csv(csv_path, row)
        print(f"OK -> {csv_path}")


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
        help="Directory to write CSV results into",
    )
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    run_compio(args.compio_bin, args.output_dir)
    run_seekable_zstd(args.seekable_zstd_bin, args.output_dir)
    run_zran(args.zran_bin, args.output_dir)

    print("\nAll experiments completed.")


if __name__ == "__main__":
    main()
