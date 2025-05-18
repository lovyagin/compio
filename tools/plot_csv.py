import pandas as pd
import matplotlib.pyplot as plt
import argparse


def plot_csv_columns(args: argparse.Namespace) -> None:
    data = pd.read_csv(args.infile)

    plt.figure(figsize=(10, 6))

    for column in data.columns:
        plt.plot(data[column], label=column)

    plt.xlabel("n_blocks")
    plt.ylabel("filesize")
    plt.legend()
    plt.grid(True)
    plt.savefig(args.outfile)
    plt.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("infile", type=str, help="path to CSV file")
    parser.add_argument("outfile", type=str, help="path to output image")
    return parser.parse_args()


if __name__ == "__main__":
    plot_csv_columns(parse_args())
