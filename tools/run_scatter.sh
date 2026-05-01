#!/bin/bash

# Define algorithms and their compression level ranges
declare -A LEVEL_RANGES
# LEVEL_RANGES["zlib"]="1 2 3 4 5 6 7 8 9"
# LEVEL_RANGES["lz4"]="1 2 3 4 5 6 7 8 9"
# LEVEL_RANGES["zstd"]="-5 -4 -3 -2 -1 0 1 2 3 4 5 6 7 8 9 10"
# LEVEL_RANGES["brotli"]="0 1 2 3 4 5 6 7 8"
LEVEL_RANGES["zstd"]="2 3 4 5 6"

# Config file to be rewritten
CONFIG_FILE="config.conf"

# Benchmark binary and common arguments
BENCHMARK_BIN="benchmarks/compio_benchmarks"
FILTER="BM_compio_ScatterWrite"
OUT_FORMAT="json"

# Check if benchmark binary exists
if [[ ! -x "$BENCHMARK_BIN" ]]; then
    echo "Error: Benchmark binary not found or not executable: $BENCHMARK_BIN"
    exit 1
fi

mkdir -p results42

# Iterate over algorithms
for algo in "${!LEVEL_RANGES[@]}"; do
    levels=(${LEVEL_RANGES[$algo]})
    for level in "${levels[@]}"; do
        # Write config file
        cat > "$CONFIG_FILE" <<EOF
compression=$algo
compression_level=$level
EOF

        # Output JSON file name
        out_file="results42/compio_${algo}_${level}.json"

        echo "Running: $algo with level $level -> $out_file"

        # Run benchmark
        "$BENCHMARK_BIN" \
            --benchmark_filter="$FILTER" \
            --benchmark_out="$out_file" \
            --benchmark_out_format="$OUT_FORMAT" \
            --benchmark_dry_run \
            --compio_config="$CONFIG_FILE"

        # Check exit status
        if [[ $? -eq 0 ]]; then
            echo "Finished successfully: $out_file"
        else
            echo "Error running benchmark for $algo level $level" >&2
        fi
    done
done

echo "All benchmarks completed."
