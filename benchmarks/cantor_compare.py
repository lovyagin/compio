#!/usr/bin/env python3
"""Runs cantor_set for every backend several times and prints a comparison table.

usage: cantor_compare.py <cantor_set binary> [--runs N] [--dir PATH] [-- extra cantor_set options]
"""
import argparse
import re
import statistics
import subprocess
import sys

CONFIGS = [
    ("ram", ["--backend=ram"]),
    ("stdio", ["--backend=stdio"]),
    ("compio none", ["--backend=compio", "--compressor=none"]),
    ("compio none, no wal", ["--backend=compio", "--compressor=none", "--wal=0"]),
    ("compio lz4", ["--backend=compio", "--compressor=lz4"]),
    ("compio zstd", ["--backend=compio", "--compressor=zstd"]),
    ("compio zlib", ["--backend=compio", "--compressor=zlib"]),
]


def run(binary, args):
    out = subprocess.run([binary] + args, check=True, capture_output=True, text=True).stdout
    total = float(re.search(r"total_seconds=([\d.]+)", out).group(1))
    cpu = float(re.search(r"cpu_seconds=([\d.]+)", out).group(1))
    disk = int(re.search(r"disk_bytes=(\d+)", out).group(1))
    rss = int(re.search(r"max_rss_kb=(\d+)", out).group(1))
    checksum = re.search(r"checksum=([0-9a-f]+)", out).group(1)
    return total, cpu, disk, rss, checksum


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--dir", default=None)
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()
    extra = args.extra + ([f"--dir={args.dir}"] if args.dir else [])

    checksums = set()
    print("| candidate | median s | min s | median cpu s | disk MiB | max RSS MiB |")
    print("|---|---|---|---|---|---|")
    for name, options in CONFIGS:
        results = [run(args.binary, options + extra) for _ in range(args.runs)]
        checksums.update(r[4] for r in results)
        times = [r[0] for r in results]
        print(f"| {name} | {statistics.median(times):.2f} | {min(times):.2f} | "
              f"{statistics.median(r[1] for r in results):.2f} | "
              f"{results[0][2] / 2**20:.0f} | {results[0][3] / 1024:.0f} |")
    if len(checksums) != 1:
        print(f"checksums differ: {sorted(checksums)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
