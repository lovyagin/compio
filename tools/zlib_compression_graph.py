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
    parser.add_argument(
        "--eval-iterations", type=int, default=3, help="number of iterations to take max for speed evaluation"
    )
    args = parser.parse_args()

    if len(args.files) != len(args.titles):
        raise argparse.ArgumentError("each file must have a title (-f and -t options)")

    return args


def main(args: argparse.Namespace) -> None:
    fig, axes = plt.subplots(2, 1, sharex="col")
    fig.set_figwidth(6)
    fig.set_figheight(8)

    opened_files = []
    for filepath in args.files:
        with open(filepath, "rb") as f:
            data = f.read()
        if len(data) < args.max_bs:
            raise RuntimeError(f"file {filepath} is too small ({len(data)} < {args.max_bs})")
        opened_files.append(data)

    block_sizes = np.linspace(args.min_bs, args.max_bs, args.points, dtype=np.int32)
    parameters = list(itertools.product(range(len(args.titles)), enumerate(block_sizes), range(args.samples)))
    random.shuffle(parameters)

    comprates = np.zeros((len(args.files), args.points))
    speeds = np.zeros((len(args.files), args.points))

    for i, (j, block_size), _ in tqdm(parameters):
        data = opened_files[i]
        start = random.randint(0, len(data) - block_size)
        end = start + block_size
        block = bytes(data[start:end])

        start_time = time.time()
        compressed_data = zlib.compress(block)
        speeds[i, j] += block_size / 1000000 / (time.time() - start_time)
        comprates[i, j] += len(compressed_data) / block_size

    speeds /= args.samples
    comprates /= args.samples

    for i, title in enumerate(args.titles):
        axes[0].plot(block_sizes, comprates[i], label=title)
        axes[1].plot(block_sizes, speeds[i], label=title)

    axes[0].set(title="ZLIB compression rate by block size", ylabel="compression rate", ylim=(0, 1.2))
    axes[1].set(xlabel="block size", ylabel="MB/s", title="ZLIB compression speed by block size")
    axes[0].grid(True)
    axes[1].grid(True)
    axes[0].legend()
    axes[1].legend()
    fig.savefig(args.o)
    plt.close()


if __name__ == "__main__":
    main(parse_args())
