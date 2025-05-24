./benchmarks/compio_benchmarks
--benchmark_repetitions=10
--benchmark_context=COMPRESSION=DISABLED
--benchmark_context=CACHE_SIZE__NODES=128
--benchmark_context=CACHE_SIZE__BLOCKS=16
--benchmark_context=BLOCK_SIZE=4096
--benchmark_context=BTREE_DEGREE=16
--benchmark_out=out.json

python ../tools/compare.py
--report-files out.json
--filters BM_stdio_ BM_compio_
--names stdio compio
--counters
real_time,ms,show_std=true
cpu_time,ms,show_std=true
bytes_per_second,speed,show_std=true,reversed=true
file_size,size
--context-keys COMPRESSION CACHE_SIZE__NODES CACHE_SIZE__BLOCKS BLOCK_SIZE BTREE_DEGREE
-o out.md