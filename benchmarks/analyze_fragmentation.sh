#!/usr/bin/env bash
# Quick analyzer for fragmentation benchmark CSV results
# Usage: ./analyze_fragmentation.sh [csv_file]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_CSV="${SCRIPT_DIR}/../build/benchmarks/fragmentation_results.csv"

CSV_FILE="${1:-$DEFAULT_CSV}"

if [ ! -f "$CSV_FILE" ]; then
    echo "Error: CSV file not found: $CSV_FILE"
    echo ""
    echo "Usage: $0 [csv_file]"
    echo ""
    echo "Default location: build/benchmarks/fragmentation_results.csv"
    echo "Run fragmentation_benchmark first to generate the CSV file:"
    echo "  cd build/benchmarks && ./fragmentation_benchmark"
    exit 1
fi

echo "╔════════════════════════════════════════════════════════════╗"
echo "║       FRAGMENTATION BENCHMARK - QUICK ANALYSIS             ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "Analyzing: $CSV_FILE"
echo ""

# Strategy comparison
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "ALLOCATION STRATEGY COMPARISON"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
awk -F',' 'NR>1 {
    oh[$2]+=$16; cnt[$2]++; time[$2]+=$20
}
END {
    for (s in oh) {
        printf "%-12s  Avg Overhead: %6.2f%%  Avg Time: %6.2f ms  Tests: %d\n",
               s, oh[s]/cnt[s], time[s]/cnt[s], cnt[s]
    }
}' "$CSV_FILE" | sort -t: -k2 -n

echo ""

# Deletion strategy impact
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "DELETION STRATEGY IMPACT"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
awk -F',' 'NR>1 {
    if ($7=="Yes") {
        yes_oh+=$16; yes_del+=$10; yes_frag+=$18; yes_cnt++
    } else {
        no_oh+=$16; no_del+=$10; no_frag+=$18; no_cnt++
    }
}
END {
    printf "Delete from Middle:  Overhead: %6.2f%%  Deleted: %5.1f  Frag Slots: %5.2f  Tests: %d\n",
           yes_oh/yes_cnt, yes_del/yes_cnt, yes_frag/yes_cnt, yes_cnt
    printf "Random Deletion:     Overhead: %6.2f%%  Deleted: %5.1f  Frag Slots: %5.2f  Tests: %d\n",
           no_oh/no_cnt, no_del/no_cnt, no_frag/no_cnt, no_cnt
    printf "\nImpact: Random deletion creates %.1fx more overhead\n", (no_oh/no_cnt)/(yes_oh/yes_cnt)
}' "$CSV_FILE"

echo ""

# Block size impact
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "BLOCK SIZE IMPACT"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
awk -F',' 'NR>1 {
    oh[$1]+=$16; waste[$1]+=$15; cnt[$1]++
}
END {
    for (bs in oh) {
        printf "Block %6s bytes:  Avg Overhead: %6.2f%%  Avg Wasted: %8.2f KB  Tests: %d\n",
               bs, oh[bs]/cnt[bs], waste[bs]/cnt[bs]/1024, cnt[bs]
    }
}' "$CSV_FILE" | sort -t: -k1 -n

echo ""

# Compression impact
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "COMPRESSION PROBABILITY IMPACT"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
awk -F',' 'NR>1 {
    oh[$3]+=$16; cnt[$3]++
}
END {
    for (cp in oh) {
        printf "Compress Prob %3.0f%%:  Avg Overhead: %6.2f%%  Tests: %d\n",
               cp*100, oh[cp]/cnt[cp], cnt[cp]
    }
}' "$CSV_FILE" | sort -t: -k1 -n

echo ""

# Best and worst cases
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "EXTREME CASES"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Find best case (minimum overhead)
echo "Best case (minimum overhead):"
awk -F',' 'NR==1 {for(i=1;i<=NF;i++) h[$i]=i; next}
NR==2 {min=$h["OverheadPercent"]; minline=$0}
NR>2 && $h["OverheadPercent"]<min {min=$h["OverheadPercent"]; minline=$0}
END {
    split(minline, f, ",");
    printf "  Strategy: %s, BlockSize: %s, Overhead: %.2f%%, Deleted: %s files\n",
           f[h["Strategy"]], f[h["BlockSize"]], f[h["OverheadPercent"]], f[h["FilesDeleted"]]
}' "$CSV_FILE"

# Find worst case (maximum overhead)
echo "Worst case (maximum overhead):"
awk -F',' 'NR==1 {for(i=1;i<=NF;i++) h[$i]=i; next}
NR==2 {max=$h["OverheadPercent"]; maxline=$0}
NR>2 && $h["OverheadPercent"]>max {max=$h["OverheadPercent"]; maxline=$0}
END {
    split(maxline, f, ",");
    printf "  Strategy: %s, BlockSize: %s, Overhead: %.2f%%, Deleted: %s files\n",
           f[h["Strategy"]], f[h["BlockSize"]], f[h["OverheadPercent"]], f[h["FilesDeleted"]]
}' "$CSV_FILE"

echo ""

# Summary statistics
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "OVERALL STATISTICS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
awk -F',' 'NR>1 {
    sum_oh+=$16; sum_del+=$10; sum_time+=$20; cnt++
    if($16<20) excellent++
    else if($16<40) good++
    else if($16<60) poor++
    else critical++
}
END {
    printf "Total tests:          %d\n", cnt
    printf "Avg overhead:         %.2f%%\n", sum_oh/cnt
    printf "Avg files deleted:    %.1f\n", sum_del/cnt
    printf "Avg test time:        %.2f ms\n", sum_time/cnt
    printf "\nOverhead distribution:\n"
    printf "  Excellent (<20%%):   %d (%.1f%%)\n", excellent, excellent/cnt*100
    printf "  Good (20-40%%):      %d (%.1f%%)\n", good, good/cnt*100
    printf "  Poor (40-60%%):      %d (%.1f%%)\n", poor, poor/cnt*100
    printf "  Critical (>60%%):    %d (%.1f%%)\n", critical, critical/cnt*100
}' "$CSV_FILE"

echo ""

