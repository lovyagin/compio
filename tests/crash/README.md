# Crash consistency checks

Build `crash_consistency` and `index_corruption` with CMake on Unix. These tools
measure known crash-safety gaps; a nonzero result does not necessarily indicate
a regression. The separate `crash_consistency_harness` CTest checks the harness
itself when Python 3.9 or later is available.

## Operation boundaries

Run a workload to completion first, checking its final contents against the model:

```sh
build_check/tests/crash_consistency --control=1 --trials=1 --ops=20 \
    --mix=mixed --init-kb=32 --flush-every=5
```

Then kill the writer after a specific number of operations:

```sh
build_check/tests/crash_consistency --stop-after=12 --trials=1 --ops=20 \
    --mix=mixed --init-kb=32 --flush-every=5 --keep=1
```

The writer pauses after operation 12, including its scheduled flush if any, and
the parent sends SIGKILL. No close or extra flush is performed. Zero stops before
the first operation. The verifier accepts a model prefix between the last
reported flush and the stop boundary; the control requires the final state.
Both size and a 64-bit content hash must match. Each operation's return value is
checked; a failed writer is reported separately from recovery failures.

The default mode still kills at a random time. A fixed seed repeats its workload,
but scheduling changes the interruption point. Boundary mode repeats the sequence
and operation count; it does not interrupt inside a library operation or flush.
SIGKILL preserves the OS page cache. Neither mode simulates power loss.

## Artifacts and replay

Each invocation creates a unique `crash-consistency-*` directory under `--dir`
(the system temporary directory by default). Each trial gets its own subdirectory:

- `archive.compio` and, when present, `archive.compio.wal`: untouched after the writer stops.
- `recovery.compio` and its WAL: the copies opened by the verifier.
- `replay.txt`: normalized command, seed, trial index, progress and verification result.

Failed trials are kept automatically; `--keep=1` also keeps successful trials.
Successful trial directories are otherwise removed. The tool prints the retained
paths. Copy these directories elsewhere if they must outlive temporary-file cleanup.

Execute the command after `replay=` to repeat one trial. `--trial-start=N` selects
the same trial index without running earlier trials. The replay uses the same
binary path and parameters; keep the tested build or record its commit separately.
For timed trials, it repeats only the workload and requested delay.

Writers have a 30-second deadline in boundary and control modes. Verifiers have
a 10-second deadline and reject sizes larger than the generated model permits.
Exit codes: `0` all consistent, `1` a writer or consistency failure, `2` invalid
arguments or a harness setup error.
