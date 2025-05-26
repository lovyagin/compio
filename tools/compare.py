import argparse
import json
import re
import csv
import itertools
import numpy as np
from scipy.stats import mannwhitneyu
from typing import List, Tuple, Dict, Any

from dataclasses import dataclass
from collections import defaultdict


def format_units(value: float, step: float, names: List[str], precision: int = 1) -> Tuple[float, str]:
    for i, x in enumerate(names):
        if value < step or i == len(names) - 1:
            return f"{{:.{precision}f}}".format(value), x
        value /= step


def format_(value: float, type_: str) -> Tuple[float, str]:
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


def smart_split(s: str, sep: str) -> Tuple[str]:
    return next(iter(csv.reader([s], delimiter=sep)))


def function_arguments(s: str) -> Tuple[List[Any], Dict[str, Any]]:
    args = []
    kwargs = {}

    for arg in smart_split(s, ","):
        s_arg = arg.split("=")
        if len(s_arg) == 1:
            args.append(arg)
        elif len(s_arg) == 2:
            kwargs[s_arg[0]] = s_arg[1]
        else:
            raise ValueError(f"invalid argument: {s}")

    return args, kwargs


def dataclass_arguments(cls: Any) -> Any:
    def wrapped(s: str) -> Any:
        args, kwargs = function_arguments(s)
        return cls(*args, **kwargs)

    return wrapped


@dataclass
class ArgCounter:
    name: str
    format_type: str
    show_std: str = "false"
    reversed: str = "false"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--report-files",
        type=str,
        nargs="+",
        help="paths to gbench report files",
    )
    parser.add_argument(
        "--filters",
        default=[],
        nargs="*",
        type=str,
        help="benchmark filters to compare",
    )
    parser.add_argument(
        "--names",
        nargs="+",
        type=str,
        help="benchmark+filter names for output",
    )
    parser.add_argument(
        "--counters",
        default=[],
        nargs="*",
        type=dataclass_arguments(ArgCounter),
        help="add counter column (name, format_type, show_std = 'false', reversed = 'false')",
    )
    parser.add_argument(
        "--context-keys",
        default=[],
        type=str,
        nargs="*",
        help="context keys to print in the header",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default=None,
        help="path to output file",
    )

    args = parser.parse_args()
    if len(args.report_files) == 1 and len(args.filters) > 0:
        args.inputs = list(zip(itertools.repeat(args.report_files[0]), args.filters))
        args.input_type = "one benchmark, many filters"
    elif len(args.report_files) > 1 and len(args.filters) == 0:
        args.inputs = list(zip(args.report_files, itertools.repeat("")))
        args.input_type = "many benchmarks, no filters"
    elif len(args.report_files) > 1 and len(args.filters) == 1:
        args.inputs = list(zip(args.report_files, itertools.repeat(args.filters[0])))
        args.input_type = "many benchmarks, one filter"
    elif len(args.report_files) == len(args.filters):
        args.inputs = list(zip(args.report_files, args.filters))
        args.input_type = "many benchmarks, many filters"
    else:
        raise ValueError("invalid number of report_files and filters")

    if len(args.inputs) != len(args.names):
        raise ValueError("invalid number of names")
    else:
        args.inputs = list(zip(*zip(*args.inputs), args.names))

    return args


def format_table_row(values: List[str]) -> str:
    return "| " + " | ".join(values) + " |\n"


def format_table_rows(rows: List[List[str]]) -> str:
    return "".join([format_table_row(row) for row in rows])


def highlight_value(value: str) -> str:
    # return f"**{value}**"
    return f'<span style="color: #87d1ff">**{value}**</span>'


def get_highlighted_indices(values: np.ndarray, reversed: bool = False, alpha: float = 1) -> List[int]:
    if reversed:
        values *= -1
    means, stds = np.mean(values, axis=1), np.std(values, axis=1, ddof=1)
    best_i = np.argmax(means)
    indices = []
    for i in range(len(means)):
        if means[i] + stds[i] * alpha >= means[best_i] - stds[best_i] * alpha:
            indices.append(i)
    return indices


def main(args: argparse.Namespace) -> None:
    # result = "# Benchmark report\n\n"
    result = ""
    keys = []
    all_benchmarks = []
    context_printed = False

    for report_file, filter_, name in args.inputs:
        with open(report_file, "r", encoding="utf-8") as f:
            report = json.load(f)

        if len(args.context_keys) > 0:
            if "one benchmark" not in args.input_type or not context_printed:
                result += f"### {name}:\n\n"
                for key in args.context_keys:
                    result += f"+ {key}: {report['context'][key]}\n"
                result += "\n"
                context_printed = True

        filter_re = re.compile(filter_)
        benchmarks_by_key = defaultdict(list)

        for benchmark in report["benchmarks"]:
            if "repetition_index" not in benchmark:
                # aggregate value
                continue
            if filter_re.search(benchmark["name"]) is not None:
                key = filter_re.sub("", benchmark["name"])
                benchmarks_by_key[key].append(benchmark)
                if key not in keys:
                    keys.append(key)

        all_benchmarks.append(benchmarks_by_key)

    N = len(args.inputs)
    mutual_keys = set.intersection(*[set(all_benchmarks[i].keys()) for i in range(N)])
    keys = [k for k in keys if k in mutual_keys]

    rows = []

    # table header
    rows.append(["benchmark_name"] + [f"{c.name} [{name}]" for c in args.counters for _, _, name in args.inputs])

    # separator
    rows.append(["-"] * (len(args.counters) * N + 1))

    # rows
    for key in keys:
        row = [key]
        for counter in args.counters:
            row_segment = []
            all_values = []
            for i in range(N):
                values = np.array([b[counter.name] for b in all_benchmarks[i][key]])
                mean_val, mean_unit = format_(values.mean(), counter.format_type)
                formatted_value = f"{mean_val} {mean_unit}"
                if counter.show_std == "true":
                    std_val, std_unit = format_(values.std(ddof=1), counter.format_type)
                    formatted_value += f" ± {std_val} {std_unit}"
                row_segment.append(formatted_value)
                all_values.append(values)

            indices = get_highlighted_indices(np.array(all_values), reversed=counter.reversed == "false")
            for idx in indices:
                row_segment[idx] = highlight_value(row_segment[idx])

            row.extend(row_segment)
        rows.append(row)

    result += format_table_rows(rows)

    if args.output:
        with open(args.output, "w+") as f:
            f.write(result)
    else:
        print(result)


if __name__ == "__main__":
    main(parse_args())
