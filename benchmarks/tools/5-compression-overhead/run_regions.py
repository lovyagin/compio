#!/usr/bin/env python3
"""
Run compression overhead measurements on 29 non-overlapping 32 MB regions
of a large input file.

Usage:
    python run_regions.py <binary> <input_file> <compressor> <level> <output_json>

The input file should be at least 29 * 32 MB = 928 MB.
"""

import argparse
import json
import os
import subprocess
import sys

REGION_SIZE = 32 * 1024 * 1024  # 32 MB
NUM_REGIONS = 2 # 29


def extract_region(input_file: str, offset: int, size: int) -> bytes:
    """Read a region from the input file at given offset."""
    with open(input_file, "rb") as f:
        f.seek(offset)
        return f.read(size)


def main():
    parser = argparse.ArgumentParser(
        description="Run compression overhead on 29 non-overlapping 32 MB regions"
    )
    parser.add_argument("binary", help="path to measure_one binary")
    parser.add_argument("input_file", help="path to large input file (>= 928 MB)")
    parser.add_argument("compressor", help="compressor name (e.g. zstd, zlib, lz4)")
    parser.add_argument("level", type=int, help="compression level")
    parser.add_argument("output_json", help="output JSON file path")
    args = parser.parse_args()

    # Validate input file size
    file_size = os.path.getsize(args.input_file)
    required_size = REGION_SIZE * NUM_REGIONS
    if file_size < required_size:
        print(
            f"Error: input file is {file_size} bytes, but need at least {required_size} bytes "
            f"({NUM_REGIONS} regions x {REGION_SIZE} bytes = {required_size // (1024*1024)} MB)",
            file=sys.stderr,
        )
        sys.exit(1)

    block_sizes = [2**i for i in range(8, 18)]  # 256 .. 65536

    # We'll store results as a dict: region_index -> list of {block_size, whole, blocked, archive}
    all_results = {}

    for region_idx in range(NUM_REGIONS):
        offset = region_idx * REGION_SIZE
        region_data = extract_region(args.input_file, offset, REGION_SIZE)

        # Write region to a temp file
        temp_path = f"/dev/shm/compio_region_{region_idx}_{os.getpid()}.tmp"
        with open(temp_path, "wb") as f:
            f.write(region_data)

        region_results = []
        for bs in block_sizes:
            proc = subprocess.run(
                [args.binary, str(bs), temp_path, args.compressor, str(args.level)],
                capture_output=True,
                text=True,
                check=True,
            )
            parts = proc.stdout.strip().split()
            if len(parts) != 3:
                raise RuntimeError(
                    f"Unexpected output for region {region_idx}, block_size {bs}: {proc.stdout}"
                )
            w, b, a = map(int, parts)
            region_results.append({"block_size": bs, "whole": w, "blocked": b, "archive": a})
            print(
                f"Region {region_idx + 1}/{NUM_REGIONS}, block_size {bs}: "
                f"whole={w}, blocked={b}, archive={a}"
            )

        os.remove(temp_path)
        all_results[region_idx] = {
            "offset": offset,
            "size": REGION_SIZE,
            "results": region_results,
        }

    data = {
        "input_file": args.input_file,
        "file_size": file_size,
        "region_size": REGION_SIZE,
        "num_regions": NUM_REGIONS,
        "compressor": args.compressor,
        "level": args.level,
        "regions": all_results,
    }
    with open(args.output_json, "w") as f:
        json.dump(data, f, indent=2)
    print(f"Saved results to {args.output_json}")


if __name__ == "__main__":
    main()
