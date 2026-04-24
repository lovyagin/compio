# Compio Benchmarks

Comprehensive benchmark suite for testing Compio library performance and fragmentation characteristics.

## Available Benchmarks

### 1. **Main Performance Benchmarks** (`compio_benchmarks`)
Google Benchmark-based performance tests for core operations.

**What it tests:**
- Consecutive read/write performance
- Random read/write performance
- Allocator performance across different strategies

**Run:**
```bash
cd build/benchmarks
./compio_benchmarks
```

### 2. **Fragmentation Benchmark** (`fragmentation_benchmark`)
Advanced parametric grid testing for memory fragmentation under realistic workloads.

**What it tests:**
- 4 allocation strategies (FIRST_FIT, BEST_FIT, WORST_FIT, NEXT_FIT)
- 6 block sizes (512B - 16KB)
- 5 compression ratios (10% - 90% compressible data)
- 6 deletion patterns (aggressive small/large/mixed, random/middle)
- **Total: 720 test configurations**

**Key metrics:**
- Overhead percentage (wasted space)
- Fragmentation level (0-100)
- Free region count
- Files created/deleted/rewritten

**Run:**
```bash
cd build/benchmarks
./fragmentation_benchmark                    # default: fragmentation_results.*
./fragmentation_benchmark my_test            # custom: my_test.csv, my_test_report.txt
```

**Output:**
- `fragmentation_results.csv` - detailed data for all 720 tests
- `fragmentation_results_report.txt` - human-readable summary with rankings

**Key findings:**
- **BEST_FIT** shows lowest fragmentation

- **Deletion pattern matters**: Random deletion creates more fragmentation
- **Optimal block size**: 4-8 KB gives best balance

### 3. **File Size Benchmark** (`fsize_benchmark`)
Tests archive size efficiency across different configurations.

**Run:**
```bash
cd build/benchmarks
./fsize_benchmark
```

### 4. **Random Usage Benchmark** (`random_usage`)
Simulates realistic mixed workload patterns.

**Run:**
```bash
cd build/benchmarks
./random_usage
```

### 5. **Locality Comparison Benchmark** (`locality_access_comparison`)
Compares random-access read performance across:
- `compio`
- `zran` (indexed gzip random access)
- `seekable zstd`

Outputs a CSV with per-run metrics vs locality parameter (`n_switch`), then
you can render a performance graph.

**Run:**
```bash
cd build/benchmarks
./locality_access_comparison locality_access_comparison.csv
python3 ../../benchmarks/plot_locality_comparison.py locality_access_comparison.csv locality_access_comparison.png
```

If you want to source `compio` curve from an external Google Benchmark JSON
(instead of the local comparison CSV), pass it as third argument:
```bash
python3 ../../benchmarks/plot_locality_comparison.py \
  locality_access_comparison.csv locality_access_comparison.png ~/.copilot/out_zstd.json
```

### 6. **3-way File Size Comparison** (`size_comparison_3way`)
Prints a small markdown table with on-disk sizes for:
- `compio (zstd)`
- `zran-compatible gzip` (with full flush points every 8 KiB)
- `seekable zstd` (8 KiB frames)

**Run:**
```bash
cd build/benchmarks
./size_comparison_3way                 # default payload: 1.0625 GiB
./size_comparison_3way 16777216        # custom payload size (bytes)
```

## Quick Start

### Build all benchmarks:
```bash
cd build
cmake ..
make compio_benchmarks fragmentation_benchmark fsize_benchmark random_usage
```

### Run fragmentation benchmark:
```bash
cd build/benchmarks
./fragmentation_benchmark
```

### Analyze results:
```bash
cd benchmarks
./analyze_fragmentation.sh
```

Or specify custom CSV file:
```bash
./analyze_fragmentation.sh /path/to/custom_results.csv
```

## Interpreting Results

### Fragmentation Benchmark Results

**Overhead Percentage:**
- `< 20%`: Excellent - minimal wasted space
- `20-40%`: Good - acceptable fragmentation
- `40-60%`: Poor - significant waste
- `> 60%`: Critical - severe fragmentation

**Free Regions Count:**
- Lower is better (more contiguous free space)
- High count indicates scattered small gaps

**Strategy Selection Guide:**
- **BEST_FIT**: Best for minimizing wasted space (recommended for most cases)
- **FIRST_FIT**: Fast, reasonable overhead
- **WORST_FIT**: Useful when future large allocations expected
- **NEXT_FIT**: Generally worst, but can help in cyclic access patterns

## Output Files

All benchmarks output to `build/benchmarks/`:
- `fragmentation_results.csv` - raw data (720 rows)
- `fragmentation_results_report.txt` - formatted analysis
- Performance benchmark results go to stdout (redirect to save)
