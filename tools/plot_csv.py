import pandas as pd
import matplotlib.pyplot as plt
import argparse


def plot_csv_columns(args: argparse.Namespace) -> None:
    data = pd.read_csv(args.infile)

    plt.figure(figsize=(10, 6))

    for column in data.columns:
        plt.plot(data[column], label=column)

    plt.xlabel(args.xlabel)
    plt.ylabel(args.ylabel)
    if args.legend:
        plt.legend()
    plt.grid(True)
    if args.xlog:
        plt.xscale("log")
    plt.savefig(args.outfile)
    plt.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("infile", type=str, help="path to CSV file")
    parser.add_argument("--xlabel", type=str, default="")
    parser.add_argument("--ylabel", type=str, default="")
    parser.add_argument("--xlog", action="store_true")
    parser.add_argument("--legend", action="store_true")
    parser.add_argument("outfile", type=str, help="path to output image")
    return parser.parse_args()


if __name__ == "__main__":
    plot_csv_columns(parse_args())
