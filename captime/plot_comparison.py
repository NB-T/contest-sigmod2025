import csv
import matplotlib.pyplot as plt
import numpy as np

# Load data
with open('comparison.csv') as f:
    reader = csv.DictReader(f)
    rows = list(reader)

# Extract columns
queries = [r['query'] for r in rows]
old_total = np.array([float(r['old_total_ms']) for r in rows])
new_total = np.array([float(r['new_total_ms']) for r in rows])
old_exec = np.array([float(r['old_exec_ms']) for r in rows])
new_exec = np.array([float(r['new_exec_ms']) for r in rows])
old_htbuild = np.array([float(r['old_htbuild_ms']) for r in rows])
new_htbuild = np.array([float(r['new_htbuild_ms']) for r in rows])

# Calculate slowdown
slowdown = new_total / old_total

# Create figure with 2x2 subplots
fig, axes = plt.subplots(2, 2, figsize=(10, 8))
fig.suptitle('Performance Comparison: Old vs New', fontsize=14, fontweight='bold')

# 1. Scatter plot: Old vs New total time (log scale)
ax1 = axes[0, 0]
ax1.scatter(old_total, new_total, alpha=0.6, edgecolors='black', linewidth=0.5)
max_val = max(old_total.max(), new_total.max()) * 1.2
ax1.plot([0.5, max_val], [0.5, max_val], 'k--', alpha=0.5, label='1:1 line')
ax1.set_xlabel('Old Total (ms)')
ax1.set_ylabel('New Total (ms)')
ax1.set_title('Old vs New Execution Time')
ax1.set_xscale('log')
ax1.set_yscale('log')
ax1.legend()
ax1.grid(True, alpha=0.3)

# 2. Slowdown distribution histogram
ax2 = axes[0, 1]
bins = np.linspace(0, 12, 25)
ax2.hist(slowdown, bins=bins, edgecolor='black', alpha=0.7, color='steelblue')
ax2.axvline(slowdown.mean(), color='red', linestyle='--', linewidth=2, label=f'Mean: {slowdown.mean():.2f}x')
ax2.axvline(np.median(slowdown), color='orange', linestyle='--', linewidth=2, label=f'Median: {np.median(slowdown):.2f}x')
ax2.axvline(1, color='green', linestyle='-', linewidth=2, alpha=0.7, label='No change (1x)')
ax2.set_xlabel('Slowdown Factor (new/old)')
ax2.set_ylabel('Number of Queries')
ax2.set_title('Slowdown Distribution')
ax2.legend(fontsize=8)
ax2.grid(True, alpha=0.3)

# 3. Time breakdown comparison (stacked bar)
ax3 = axes[1, 0]
categories = ['Old', 'New']
exec_sums = [old_exec.sum(), new_exec.sum()]
htbuild_sums = [old_htbuild.sum(), new_htbuild.sum()]
other_sums = [old_total.sum() - old_exec.sum() - old_htbuild.sum(),
              new_total.sum() - new_exec.sum() - new_htbuild.sum()]

x = np.arange(len(categories))
width = 0.5

bars1 = ax3.bar(x, other_sums, width, label='Other', color='lightgray')
bars2 = ax3.bar(x, exec_sums, width, bottom=other_sums, label='Exec', color='steelblue')
bars3 = ax3.bar(x, htbuild_sums, width, bottom=[o+e for o,e in zip(other_sums, exec_sums)], label='HT Build', color='coral')

ax3.set_ylabel('Total Time (ms)')
ax3.set_title('Time Breakdown by Component')
ax3.set_xticks(x)
ax3.set_xticklabels(categories)
ax3.legend()
ax3.grid(True, alpha=0.3, axis='y')

# Add total labels on bars
for i, total in enumerate([old_total.sum(), new_total.sum()]):
    ax3.text(i, total + 200, f'{total:.0f}ms', ha='center', fontweight='bold')

# 4. Top 10 slowest queries
ax4 = axes[1, 1]
sorted_idx = np.argsort(slowdown)[::-1][:10]
top_queries = [queries[i] for i in sorted_idx][::-1]
top_slowdown = [slowdown[i] for i in sorted_idx][::-1]
colors = plt.cm.Reds(np.linspace(0.3, 0.8, len(top_queries)))
bars = ax4.barh(top_queries, top_slowdown, color=colors, edgecolor='black', linewidth=0.5)
ax4.axvline(1, color='green', linestyle='-', linewidth=2, alpha=0.7)
ax4.set_xlabel('Slowdown Factor')
ax4.set_title('Top 10 Slowest Queries')
ax4.grid(True, alpha=0.3, axis='x')

# Add value labels
for bar, val in zip(bars, top_slowdown):
    ax4.text(val + 0.1, bar.get_y() + bar.get_height()/2, f'{val:.1f}x', va='center', fontsize=8)

plt.tight_layout()
plt.savefig('comparison_plots.png', dpi=150, bbox_inches='tight')
plt.savefig('comparison_plots.pdf', bbox_inches='tight')
print("Saved: comparison_plots.png and comparison_plots.pdf")

# Print summary stats
print(f"\nSummary Statistics:")
print(f"  Total queries: {len(rows)}")
print(f"  Mean slowdown: {slowdown.mean():.2f}x")
print(f"  Median slowdown: {np.median(slowdown):.2f}x")
print(f"  Queries faster in new: {(slowdown < 1).sum()}")
print(f"  Queries slower in new: {(slowdown > 1).sum()}")
