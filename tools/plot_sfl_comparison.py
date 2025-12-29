#!/usr/bin/env python3
"""
Script to visualize Segregated Free List comparison results
"""

import matplotlib.pyplot as plt
import numpy as np

# Data from benchmark results
scenarios = ['Uniform\nSmall', 'Mixed\nSizes', 'Large\nBlocks', 'High\nFrag']

# Speedup data
bestfit_speedup = [4.60, 4.00, 4.47, 4.09]
firstfit_speedup = [17.82, 27.23, 18.05, 28.43]

# Overhead comparison (%)
current_bf_overhead = [1.3, 0.4, 0.4, 2.2]
sfl_bf_overhead = [1.2, 0.2, 0.2, 0.6]

# Create figure with two subplots
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

# Plot 1: Speedup comparison
x = np.arange(len(scenarios))
width = 0.35

bars1 = ax1.bar(x - width/2, bestfit_speedup, width, label='Best-Fit', color='#2E86AB', alpha=0.8)
bars2 = ax1.bar(x + width/2, firstfit_speedup, width, label='First-Fit', color='#A23B72', alpha=0.8)

ax1.set_xlabel('Scenario', fontsize=12, fontweight='bold')
ax1.set_ylabel('Speedup (x)', fontsize=12, fontweight='bold')
ax1.set_title('SFL: Allocation Speedup\n(vs multimap)', fontsize=13, fontweight='bold')
ax1.set_xticks(x)
ax1.set_xticklabels(scenarios)
ax1.legend(fontsize=11)
ax1.grid(axis='y', alpha=0.3, linestyle='--')
ax1.set_ylim(0, 32)

# Add value labels on bars
for bars in [bars1, bars2]:
    for bar in bars:
        height = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2., height,
                f'{height:.1f}x',
                ha='center', va='bottom', fontsize=9, fontweight='bold')

# Plot 2: Overhead comparison
bars3 = ax2.bar(x - width/2, current_bf_overhead, width, label='Current (multimap)', color='#F18F01', alpha=0.8)
bars4 = ax2.bar(x + width/2, sfl_bf_overhead, width, label='SFL (10 buckets)', color='#06A77D', alpha=0.8)

ax2.set_xlabel('Scenario', fontsize=12, fontweight='bold')
ax2.set_ylabel('Overhead (%)', fontsize=12, fontweight='bold')
ax2.set_title('Best-Fit: Overhead Comparison\n(lower is better)', fontsize=13, fontweight='bold')
ax2.set_xticks(x)
ax2.set_xticklabels(scenarios)
ax2.legend(fontsize=11)
ax2.grid(axis='y', alpha=0.3, linestyle='--')

# Add value labels on bars
for bars in [bars3, bars4]:
    for bar in bars:
        height = bar.get_height()
        ax2.text(bar.get_x() + bar.get_width()/2., height,
                f'{height:.1f}%',
                ha='center', va='bottom', fontsize=9, fontweight='bold')

plt.tight_layout()
plt.savefig('sfl_comparison.png', dpi=300, bbox_inches='tight')
print("Graph saved: sfl_comparison.png")

# Create separate plot for deallocation speedup
fig2, ax3 = plt.subplots(figsize=(8, 5))

# Average deallocation speedup is ~6x
dealloc_speedup = [6.57, 6.25, 6.33, 7.10]  # calculated from avg times

bars5 = ax3.bar(scenarios, dealloc_speedup, color='#C73E1D', alpha=0.8, width=0.6)

ax3.set_xlabel('Scenario', fontsize=12, fontweight='bold')
ax3.set_ylabel('Speedup (x)', fontsize=12, fontweight='bold')
ax3.set_title('SFL: Deallocation Speedup\n(vs multimap)', fontsize=13, fontweight='bold')
ax3.grid(axis='y', alpha=0.3, linestyle='--')
ax3.set_ylim(0, 8)

# Add value labels
for bar in bars5:
    height = bar.get_height()
    ax3.text(bar.get_x() + bar.get_width()/2., height,
            f'{height:.1f}x',
            ha='center', va='bottom', fontsize=10, fontweight='bold')

plt.tight_layout()
plt.savefig('sfl_dealloc.png', dpi=300, bbox_inches='tight')
print("Graph saved: sfl_dealloc.png")

print("\n=== Summary ===")
print(f"Average Best-Fit speedup: {np.mean(bestfit_speedup):.1f}x")
print(f"Average First-Fit speedup: {np.mean(firstfit_speedup):.1f}x")
print(f"Average Deallocation speedup: {np.mean(dealloc_speedup):.1f}x")
print(f"Average overhead improvement: {np.mean(np.array(current_bf_overhead) - np.array(sfl_bf_overhead)):.2f}%")


# Create separate plot for deallocation speedup
fig2, ax3 = plt.subplots(figsize=(8, 5))

# Average deallocation speedup is ~6x
dealloc_speedup = [6.57, 6.25, 6.33, 7.10]  # calculated from avg times

bars5 = ax3.bar(scenarios, dealloc_speedup, color='#C73E1D', alpha=0.8, width=0.6)

ax3.set_xlabel('Scenario', fontsize=12, fontweight='bold')
ax3.set_ylabel('Speedup (x)', fontsize=12, fontweight='bold')
ax3.set_title('SFL: Deallocation Speedup\n(vs multimap)', fontsize=13, fontweight='bold')
ax3.grid(axis='y', alpha=0.3, linestyle='--')
ax3.set_ylim(0, 8)

# Add value labels
for bar in bars5:
    height = bar.get_height()
    ax3.text(bar.get_x() + bar.get_width()/2., height,
            f'{height:.1f}x',
            ha='center', va='bottom', fontsize=10, fontweight='bold')

plt.tight_layout()
plt.savefig('sfl_dealloc.png', dpi=300, bbox_inches='tight')
