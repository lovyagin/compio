import argparse
import json
import re
import csv
import numpy as np
from typing import List, Tuple, Dict, Any

from dataclasses import dataclass
from collections import defaultdict


def format_units(
    value: float, step: float, names: List[str], precision: int = 1
) -> Tuple[float, str]:
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


@dataclass
class ArgFilter:
    filter: str
    name: str | None = None

    def __post_init__(self):
        if self.name is None:
            self.name = self.filter


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "report_file",
        type=str,
        help="path to gbench report file",
    )
    parser.add_argument(
        "filters",
        nargs=2,
        type=dataclass_arguments(ArgFilter),
        help="benchmark filters to compare (filter, [name])",
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
    return parser.parse_args()


def format_table_row(values: List[str]) -> str:
    return "| " + " | ".join(values) + " |\n"


def format_table_rows(rows: List[List[str]]) -> str:
    return "".join([format_table_row(row) for row in rows])


def highlight_value(value: str) -> str:
    return f"**{value}**"


def main(args: argparse.Namespace) -> None:
    with open(args.report_file, "r", encoding="utf-8") as f:
        report = json.load(f)

    context = report["context"]
    benchmarks = report["benchmarks"]

    result = "# Benchmark report\n\n"
    for key in args.context_keys:
        result += f"+ {key}: {context[key]}\n"
    result += "\n"

    filters = [re.compile(f.filter) for f in args.filters]

    corr_benchmarks = [defaultdict(list) for _ in args.filters]
    keys = []

    for benchmark in benchmarks:
        if "repetition_index" not in benchmark:
            # aggregate value
            continue
        for i, f in enumerate(filters):
            if f.search(benchmark["name"]) is not None:
                key = f.sub("", benchmark["name"])
                corr_benchmarks[i][key].append(benchmark)
                if key not in keys:
                    keys.append(key)
                break

    keys_presented_in_both = set.union(
        *[set(corr_benchmarks[i].keys()) for i in range(len(args.filters))]
    )
    keys = [k for k in keys if k in keys_presented_in_both]

    rows = []

    # table header
    rows.append(
        ["benchmark_name"]
        + [f"{c.name} [{f.name}]" for c in args.counters for f in args.filters]
    )

    # separator
    rows.append(["-"] * (len(args.counters) * len(args.filters) + 1))

    # rows
    for key in keys:
        row = [key]
        for counter in args.counters:
            row_segment = []
            for i in range(len(args.filters)):
                values = np.array([b[counter.name] for b in corr_benchmarks[i][key]])
                if counter.show_std == "true":
                    mean_val, mean_unit = format_(values.mean(), counter.format_type)
                    std_val, std_unit = format_(values.std(), counter.format_type)
                    row_segment.append(
                        (
                            f"{mean_val} {mean_unit} ± {std_val} {std_unit}",
                            values.mean(),
                        )
                    )
                else:
                    mean_val, mean_unit = format_(values.mean(), counter.format_type)
                    row_segment.append((f"{mean_val} {mean_unit}", values.mean()))

            # find best score and highlight
            max_column_idx = sorted(
                enumerate(row_segment),
                key=lambda x: x[1][1],
                reverse=counter.reversed == "true",
            )[0][0]
            row_segment = [x[0] for x in row_segment]
            row_segment[max_column_idx] = highlight_value(row_segment[max_column_idx])
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
