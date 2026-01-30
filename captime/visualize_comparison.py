#!/usr/bin/env python3
"""
Visualize the comparison.csv benchmark data with charts and plots.
"""

import csv
import matplotlib.pyplot as plt
from pathlib import Path


SCRIPT_DIR = Path(__file__).parent
CSV_PATH = SCRIPT_DIR / 'comparison.csv'


def load_data():
    """Load and prepare the comparison data."""
    data = {
        'query': [],
        'old_total_ms': [],
        'old_compile_ms': [],
        'old_exec_ms': [],
        'old_htbuild_ms': [],
        'new_total_ms': [],
        'new_compile_ms': [],
        'new_exec_ms': [],
        'new_htbuild_ms': [],
        'diff_ms': [],
        'speedup': [],
    }

    with open(CSV_PATH, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            data['query'].append(row['query'])
            data['old_total_ms'].append(float(row['old_total_ms']))
            data['old_compile_ms'].append(float(row['old_compile_ms']))
            data['old_exec_ms'].append(float(row['old_exec_ms']))
            data['old_htbuild_ms'].append(float(row['old_htbuild_ms']))
            data['new_total_ms'].append(float(row['new_total_ms']))
            data['new_compile_ms'].append(float(row['new_compile_ms']))
            data['new_exec_ms'].append(float(row['new_exec_ms']))
            data['new_htbuild_ms'].append(float(row['new_htbuild_ms']))
            data['diff_ms'].append(float(row['diff_ms']))
            data['speedup'].append(float(row['speedup']))

    # Sort by query name
    indices = sorted(range(len(data['query'])), key=lambda i: data['query'][i])
    for key in data:
        data[key] = [data[key][i] for i in indices]

    return data


def median(values):
    """Compute median of a list."""
    s = sorted(values)
    n = len(s)
    if n % 2 == 1:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2


def plot_runtime_comparison(data, ax):
    """Bar chart comparing old vs new total runtime."""
    n = len(data['query'])
    x = list(range(n))
    width = 0.35

    ax.bar([i - width/2 for i in x], data['old_total_ms'], width, label='Old', color='steelblue', alpha=0.8)
    ax.bar([i + width/2 for i in x], data['new_total_ms'], width, label='New', color='coral', alpha=0.8)

    ax.set_xlabel('Query')
    ax.set_ylabel('Runtime (ms)')
    ax.set_title('Runtime Comparison: Old vs New')
    ax.set_xticks(x[::5])
    ax.set_xticklabels([data['query'][i] for i in x[::5]], rotation=45, ha='right', fontsize=8)
    ax.legend()
    ax.grid(axis='y', alpha=0.3)


def plot_scatter_comparison(data, ax):
    """Scatter plot of old vs new runtimes with diagonal reference."""
    max_val = max(max(data['old_total_ms']), max(data['new_total_ms']))

    # Color by speedup
    colors = ['green' if s > 1 else 'red' for s in data['speedup']]

    ax.scatter(data['old_total_ms'], data['new_total_ms'], c=colors, alpha=0.6, s=50)
    ax.plot([0, max_val], [0, max_val], 'k--', alpha=0.5, label='y=x (equal)')

    ax.set_xlabel('Old Runtime (ms)')
    ax.set_ylabel('New Runtime (ms)')
    ax.set_title('Old vs New Runtime (green=faster, red=slower)')
    ax.set_xlim(0, max_val * 1.05)
    ax.set_ylim(0, max_val * 1.05)
    ax.grid(alpha=0.3)
    ax.set_aspect('equal')


def plot_speedup_distribution(data, ax):
    """Histogram of speedup values."""
    speedups = data['speedup']
    med = median(speedups)

    ax.hist(speedups, bins=30, color='steelblue', alpha=0.7, edgecolor='black')
    ax.axvline(x=1.0, color='red', linestyle='--', linewidth=2, label='No change (1.0x)')
    ax.axvline(x=med, color='green', linestyle='-', linewidth=2, label=f'Median ({med:.2f}x)')

    ax.set_xlabel('Speedup (old/new)')
    ax.set_ylabel('Count')
    ax.set_title('Distribution of Speedup Values')
    ax.legend()
    ax.grid(alpha=0.3)


def plot_top_differences(data, ax):
    """Horizontal bar chart of top improvements and regressions."""
    # Sort by diff_ms
    indices = sorted(range(len(data['diff_ms'])), key=lambda i: data['diff_ms'][i])

    # Get top 10 fastest and slowest
    top_faster_idx = indices[:10]
    top_slower_idx = indices[-10:]
    combined_idx = top_faster_idx + top_slower_idx

    queries = [data['query'][i] for i in combined_idx]
    diffs = [data['diff_ms'][i] for i in combined_idx]
    colors = ['green' if d < 0 else 'red' for d in diffs]

    y_pos = list(range(len(combined_idx)))
    ax.barh(y_pos, diffs, color=colors, alpha=0.7)
    ax.set_yticks(y_pos)
    ax.set_yticklabels(queries, fontsize=8)
    ax.axvline(x=0, color='black', linewidth=0.5)
    ax.set_xlabel('Difference (ms) [negative = faster]')
    ax.set_title('Top 10 Fastest & Slowest Queries (New vs Old)')
    ax.grid(axis='x', alpha=0.3)


def plot_breakdown_comparison(data, ax):
    """Stacked bar showing time breakdown for old vs new."""
    # Select a subset of queries for readability
    subset_idx = list(range(0, len(data['query']), 4))
    n = len(subset_idx)
    x = list(range(n))
    width = 0.35

    old_exec = [data['old_exec_ms'][i] for i in subset_idx]
    old_ht = [data['old_htbuild_ms'][i] for i in subset_idx]
    new_exec = [data['new_exec_ms'][i] for i in subset_idx]
    new_ht = [data['new_htbuild_ms'][i] for i in subset_idx]
    queries = [data['query'][i] for i in subset_idx]

    # Old breakdown
    ax.bar([i - width/2 for i in x], old_exec, width, label='Old Exec', color='steelblue', alpha=0.8)
    ax.bar([i - width/2 for i in x], old_ht, width, bottom=old_exec, label='Old HT Build', color='lightblue', alpha=0.8)

    # New breakdown
    ax.bar([i + width/2 for i in x], new_exec, width, label='New Exec', color='coral', alpha=0.8)
    ax.bar([i + width/2 for i in x], new_ht, width, bottom=new_exec, label='New HT Build', color='lightsalmon', alpha=0.8)

    ax.set_xlabel('Query')
    ax.set_ylabel('Time (ms)')
    ax.set_title('Execution Time Breakdown (subset)')
    ax.set_xticks(x)
    ax.set_xticklabels(queries, rotation=45, ha='right', fontsize=7)
    ax.legend(loc='upper left', fontsize=8)
    ax.grid(axis='y', alpha=0.3)


def plot_summary_stats(data, ax):
    """Text summary of key statistics."""
    ax.axis('off')

    total_old = sum(data['old_total_ms'])
    total_new = sum(data['new_total_ms'])
    overall_speedup = total_old / total_new if total_new > 0 else 0

    faster_count = sum(1 for s in data['speedup'] if s > 1)
    slower_count = sum(1 for s in data['speedup'] if s < 1)
    n_queries = len(data['query'])

    mean_speedup = sum(data['speedup']) / n_queries
    med_speedup = median(data['speedup'])
    max_speedup = max(data['speedup'])
    min_speedup = min(data['speedup'])
    max_idx = data['speedup'].index(max_speedup)
    min_idx = data['speedup'].index(min_speedup)

    stats_text = f"""
    SUMMARY STATISTICS
    ══════════════════════════════════════

    Total Queries:           {n_queries}

    Total Old Runtime:       {total_old:,.1f} ms
    Total New Runtime:       {total_new:,.1f} ms
    Overall Speedup:         {overall_speedup:.3f}x

    Queries Faster (new):    {faster_count} ({100*faster_count/n_queries:.1f}%)
    Queries Slower (new):    {slower_count} ({100*slower_count/n_queries:.1f}%)

    Mean Speedup:            {mean_speedup:.3f}x
    Median Speedup:          {med_speedup:.3f}x

    Max Speedup:             {max_speedup:.3f}x ({data['query'][max_idx]})
    Min Speedup:             {min_speedup:.3f}x ({data['query'][min_idx]})
    """

    ax.text(0.1, 0.5, stats_text, transform=ax.transAxes, fontsize=10,
            verticalalignment='center', fontfamily='monospace',
            bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))


def main():
    print(f"Loading data from {CSV_PATH}...")
    data = load_data()
    print(f"Loaded {len(data['query'])} queries")

    # Create a single-plot figure (runtime comparison only)
    fig, ax = plt.subplots(figsize=(16, 6))
    plot_runtime_comparison(data, ax)
    fig.tight_layout()

    # Save figure
    output_path = SCRIPT_DIR / 'comparison_visualization.png'
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"Saved visualization to {output_path}")

    # Also show if running interactively
    plt.show()


if __name__ == '__main__':
    main()
