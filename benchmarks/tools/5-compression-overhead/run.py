import argparse
import json
import os
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", help="path to measure_one binary")
    parser.add_argument("input_file", help="input file to compress")
    parser.add_argument("compressor", help="compressor name")
    parser.add_argument("level", type=int, help="compression level")
    parser.add_argument("output_json", help="output JSON file path")
    args = parser.parse_args()

    block_sizes = [2**i for i in range(8, 18)]
    original_size = os.path.getsize(args.input_file)

    results = []
    for bs in block_sizes:
        proc = subprocess.run(
            [args.binary, str(bs), args.input_file, args.compressor, str(args.level)],
            capture_output=True,
            text=True,
            check=True,
        )
        parts = proc.stdout.strip().split()
        if len(parts) != 3:
            raise RuntimeError(f"Unexpected output: {proc.stdout}")
        w, b, a = map(int, parts)
        results.append({"block_size": bs, "whole": w, "blocked": b, "archive": a})
        print(f"Block size {bs}: whole={w}, blocked={b}, archive={a}")

    data = {
        "original_size": original_size,
        "compressor": args.compressor,
        "level": args.level,
        "input_file": args.input_file,
        "results": results,
    }
    with open(args.output_json, "w") as f:
        json.dump(data, f, indent=2)
    print(f"Saved to {args.output_json}")


if __name__ == "__main__":
    main()
