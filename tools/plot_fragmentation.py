#!/usr/bin/env python3
"""
Simplified fragmentation visualization - only key results for presentation
"""

import matplotlib.pyplot as plt
import numpy as np

# Data from fragmentation benchmark results
strategies = ['FIRST_FIT', 'BEST_FIT', 'WORST_FIT', 'NEXT_FIT']
avg_overhead = [43.22, 35.81, 51.59, 53.34]

# Block size data
block_sizes = ['512B', '1KB', '2KB', '4KB', '8KB', '16KB']
block_overhead = [51.76, 54.83, 47.92, 40.76, 39.76, 40.92]

# Create single figure with two key plots
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

# Plot 1: Strategy comparison
x = np.arange(len(strategies))
colors = ['#2E86AB' if s == 'BEST_FIT' else '#F18F01' for s in strategies]

bars1 = ax1.bar(x, avg_overhead, color=colors, alpha=0.8, width=0.6)

ax1.set_xlabel('Allocation Strategy', fontsize=12, fontweight='bold')
ax1.set_ylabel('Average Overhead (%)', fontsize=12, fontweight='bold')
ax1.set_title('Strategy Comparison\n(lower is better)', fontsize=13, fontweight='bold')
ax1.set_xticks(x)
ax1.set_xticklabels(strategies, rotation=0, fontsize=10)
ax1.grid(axis='y', alpha=0.3, linestyle='--')
ax1.set_ylim(0, 60)

# Add value labels
for i, (bar, val) in enumerate(zip(bars1, avg_overhead)):
    ax1.text(bar.get_x() + bar.get_width()/2., val + 1,
             f'{val:.1f}%',
             ha='center', va='bottom', fontsize=10, fontweight='bold')

# Highlight best strategy
best_idx = avg_overhead.index(min(avg_overhead))
ax1.text(best_idx, avg_overhead[best_idx] - 3, 'Optimal',
         ha='center', fontsize=9, fontweight='bold', color='white',
         bbox=dict(boxstyle='round,pad=0.3', facecolor='#06A77D', alpha=0.8))

# Plot 2: Block size impact
x2 = np.arange(len(block_sizes))
colors2 = ['#06A77D' if bs in ['4KB', '8KB'] else '#F18F01' for bs in block_sizes]

bars2 = ax2.bar(x2, block_overhead, color=colors2, alpha=0.8, width=0.6)

ax2.set_xlabel('Block Size', fontsize=12, fontweight='bold')
ax2.set_ylabel('Average Overhead (%)', fontsize=12, fontweight='bold')
ax2.set_title('Block Size Impact\n(optimal: 4-8 KB)', fontsize=13, fontweight='bold')
ax2.set_xticks(x2)
ax2.set_xticklabels(block_sizes, fontsize=10)
ax2.grid(axis='y', alpha=0.3, linestyle='--')
ax2.set_ylim(0, 60)

# Add value labels
for bar, val in zip(bars2, block_overhead):
    ax2.text(bar.get_x() + bar.get_width()/2., val + 1,
             f'{val:.1f}%',
             ha='center', va='bottom', fontsize=10, fontweight='bold')

# Show optimal range with a simple horizontal line
ax2.axhline(y=40, color='#06A77D', linestyle='--', linewidth=2.5, alpha=0.8, label='Optimal level')
ax2.legend(fontsize=9, loc='upper right')

plt.tight_layout()
plt.savefig('fragmentation_simple.png', dpi=300, bbox_inches='tight')
print("Graph saved: fragmentation_simple.png")

print("\n=== Key Results ===")
print(f"Best strategy: BEST_FIT ({min(avg_overhead):.1f}% overhead)")
print(f"Optimal block size: 4-8KB (~{np.mean([block_overhead[3], block_overhead[4]]):.1f}% overhead)")
print(f"BEST_FIT is {((avg_overhead[0] - avg_overhead[1]) / avg_overhead[1] * 100):.1f}% more efficient than FIRST_FIT")
