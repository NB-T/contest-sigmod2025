#!/usr/bin/env python3
"""Compare average runtimes between two hash join implementations."""

import argparse
import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

def parse_log_file(filepath):
    """Extract all runtime values from a log file."""
    runtimes = []
    pattern = r"Runtime:\s+([\d.]+)\s+ms"

    with open(filepath, 'r') as f:
        for line in f:
            match = re.search(pattern, line)
            if match:
                runtimes.append(float(match.group(1)))
    return runtimes

def get_job_averages(data_dir):
    """Get average runtime for each job in a data directory."""
    averages = {}

    for log_file in Path(data_dir).glob("*.log"):
        job_name = log_file.stem  # e.g., "1a" from "1a.log"
        runtimes = parse_log_file(log_file)
        if runtimes:
            averages[job_name] = sum(runtimes) / len(runtimes)

    return averages

def main():
    parser = argparse.ArgumentParser(description="Compare average runtimes between two implementations")
    parser.add_argument("dir1", help="First data directory")
    parser.add_argument("dir2", help="Second data directory")
    args = parser.parse_args()

    dir1 = Path(args.dir1)
    dir2 = Path(args.dir2)

    averages1 = get_job_averages(dir1)
    averages2 = get_job_averages(dir2)

    # Get all jobs present in both
    common_jobs = set(averages1.keys()) & set(averages2.keys())

    name1 = dir1.name
    name2 = dir2.name

    # Count wins to determine which dataset is faster overall
    wins1 = sum(1 for job in common_jobs if averages1[job] < averages2[job])
    wins2 = len(common_jobs) - wins1

    # Sort by the runtime of whichever dataset wins the majority of comparisons
    if wins1 >= wins2:
        all_jobs = sorted(common_jobs, key=lambda x: averages1[x])
    else:
        all_jobs = sorted(common_jobs, key=lambda x: averages2[x])

    print(f"{'Job':<8} {name1 + ' (ms)':<12} {name2 + ' (ms)':<12} {'Diff (ms)':<12} {'Speedup':<10}")
    print("-" * 54)

    total1 = 0
    total2 = 0

    for job in all_jobs:
        avg1 = averages1[job]
        avg2 = averages2[job]
        diff = avg2 - avg1
        speedup = avg2 / avg1 if avg1 > 0 else float('inf')

        total1 += avg1
        total2 += avg2

        print(f"{job:<8} {avg1:<12.3f} {avg2:<12.3f} {diff:<+12.3f} {speedup:<10.2f}x")

    print("-" * 54)
    print(f"{'TOTAL':<8} {total1:<12.3f} {total2:<12.3f} {total2 - total1:<+12.3f} {total2/total1:.2f}x")
    print()
    print(f"Overall average ({name1}):  {total1 / len(all_jobs):.3f} ms")
    print(f"Overall average ({name2}):  {total2 / len(all_jobs):.3f} ms")
    print(f"{name1} faster: {wins1} jobs | {name2} faster: {wins2} jobs")

    # Plot
    vals1 = [averages1[job] for job in all_jobs]
    vals2 = [averages2[job] for job in all_jobs]

    x = np.arange(len(all_jobs))
    width = 0.35

    fig, ax = plt.subplots(figsize=(16, 7))
    bars1 = ax.bar(x - width/2, vals1, width, label=name1)
    bars2 = ax.bar(x + width/2, vals2, width, label=name2)

    ax.set_xlabel('Job')
    ax.set_ylabel('Average runtime (ms)')
    ax.set_title(f'Runtime Comparison: {name1} vs {name2}')
    ax.set_xticks(x)
    ax.set_xticklabels(all_jobs, rotation=60, ha='right', fontsize=8)
    ax.legend()
    ax.grid(axis='y', alpha=0.3)

    plt.tight_layout()
    plt.subplots_adjust(bottom=0.18)
    output_file = f"compare_{name1}_vs_{name2}.png"
    plt.savefig(output_file, dpi=150)
    print(f"\nPlot saved to: {output_file}")

    # Slowdown ratio chart
    # Determine which dataset is slower overall
    if total1 > total2:
        slow_name, fast_name = name1, name2
        slow_avgs, fast_avgs = averages1, averages2
    else:
        slow_name, fast_name = name2, name1
        slow_avgs, fast_avgs = averages2, averages1

    # Calculate slowdown ratios (slow / fast)
    ratios = {}
    for job in common_jobs:
        ratios[job] = slow_avgs[job] / fast_avgs[job] if fast_avgs[job] > 0 else float('inf')

    # Sort jobs by ratio
    sorted_jobs = sorted(ratios.keys(), key=lambda x: ratios[x])
    sorted_ratios = [ratios[job] for job in sorted_jobs]

    # Determine quantile boundaries
    n_quantiles = 5
    quantile_boundaries = np.percentile(sorted_ratios, np.linspace(0, 100, n_quantiles + 1))

    # Assign jobs to quantiles
    quantile_labels = []
    quantile_jobs = {i: [] for i in range(n_quantiles)}
    for job in sorted_jobs:
        ratio = ratios[job]
        for i in range(n_quantiles):
            if ratio <= quantile_boundaries[i + 1] or i == n_quantiles - 1:
                quantile_jobs[i].append(job)
                quantile_labels.append(i)
                break

    # Print quantile summary
    print(f"\n{'='*60}")
    print(f"Slowdown Ratios: {slow_name} / {fast_name}")
    print(f"{'='*60}")
    for i in range(n_quantiles):
        low = quantile_boundaries[i]
        high = quantile_boundaries[i + 1]
        jobs_in_q = quantile_jobs[i]
        pct_labels = ['0-20%', '20-40%', '40-60%', '60-80%', '80-100%']
        print(f"\nQuantile {i+1} ({pct_labels[i]}): {low:.2f}x - {high:.2f}x slowdown")
        print(f"  Jobs ({len(jobs_in_q)}): {', '.join(jobs_in_q)}")

    # Create the quantile chart
    fig2, ax2 = plt.subplots(figsize=(16, 8))

    # Color map for quantiles
    colors = plt.cm.RdYlGn_r(np.linspace(0.1, 0.9, n_quantiles))

    x2 = np.arange(len(sorted_jobs))
    bar_colors = [colors[q] for q in quantile_labels]

    bars = ax2.bar(x2, sorted_ratios, color=bar_colors, edgecolor='black', linewidth=0.5)

    # Add quantile boundary lines
    for i in range(1, n_quantiles):
        ax2.axhline(y=quantile_boundaries[i], color='gray', linestyle='--', alpha=0.7, linewidth=1)

    ax2.set_xlabel('Job (sorted by slowdown ratio)')
    ax2.set_ylabel(f'Slowdown Ratio ({slow_name} / {fast_name})')
    ax2.set_title(f'Pairwise Slowdown Ratios by Quantile\n({slow_name} is slower overall)')
    ax2.set_xticks(x2)
    ax2.set_xticklabels(sorted_jobs, rotation=60, ha='right', fontsize=7)
    ax2.grid(axis='y', alpha=0.3)

    # Add legend for quantiles
    from matplotlib.patches import Patch
    legend_elements = []
    for i in range(n_quantiles):
        low = quantile_boundaries[i]
        high = quantile_boundaries[i + 1]
        pct_labels = ['0-20%', '20-40%', '40-60%', '60-80%', '80-100%']
        legend_elements.append(Patch(facecolor=colors[i], edgecolor='black',
                                     label=f'Q{i+1} ({pct_labels[i]}): {low:.2f}x-{high:.2f}x'))
    ax2.legend(handles=legend_elements, loc='upper left', fontsize=8)

    # Add horizontal line at ratio=1 for reference
    ax2.axhline(y=1.0, color='blue', linestyle='-', alpha=0.5, linewidth=2, label='No slowdown')

    plt.tight_layout()
    plt.subplots_adjust(bottom=0.18)
    output_file2 = f"slowdown_quantiles_{slow_name}_vs_{fast_name}.png"
    plt.savefig(output_file2, dpi=150)
    print(f"\nSlowdown quantile plot saved to: {output_file2}")

if __name__ == "__main__":
    main()
