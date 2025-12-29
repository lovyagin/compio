#!/usr/bin/env python3
"""
Script to explain why BEST_FIT is preferred over FIRST_FIT despite speed difference
"""

import matplotlib.pyplot as plt
import numpy as np

# Data from benchmark results
scenarios = ['Uniform\nSmall', 'Mixed\nSizes', 'Large\nBlocks', 'High\nFrag']

# SFL implementation comparison
sfl_bestfit_speedup = [4.60, 4.00, 4.47, 4.09]
sfl_firstfit_speedup = [17.82, 27.23, 18.05, 28.43]

# Overhead comparison for SFL (%)
sfl_bf_overhead = [1.2, 0.2, 0.2, 0.6]
sfl_ff_overhead = [1.3, 0.4, 0.2, 40.0]  # Note: High frag case is 40%!

# Free regions (fragmentation indicator)
sfl_bf_regions = [3, 11, 19, 5]
sfl_ff_regions = [7, 19, 11, 881]  # Note: High frag has 881 regions!

# Create figure with three subplots
fig = plt.figure(figsize=(16, 5))

# Plot 1: Speed comparison
ax1 = fig.add_subplot(131)
x = np.arange(len(scenarios))
width = 0.35

bars1 = ax1.bar(x - width/2, sfl_bestfit_speedup, width, label='Best-Fit', color='#2E86AB', alpha=0.8)
bars2 = ax1.bar(x + width/2, sfl_firstfit_speedup, width, label='First-Fit', color='#A23B72', alpha=0.8)

ax1.set_xlabel('Scenario', fontsize=11, fontweight='bold')
ax1.set_ylabel('Speedup (x)', fontsize=11, fontweight='bold')
ax1.set_title('Allocation Speed\n(higher is better)', fontsize=12, fontweight='bold')
ax1.set_xticks(x)
ax1.set_xticklabels(scenarios, fontsize=9)
ax1.legend(fontsize=10, loc='upper left')
ax1.grid(axis='y', alpha=0.3, linestyle='--')
ax1.set_ylim(0, 32)

# Add value labels
for bars in [bars1, bars2]:
    for bar in bars:
        height = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2., height,
                f'{height:.1f}x',
                ha='center', va='bottom', fontsize=8, fontweight='bold')

# Plot 2: Overhead comparison (logarithmic for high frag case)
ax2 = fig.add_subplot(132)
bars3 = ax2.bar(x - width/2, sfl_bf_overhead, width, label='Best-Fit', color='#06A77D', alpha=0.8)
bars4 = ax2.bar(x + width/2, sfl_ff_overhead, width, label='First-Fit', color='#F18F01', alpha=0.8)

ax2.set_xlabel('Scenario', fontsize=11, fontweight='bold')
ax2.set_ylabel('Overhead (%)', fontsize=11, fontweight='bold')
ax2.set_title('Space Overhead\n(lower is better)', fontsize=12, fontweight='bold')
ax2.set_xticks(x)
ax2.set_xticklabels(scenarios, fontsize=9)
ax2.legend(fontsize=10)
ax2.grid(axis='y', alpha=0.3, linestyle='--')
ax2.set_yscale('log')
ax2.set_ylim(0.1, 50)

# Add value labels
for idx, (bf, ff) in enumerate(zip(sfl_bf_overhead, sfl_ff_overhead)):
    ax2.text(idx - width/2, bf, f'{bf:.1f}%', ha='center', va='bottom', fontsize=8, fontweight='bold')
    ax2.text(idx + width/2, ff, f'{ff:.1f}%', ha='center', va='bottom', fontsize=8, fontweight='bold')

# Plot 3: Fragmentation (free regions count)
ax3 = fig.add_subplot(133)
bars5 = ax3.bar(x - width/2, sfl_bf_regions, width, label='Best-Fit', color='#06A77D', alpha=0.8)
bars6 = ax3.bar(x + width/2, sfl_ff_regions, width, label='First-Fit', color='#F18F01', alpha=0.8)

ax3.set_xlabel('Scenario', fontsize=11, fontweight='bold')
ax3.set_ylabel('Fragment Count', fontsize=11, fontweight='bold')
ax3.set_title('Memory Fragmentation\n(lower is better)', fontsize=12, fontweight='bold')
ax3.set_xticks(x)
ax3.set_xticklabels(scenarios, fontsize=9)
ax3.legend(fontsize=10)
ax3.grid(axis='y', alpha=0.3, linestyle='--')
ax3.set_yscale('log')
ax3.set_ylim(1, 1000)

# Add value labels
for idx, (bf, ff) in enumerate(zip(sfl_bf_regions, sfl_ff_regions)):
    ax3.text(idx - width/2, bf, str(bf), ha='center', va='bottom', fontsize=8, fontweight='bold')
    ax3.text(idx + width/2, ff, str(ff), ha='center', va='bottom', fontsize=8, fontweight='bold')

plt.tight_layout()
plt.savefig('strategy_tradeoff.png', dpi=300, bbox_inches='tight')
print("Graph saved: strategy_tradeoff.png")

# Calculate averages (excluding high frag outlier for fair comparison)
avg_bf_speed = np.mean(sfl_bestfit_speedup[:3])
avg_ff_speed = np.mean(sfl_firstfit_speedup[:3])
avg_bf_overhead = np.mean(sfl_bf_overhead[:3])
avg_ff_overhead = np.mean(sfl_ff_overhead[:3])

print("\n=== Key justification for BEST_FIT choice ===")
print(f"1. Speed: FF is {avg_ff_speed/avg_bf_speed:.1f}x faster, but BF is still fast (4-5x improvement)")
print(f"2. Overhead: BF consistently low ({avg_bf_overhead:.2f}%), FF can reach {sfl_ff_overhead[3]:.0f}%")
print(f"3. Fragmentation: In worst case FF creates {sfl_ff_regions[3]} fragments vs {sfl_bf_regions[3]} for BF")
print(f"4. Stability: BF is predictable in all scenarios, FF degrades catastrophically under high fragmentation")
print(f"\nCONCLUSION: Sacrifice 5x speed for 67x space savings and 176x less fragmentation")

