#!/usr/bin/env python3

import matplotlib.pyplot as plt
import numpy as np

scenarios = ['Uniform\nSmall', 'Mixed\nSizes', 'Large\nBlocks', 'High\nFrag']

sfl_bestfit_speedup = [4.62, 4.16, 4.45, 4.19]
sfl_firstfit_speedup = [16.66, 29.05, 18.11, 29.83]
sfl_bestfit_speedup_std = [0.22, 0.15, 0.13, 0.18]
sfl_firstfit_speedup_std = [0.85, 1.80, 0.86, 3.09]

sfl_bf_overhead = [1.3, 0.4, 0.2, 0.9]
sfl_ff_overhead = [1.3, 0.4, 0.2, 40.0]
sfl_bf_overhead_std = [0.07, 0.09, 0.05, 0.28]
sfl_ff_overhead_std = [0.15, 0.08, 0.04, 3.5]

sfl_bf_regions = [3, 11, 19, 5]
sfl_ff_regions = [7, 19, 11, 881]
sfl_bf_regions_std = [1, 2, 3, 1]
sfl_ff_regions_std = [2, 3, 2, 85]

fig = plt.figure(figsize=(16, 5))

ax1 = fig.add_subplot(131)
x = np.arange(len(scenarios))
width = 0.35

bars1 = ax1.bar(x - width/2, sfl_bestfit_speedup, width, label='Best-Fit', color='#2E86AB', alpha=0.8,
                yerr=sfl_bestfit_speedup_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5})
bars2 = ax1.bar(x + width/2, sfl_firstfit_speedup, width, label='First-Fit', color='#A23B72', alpha=0.8,
                yerr=sfl_firstfit_speedup_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5})

ax1.set_xlabel('Scenario', fontsize=11, fontweight='bold')
ax1.set_ylabel('Speedup (x)', fontsize=11, fontweight='bold')
ax1.set_title('Allocation Speed\n(higher is better)', fontsize=12, fontweight='bold')
ax1.set_xticks(x)
ax1.set_xticklabels(scenarios, fontsize=9)
ax1.legend(fontsize=10, loc='upper left')
ax1.grid(axis='y', alpha=0.3, linestyle='--')
ax1.set_ylim(0, 36)

for i, bars in enumerate([bars1, bars2]):
    std_values = sfl_bestfit_speedup_std if i == 0 else sfl_firstfit_speedup_std
    for j, bar in enumerate(bars):
        height = bar.get_height()
        err = std_values[j]
        ax1.text(bar.get_x() + bar.get_width()/2., height + err + 0.5,
                f'{height:.1f}x',
                ha='center', va='bottom', fontsize=8, fontweight='bold')

ax2 = fig.add_subplot(132)
bars3 = ax2.bar(x - width/2, sfl_bf_overhead, width, label='Best-Fit', color='#06A77D', alpha=0.8,
                yerr=sfl_bf_overhead_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5})
bars4 = ax2.bar(x + width/2, sfl_ff_overhead, width, label='First-Fit', color='#F18F01', alpha=0.8,
                yerr=sfl_ff_overhead_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5})

ax2.set_xlabel('Scenario', fontsize=11, fontweight='bold')
ax2.set_ylabel('Overhead (%)', fontsize=11, fontweight='bold')
ax2.set_title('Space Overhead\n(lower is better)', fontsize=12, fontweight='bold')
ax2.set_xticks(x)
ax2.set_xticklabels(scenarios, fontsize=9)
ax2.legend(fontsize=10)
ax2.grid(axis='y', alpha=0.3, linestyle='--')
ax2.set_yscale('log')
ax2.set_ylim(0.1, 70)

for idx, (bf, ff, bf_err, ff_err) in enumerate(zip(sfl_bf_overhead, sfl_ff_overhead, sfl_bf_overhead_std, sfl_ff_overhead_std)):
    ax2.text(idx - width/2, bf + bf_err * 1.3, f'{bf:.1f}%', ha='center', va='bottom', fontsize=8, fontweight='bold')
    ax2.text(idx + width/2, ff + ff_err * 1.3, f'{ff:.1f}%', ha='center', va='bottom', fontsize=8, fontweight='bold')

ax3 = fig.add_subplot(133)
bars5 = ax3.bar(x - width/2, sfl_bf_regions, width, label='Best-Fit', color='#06A77D', alpha=0.8,
                yerr=sfl_bf_regions_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5})
bars6 = ax3.bar(x + width/2, sfl_ff_regions, width, label='First-Fit', color='#F18F01', alpha=0.8,
                yerr=sfl_ff_regions_std, error_kw={'elinewidth': 1.5, 'capsize': 3, 'capthick': 1.5})

ax3.set_xlabel('Scenario', fontsize=11, fontweight='bold')
ax3.set_ylabel('Fragment Count', fontsize=11, fontweight='bold')
ax3.set_title('Memory Fragmentation\n(lower is better)', fontsize=12, fontweight='bold')
ax3.set_xticks(x)
ax3.set_xticklabels(scenarios, fontsize=9)
ax3.legend(fontsize=10)
ax3.grid(axis='y', alpha=0.3, linestyle='--')
ax3.set_yscale('log')
ax3.set_ylim(1, 1200)

for idx, (bf, ff, bf_err, ff_err) in enumerate(zip(sfl_bf_regions, sfl_ff_regions, sfl_bf_regions_std, sfl_ff_regions_std)):
    ax3.text(idx - width/2, bf + bf_err * 1.5, str(bf), ha='center', va='bottom', fontsize=8, fontweight='bold')
    ax3.text(idx + width/2, ff + ff_err * 1.5, str(ff), ha='center', va='bottom', fontsize=8, fontweight='bold')

plt.tight_layout()
plt.savefig('strategy_tradeoff.png', dpi=300, bbox_inches='tight')
