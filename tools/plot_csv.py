import argparse


def plot_csv_columns(args: argparse.Namespace) -> None:
    import matplotlib.pyplot as plt
    import pandas as pd

    data = pd.read_csv(args.infile)

    plt.figure(figsize=(10, 6))

    if len(args.columns) > 0:
        columns = [col for col in data.columns if col in args.columns]
    else:
        columns = data.columns

    if args.smoothing is not None:
        try:
            window_length, polyorder = list(map(int, args.smoothing.split(",")))
        except Exception as e:
            raise argparse.ArgumentError(None, "--smoothing must be two comma separated integers") from e
        from scipy.signal import savgol_filter

    for column in columns:
        if args.smoothing is not None:
            data[column] = savgol_filter(data[column], window_length, polyorder)
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
    parser.add_argument(
        "-C", "--columns", type=str, nargs="*", help="columns to show (default: all)"
    )
    parser.add_argument(
        "--smoothing",
        type=str,
        help="smoothing parameters: window length and polyorder separated by comma (default: no smoothing)",
    )
    parser.add_argument("outfile", type=str, help="path to output image")
    return parser.parse_args()


if __name__ == "__main__":
    plot_csv_columns(parse_args())
