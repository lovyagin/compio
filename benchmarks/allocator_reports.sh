BENCHMARK_REPETITIONS=10

cmake --build .

./benchmarks/compio_benchmarks \
--benchmark_repetitions=$BENCHMARK_REPETITIONS \
--benchmark_filter=BM_[A-Z] \
--benchmark_out=alloc.json


python3 ../tools/compare.py \
--report-files alloc.json \
--filters "/0(/|$)" "/1(/|$)" "/2(/|$)" "/3(/|$)" \
--names FIRST_FIT BEST_FIT WORST_FIT NEXT_FIT \
--counters \
    real_time,ms,show_std=true \
    cpu_time,ms,show_std=true \
    AvgAllocTimeNs,ns,show_std=true \
    SuccessRate,plain_percent \
    SpaceEfficiency,plain_percent \
    FileSize,size \
--replace-match-with / \
-o alloc_compare.md


python3 ../tools/compare.py \
--report-files alloc.json \
--filters BM_AlternatingSmallLargeAllocations/0 \
          BM_AlternatingSmallLargeAllocations/1 \
          BM_AlternatingSmallLargeAllocations/2 \
          BM_AlternatingSmallLargeAllocations/3 \
--names FIRST_FIT BEST_FIT WORST_FIT NEXT_FIT \
--counters \
    AvgAllocTimeNs,ns,show_std=true \
    SuccessRate,plain_percent \
--replace-match-with BM_AlternatingSmallLargeAllocations \
-o alloc_compare_1.md


python3 ../tools/compare.py \
--report-files alloc.json \
--filters Fragmentation/0 \
          Fragmentation/1 \
          Fragmentation/2 \
          Fragmentation/3 \
--names FIRST_FIT BEST_FIT WORST_FIT NEXT_FIT \
--counters \
    SpaceEfficiency,plain_percent \
    FileSize,size \
--replace-match-with Fragmentation \
-o alloc_compare_2.md