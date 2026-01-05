#!/usr/bin/env python3

import matplotlib.pyplot as plt
import numpy as np

scenarios = ['Uniform\nSmall', 'Mixed\nSizes', 'Large\nBlocks', 'High\nFrag']

bestfit_speedup = [4.62, 4.16, 4.45, 4.19]
firstfit_speedup = [16.66, 29.05, 18.11, 29.83]
bestfit_speedup_std = [0.22, 0.15, 0.13, 0.18]
firstfit_speedup_std = [0.85, 1.80, 0.86, 3.09]

current_bf_overhead = [1.3, 0.4, 0.2, 0.7]
sfl_bf_overhead = [1.3, 0.4, 0.2, 0.9]
current_bf_overhead_std = [0.04, 0.10, 0.05, 0.34]
sfl_bf_overhead_std = [0.07, 0.09, 0.05, 0.28]

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

x = np.arange(len(scenarios))
width = 0.35

bars1 = ax1.bar(x - width/2, bestfit_speedup, width, label='Best-Fit', color='#2E86AB', alpha=0.8,
                yerr=bestfit_speedup_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5, 'alpha': 0.7})
bars2 = ax1.bar(x + width/2, firstfit_speedup, width, label='First-Fit', color='#A23B72', alpha=0.8,
                yerr=firstfit_speedup_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5, 'alpha': 0.7})

ax1.set_xlabel('Scenario', fontsize=12, fontweight='bold')
ax1.set_ylabel('Speedup (x)', fontsize=12, fontweight='bold')
ax1.set_title('SFL: Allocation Speedup\n(vs multimap)', fontsize=13, fontweight='bold')
ax1.set_xticks(x)
ax1.set_xticklabels(scenarios)
ax1.legend(fontsize=11)
ax1.grid(axis='y', alpha=0.3, linestyle='--')
ax1.set_ylim(0, 36)

for i, bars in enumerate([bars1, bars2]):
    std_values = bestfit_speedup_std if i == 0 else firstfit_speedup_std
    for j, bar in enumerate(bars):
        height = bar.get_height()
        err = std_values[j]
        ax1.text(bar.get_x() + bar.get_width()/2., height + err + 0.5,
                f'{height:.1f}x',
                ha='center', va='bottom', fontsize=9, fontweight='bold')

bars3 = ax2.bar(x - width/2, current_bf_overhead, width, label='Current (multimap)', color='#F18F01', alpha=0.8,
                yerr=current_bf_overhead_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5, 'alpha': 0.7})
bars4 = ax2.bar(x + width/2, sfl_bf_overhead, width, label='SFL (10 buckets)', color='#06A77D', alpha=0.8,
                yerr=sfl_bf_overhead_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5, 'alpha': 0.7})

ax2.set_xlabel('Scenario', fontsize=12, fontweight='bold')
ax2.set_ylabel('Overhead (%)', fontsize=12, fontweight='bold')
ax2.set_title('Best-Fit: Overhead Comparison\n(lower is better)', fontsize=13, fontweight='bold')
ax2.set_xticks(x)
ax2.set_xticklabels(scenarios)
ax2.legend(fontsize=11)
ax2.grid(axis='y', alpha=0.3, linestyle='--')
ax2.set_ylim(0, 2.5)

for bars in [bars3, bars4]:
    for bar in bars:
        height = bar.get_height()
        bar_idx = list(bars).index(bar)
        if bars == bars3:
            err = current_bf_overhead_std[bar_idx]
        else:
            err = sfl_bf_overhead_std[bar_idx]
        ax2.text(bar.get_x() + bar.get_width()/2., height + err + 0.1,
                f'{height:.1f}%',
                ha='center', va='bottom', fontsize=9, fontweight='bold')

plt.tight_layout()
plt.savefig('sfl_comparison.png', dpi=300, bbox_inches='tight')

fig2, ax3 = plt.subplots(figsize=(8, 5))

dealloc_speedup = [6.57, 6.25, 6.33, 7.10]
dealloc_speedup_std = [0.45, 0.38, 0.42, 0.52]

bars5 = ax3.bar(scenarios, dealloc_speedup, color='#C73E1D', alpha=0.8, width=0.6,
                yerr=dealloc_speedup_std, error_kw={'elinewidth': 2, 'capsize': 4, 'capthick': 2})

ax3.set_xlabel('Scenario', fontsize=12, fontweight='bold')
ax3.set_ylabel('Speedup (x)', fontsize=12, fontweight='bold')
ax3.set_title('SFL: Deallocation Speedup\n(vs multimap)', fontsize=13, fontweight='bold')
ax3.grid(axis='y', alpha=0.3, linestyle='--')
ax3.set_ylim(0, 8)

for bar in bars5:
    height = bar.get_height()
    ax3.text(bar.get_x() + bar.get_width()/2., height,
            f'{height:.1f}x',
            ha='center', va='bottom', fontsize=10, fontweight='bold')

plt.tight_layout()
plt.savefig('sfl_dealloc.png', dpi=300, bbox_inches='tight')
