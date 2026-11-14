python ../tools/compare.py \
--report-files allocation_results.json \
--filters "BM_.*FIRST_FIT" "BM_.*BEST_FIT" "BM_.*WORST_FIT" "BM_.*NEXT_FIT" \
--names "First Fit" "Best Fit" "Worst Fit" "Next Fit" \
--counters real_time,ms,show_std=true cpu_time,ms,show_std=true AvgAllocTimeNs,time,show_std=true SuccessRate,percent SpaceEfficiency,percent FileSize,size LargeAllocSuccess,count TotalAllocations,count \
--context-keys ALLOCATION_STRATEGY_TEST \
-o allocation_comparison.md