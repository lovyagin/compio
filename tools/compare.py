import argparse
import json
import re
import numpy as np

from collections import defaultdict


REPL_TOKEN = r"<$>"


def format_units(value: float, step: float, names: list[str], precision: int = 1) -> tuple[float, str]:
    for i, x in enumerate(names):
        if value < step or i == len(names) - 1:
            return f"{{:.{precision}f}}".format(value), x
        value /= step


def format_(value: float, type_: str) -> tuple[float, str]:
    if type_ == "ms":
        return format_units(value, 1000, ["ms", "s"])
    elif type_ == "size":
        return format_units(value, 1000, ["B", "KB", "MB", "GB", "TB"])
    elif type_ == "speed":
        return format_units(value, 1000, ["B/s", "KB/s", "MB/s", "GB/s", "TB/s"])
    elif type_ == "plain":
        return format_units(value, 1, [""])
    else:
        raise ValueError(f"unknown format type: {type_}")


FORMATS = ["ms", "size", "speed", "plain"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("report_file", type=str, help="path to benchmark report")
    filters_action = parser.add_argument("--filters", default=[], nargs="+", help="filters to compare")
    counter_action = parser.add_argument(
        "--counter",
        action="append",
        nargs="+",
        help="custom counter (<name> <format-type> <show-std> <more-is-better>)",
    )
    parser.add_argument("--context-keys", default=[], nargs="+", help="context keys to print in the header")
    parser.add_argument("-o", "--output", type=str, default=None, help="path to output file")
    args = parser.parse_args()

    if len(args.filters) == 0:
        raise argparse.ArgumentError(filters_action, "specify at least one filter")

    for c in args.counter:
        if len(c) != 4:
            raise argparse.ArgumentError(
                counter_action,
                f"each --counter argument must specify <name> <format-type> <show-std> <more-is-better> ({c})",
            )
        if c[1] not in FORMATS:
            raise argparse.ArgumentError(counter_action, f"unknown format type: {c[1]}")
        c[2] = bool(int(c[2]))
        c[3] = bool(int(c[3]))

    return args


def main(args: argparse.Namespace) -> None:
    with open(args.report_file, "r") as f:
        report = json.load(f)

    context = report["context"]
    benchmarks = report["benchmarks"]

    result = "# Benchmark report\n\n"
    for key in args.context_keys:
        result += f"+ {key}: {context[key]}\n"
    result += "\n"

    filters = [re.compile(f) for f in args.filters]

    corr_benchmarks = [defaultdict(list) for _ in args.filters]

    for benchmark in benchmarks:
        if "repetition_index" not in benchmark:
            # aggregate value
            continue
        for i, f in enumerate(filters):
            if f.search(benchmark["name"]) is not None:
                key = f.sub(REPL_TOKEN, benchmark["name"])
                corr_benchmarks[i][key].append(benchmark)
                break

    result += "| benchmark_name "
    for c in args.counter:
        name = c[0]
        for f in args.filters:
            result += f"| {name} [{f}] "
    result += "|\n" + "-".join(["|" for _ in range(len(args.counter) * len(args.filters) + 2)]) + "\n"

    for key in set.union(*[set(corr_benchmarks[i].keys()) for i in range(len(args.filters))]):
        title = re.sub(re.escape(REPL_TOKEN), f"[{' vs '.join(args.filters)}]", key)

        result += f"| {title} "
        for name, type_, show_std, more_is_better in args.counter:
            column_values = []
            for i in range(len(args.filters)):
                values = np.array([b[name] for b in corr_benchmarks[i][key]])
                if show_std:
                    mean_val, mean_unit = format_(values.mean(), type_)
                    std_val, std_unit = format_(values.std(), type_)
                    column_values.append((f"{mean_val} {mean_unit} ± {std_val} {std_unit}", values.mean()))
                else:
                    mean_val, mean_unit = format_(values.mean(), type_)
                    column_values.append((f"{mean_val} {mean_unit}", values.mean()))
            max_column_idx = sorted(enumerate(column_values), key=lambda x: x[1][1], reverse=more_is_better)[0][0]
            column_values = [x[0] for x in column_values]
            column_values[max_column_idx] = f"**{column_values[max_column_idx]}**"
            result += "| " + " | ".join(column_values) + " "
        result += "|\n"

    if args.output:
        with open(args.output, "w+") as f:
            f.write(result)
    else:
        print(result)


if __name__ == "__main__":
    main(parse_args())
