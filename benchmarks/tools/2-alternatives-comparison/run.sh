#!/bin/bash

BENCHMARK_BIN="$1"
RESULTS_DIR="$2"

if [[ -z "$BENCHMARK_BIN" || -z "$RESULTS_DIR" ]]; then
    echo "Usage: $0 <benchmark_binary> <results_directory>"
    exit 1
fi

if [[ ! -x "$BENCHMARK_BIN" ]]; then
    echo "Error: Benchmark binary not found or not executable: $BENCHMARK_BIN"
    exit 1
fi

mkdir -p "$RESULTS_DIR"

CONFIG_FILE="config.conf"

# --- compio with zstd level 1 ---
cat > "$CONFIG_FILE" <<EOF
compression=zstd
compression_level=1
EOF
echo "Running: compio zstd level 1 -> $RESULTS_DIR/compio_zstd_1.json"
"$BENCHMARK_BIN" \
    --benchmark_filter="compio_OptimalUsage/0" \
    --benchmark_out="$RESULTS_DIR/compio_zstd_1.json" \
    --benchmark_min_time=15s \
    --benchmark_report_aggregates_only \
    --compio_config="$CONFIG_FILE"
if [[ $? -eq 0 ]]; then
    echo "Finished successfully: $RESULTS_DIR/compio_zstd_1.json"
else
    echo "Error running compio zstd level 1" >&2
fi

# --- compio with zlib level 1 ---
cat > "$CONFIG_FILE" <<EOF
compression=zlib
compression_level=1
EOF
echo "Running: compio zlib level 1 -> $RESULTS_DIR/compio_zlib_1.json"
"$BENCHMARK_BIN" \
    --benchmark_filter="compio_OptimalUsage/0" \
    --benchmark_out="$RESULTS_DIR/compio_zlib_1.json" \
    --benchmark_min_time=30s \
    --benchmark_report_aggregates_only \
    --compio_config="$CONFIG_FILE"
if [[ $? -eq 0 ]]; then
    echo "Finished successfully: $RESULTS_DIR/compio_zlib_1.json"
else
    echo "Error running compio zlib level 1" >&2
fi

# --- zran (gzip + deflate_index) ---
echo "Running: zran -> $RESULTS_DIR/zran.json"
"$BENCHMARK_BIN" \
    --benchmark_filter="BM_zran_OptimalUsage" \
    --benchmark_out="$RESULTS_DIR/zran.json" \
    --benchmark_min_time=30s \
    --benchmark_report_aggregates_only \
    --compio_config="$CONFIG_FILE"
if [[ $? -eq 0 ]]; then
    echo "Finished successfully: $RESULTS_DIR/zran.json"
else
    echo "Error running zran" >&2
fi

# --- seekable zstd ---
echo "Running: seekable zstd -> $RESULTS_DIR/seekable_zstd.json"
"$BENCHMARK_BIN" \
    --benchmark_filter="BM_seekable_zstd_OptimalUsage" \
    --benchmark_out="$RESULTS_DIR/seekable_zstd.json" \
    --benchmark_min_time=30s \
    --benchmark_report_aggregates_only \
    --compio_config="$CONFIG_FILE"
if [[ $? -eq 0 ]]; then
    echo "Finished successfully: $RESULTS_DIR/seekable_zstd.json"
else
    echo "Error running seekable zstd" >&2
fi

rm -f "$CONFIG_FILE"

echo "All benchmarks completed."
