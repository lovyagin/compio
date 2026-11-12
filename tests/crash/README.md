# Crash consistency checks

Build `crash_consistency` and `index_corruption` with CMake on Unix. These tools
measure crash safety. The separate `crash_consistency_harness` CTest checks
the harness itself and runs a short kill matrix when Python 3.9 or later is
available.

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

## Completed operations with the WAL

With `--wal=1` and a sync mode other than `off`, `--redo=1` requires every
operation that returned before the kill to survive, not only the flushed ones.
It turns automatic batching off, because operations of one batch commit together.

```sh
build_check/tests/crash_consistency --redo=1 --flush-every=0 --trials=50
```

## Unflushed file removal

The `checkpoint-remove` scenario checks that an unflushed deletion preserves the
last saved file. It creates and closes an archive containing a 16 KiB file,
verifies a copy of that checkpoint in a separate process, opens the archive
again, removes the file, then pauses without flushing or closing. The parent
sends SIGKILL after receiving confirmation that removal completed. Recovery must
return the initial file with its original size and content hash. An absent file
is a failure even though removal completed before the crash.

Compare runs with physical release disabled and enabled:

```sh
build_check/tests/crash_consistency --scenario=checkpoint-remove --trials=1 \
    --fill-holes=0 --keep=1
build_check/tests/crash_consistency --scenario=checkpoint-remove --trials=1 \
    --fill-holes=1 --keep=1
```

Both runs must preserve the checkpoint. Storage freed below the published
container end stays unavailable for reuse and physical release until an
allocator snapshot containing those frees and its header are durable. The
harness integration tests require preservation with physical release enabled
and disabled. A failure returns a nonzero exit code and keeps the artifacts.

Run the clean control separately. It must reopen successfully with the file
absent after flushing and closing:

```sh
build_check/tests/crash_consistency --scenario=checkpoint-remove --trials=1 \
    --fill-holes=1 --control=1 --keep=1
```

This scenario uses no compression, disables automatic batching and compaction, and requires
WAL off (the scenario default). It rejects `--ops`, `--mix`, `--flush-every`,
`--init-kb`, `--stop-after`, `--max-delay-ms` and `--sync`: its workload and stop
boundary are fixed. `--seed`, `--trial-start`, `--trials`, `--block` and `--degree`
still configure the initial data or archive. `--fill-holes` maps to
`fill_holes_with_zeros` and is also available in the default `operations` scenario;
when omitted it keeps the library default. It permits hole punching with zeroing
as a fallback, so these runs do not distinguish the two physical release methods.

## File removal interrupted during flush

On Linux with GNU/Clang, `checkpoint-remove-flush` reuses the removal workload
but stops inside `compio_flush`: after the archive data has been synchronized,
just before seeking to the alternate header slot. Link-time wrappers in the
harness executable intercept `fsync` and `fseeko`; the library has no test hooks.
The child reports the stop boundary only after a successful archive `fsync`,
then pauses until the parent sends SIGKILL. If the boundary is not reached or
synchronization has not succeeded, the writer fails.

```sh
build_check/tests/crash_consistency --scenario=checkpoint-remove-flush --trials=1 \
    --fill-holes=0 --keep=1
build_check/tests/crash_consistency --scenario=checkpoint-remove-flush --trials=1 \
    --fill-holes=1 --keep=1
build_check/tests/crash_consistency --scenario=checkpoint-remove-flush --trials=1 \
    --fill-holes=1 --control=1 --keep=1
build_check/tests/crash_consistency --scenario=checkpoint-remove-flush --trials=1 \
    --degree=2 --block=1024 --fill-holes=0 --keep=1
```

Both header slots must remain byte-for-byte identical to the saved checkpoint;
otherwise the writer is classified as failed. Recovery must return the initial
file, as in `checkpoint-remove`. The clean control performs no interception and
requires the removed file to remain absent after flush and close. The same
option restrictions apply. Other platforms reject this scenario before creating
artifacts.

Interrupted runs return the initial file with both physical release settings:
the index is copy-on-write, so the published root and every node below it stay
untouched until the next header is durable. The scenario requires the header
slots and the root to be unchanged. This test covers process termination and
does not simulate power loss.

The manifest adds `header_cut_reached`, `headers_unchanged`, and `root_unchanged`.
The root comparison uses its saved address and serialized node region. A changed
root is diagnostic; recovery of the entire original file remains the criterion.
In a clean control, header/root comparisons describe the completed update and
are not preservation requirements.

## Mixed workload

`mixed_kill` runs a writer that appends stamped records to several files, runs
transactions (a quarter of them aborted), removes and recreates a file, uses batches,
flushes and compacts. The parent sends SIGKILL after a random delay, reopens the archive
and requires every record to be valid and in order, no torn size, both files written by a
transaction to have the same length, and the recovered archive to compact and reopen.

```sh
build_check/tests/mixed_kill <trials> <wal_sync_mode 0|1|2> <tiny_nodes 0|1> <max_delay_ms>
build_check/tests/mixed_kill 1000 0 1 1500
```

`tiny_nodes=1` uses 512-byte blocks and B-tree degree 2, which splits the index often.
A failing trial prints its number and the reason; the exit code is 1. The `mixed_kill_short`
CTest runs 15 trials. SIGKILL keeps the OS page cache, so this does not simulate power loss.

## Artifacts and replay

Each invocation creates a unique `crash-consistency-*` directory under `--dir`
(the system temporary directory by default). Each trial gets its own subdirectory:

- `archive.compio` and, when present, `archive.compio.wal`: untouched after the writer stops.
- `recovery.compio` and its WAL: the copies opened by the verifier.
- `replay.txt`: normalized command, seed, trial index, progress and verification result.

Removal trials also keep `checkpoint.compio`, a copy made before deletion.
Their manifests record the checkpoint size/hash and whether physical release
was enabled. Verification uses a read-only open so checking an absent file cannot
create it. The numeric `verifier_code` is `0` for a complete read, `1` for an open
failure, `2` for a read error, and `3` for a missing file in a removal trial.

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
