import argparse
import zlib
import time
import random
import itertools
import numpy as np
import matplotlib.pyplot as plt
from tqdm import tqdm


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("-f", "--files", nargs="+", help="list of files with sample data")
    parser.add_argument("-t", "--titles", nargs="+", help="corresponding names on graph for each file")
    parser.add_argument("-o", type=str, required=True, help="output image path")
    parser.add_argument("--samples", type=int, default=100, help="number of samples for measuring")
    parser.add_argument("--min-bs", type=int, default=64, help="min block size")
    parser.add_argument("--max-bs", type=int, default=8192, help="max block size")
    parser.add_argument("--points", dest="points", type=int, default=50, help="number of points on graph")
    args = parser.parse_args()

    if len(args.files) != len(args.titles):
        raise argparse.ArgumentError("each file must have a title (-f and -t options)")

    return args


def main(args: argparse.Namespace) -> None:
    fig, axes = plt.subplots(1, 1, sharex="col")
    fig.set_figwidth(6)
    fig.set_figheight(4)

    progress = iter(tqdm(range(len(args.files) * args.points * args.samples)))
    block_sizes = np.linspace(args.min_bs, args.max_bs, args.points, dtype=np.int32)

    for filepath, title in zip(args.files, args.titles):
        with open(filepath, "rb") as f:
            data = f.read()
        if len(data) < args.max_bs:
            raise RuntimeError(f"file {filepath} is too small ({len(data)} < {args.max_bs})")

        comprates = np.zeros(args.points)

        for j, block_size in enumerate(block_sizes):
            for _ in range(args.samples):
                next(progress)
                start = random.randint(0, len(data) - block_size)
                end = start + block_size
                block = bytes(data[start:end])
                comprates[j] += len(zlib.compress(block)) / block_size

        comprates /= args.samples
        axes.plot(block_sizes, comprates, label=title)

    axes.set(title="ZLIB compression rate by block size", ylabel="compression rate", ylim=(0, 1.2))
    axes.grid(True)
    axes.legend()
    fig.savefig(args.o)
    plt.close()


if __name__ == "__main__":
    main(parse_args())
