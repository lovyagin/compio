#!/bin/bash

benchmarks/blocks_benchmark 5000 30000000 5000 0 16 50000 >blocks.csv
python ../tools/plot_blocks.py