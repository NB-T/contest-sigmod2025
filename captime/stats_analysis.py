#!/usr/bin/env python3
"""
Statistical analysis of runtime differences between old and new approaches.
"""

import math
import sys
from pathlib import Path
from parse_timing import load_all_timings


def compute_statistics(values: list) -> dict:
    """Compute basic statistics for a list of values."""
    if not values:
        return {'n': 0, 'mean': 0, 'median': 0, 'std': 0, 'min': 0, 'max': 0, 'sum': 0}

    n = len(values)
    mean = sum(values) / n
    sorted_vals = sorted(values)
    median = sorted_vals[n // 2] if n % 2 == 1 else (sorted_vals[n // 2 - 1] + sorted_vals[n // 2]) / 2

    variance = sum((x - mean) ** 2 for x in values) / n if n > 0 else 0
    std = math.sqrt(variance)

    return {
        'n': n,
        'mean': mean,
        'median': median,
        'std': std,
        'min': min(values),
        'max': max(values),
        'sum': sum(values)
    }


def print_stats(name: str, stats: dict):
    """Print statistics in a formatted way."""
    print(f"\n{name}:")
    print(f"  Count:    {stats['n']}")
    print(f"  Sum:      {stats['sum']:.2f} ms")
    print(f"  Mean:     {stats['mean']:.2f} ms")
    print(f"  Median:   {stats['median']:.2f} ms")
    print(f"  Std Dev:  {stats['std']:.2f} ms")
    print(f"  Min:      {stats['min']:.2f} ms")
    print(f"  Max:      {stats['max']:.2f} ms")


def analyze_by_runtime_range(old_timings: dict, new_timings: dict):
    """Analyze results grouped by runtime range."""
    print(f"\n{'='*70}")
    print("ANALYSIS BY RUNTIME RANGE (based on old runtime)")
    print(f"{'='*70}")

    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    # Categorize by old runtime
    ranges = [
        (0, 10, "0-10ms"),
        (10, 25, "10-25ms"),
        (25, 50, "25-50ms"),
        (50, 100, "50-100ms"),
        (100, float('inf'), "100ms+")
    ]

    for low, high, label in ranges:
        queries_in_range = []
        for q in common_queries:
            old_time = old_timings[q].total_runtime_ms
            if low <= old_time < high:
                queries_in_range.append(q)

        if not queries_in_range:
            continue

        old_times = [old_timings[q].total_runtime_ms for q in queries_in_range]
        new_times = [new_timings[q].total_runtime_ms for q in queries_in_range]
        diffs = [new - old for old, new in zip(old_times, new_times)]
        speedups = [old / new if new > 0 else 0 for old, new in zip(old_times, new_times)]

        old_stats = compute_statistics(old_times)
        new_stats = compute_statistics(new_times)
        diff_stats = compute_statistics(diffs)
        speedup_stats = compute_statistics(speedups)

        faster = sum(1 for d in diffs if d < -0.5)
        slower = sum(1 for d in diffs if d > 0.5)

        print(f"\n{label}: {len(queries_in_range)} queries")
        print(f"  Old mean:      {old_stats['mean']:.2f} ms")
        print(f"  New mean:      {new_stats['mean']:.2f} ms")
        print(f"  Mean diff:     {diff_stats['mean']:+.2f} ms")
        print(f"  Mean speedup:  {speedup_stats['mean']:.2f}x")
        print(f"  Faster count:  {faster} ({100*faster/len(queries_in_range):.0f}%)")
        print(f"  Slower count:  {slower} ({100*slower/len(queries_in_range):.0f}%)")


def analyze_compile_time_impact(old_timings: dict, new_timings: dict):
    """Analyze the JIT compilation overhead in both approaches."""
    print(f"\n{'='*70}")
    print("JIT COMPILATION TIME ANALYSIS")
    print(f"{'='*70}")

    old_compile_times = [t.get_total_compile_time() for t in old_timings.values()]
    new_compile_times = [t.get_total_compile_time() for t in new_timings.values()]
    old_total_times = [t.total_runtime_ms for t in old_timings.values()]
    new_total_times = [t.total_runtime_ms for t in new_timings.values()]

    old_compile_stats = compute_statistics(old_compile_times)
    new_compile_stats = compute_statistics(new_compile_times)

    print_stats("Old Approach - Compile Time (per query)", old_compile_stats)
    print_stats("New Approach - Compile Time (per query)", new_compile_stats)

    print(f"\nComparison:")
    print(f"  Old total compile time: {old_compile_stats['sum']:.2f} ms")
    print(f"  New total compile time: {new_compile_stats['sum']:.2f} ms")
    print(f"  Difference: {new_compile_stats['sum'] - old_compile_stats['sum']:+.2f} ms")


def analyze_execution_patterns(old_timings: dict, new_timings: dict):
    """Analyze execution time patterns."""
    print(f"\n{'='*70}")
    print("EXECUTION PATTERN ANALYSIS")
    print(f"{'='*70}")

    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    old_exec_times = []
    new_exec_times = []
    old_htbuild_times = []
    new_htbuild_times = []

    for q in common_queries:
        old_exec_times.append(old_timings[q].get_total_exec_time())
        new_exec_times.append(new_timings[q].get_total_exec_time())
        old_htbuild_times.append(old_timings[q].get_total_ht_build_time())
        new_htbuild_times.append(new_timings[q].get_total_ht_build_time())

    print("\nExecution Time (pipelineExec):")
    old_exec_stats = compute_statistics(old_exec_times)
    new_exec_stats = compute_statistics(new_exec_times)
    print(f"  Old total: {old_exec_stats['sum']:.2f} ms, mean: {old_exec_stats['mean']:.2f} ms")
    print(f"  New total: {new_exec_stats['sum']:.2f} ms, mean: {new_exec_stats['mean']:.2f} ms")
    print(f"  Diff:      {new_exec_stats['sum'] - old_exec_stats['sum']:+.2f} ms")

    print("\nHashtable Build Time (htBuildTotal):")
    old_ht_stats = compute_statistics(old_htbuild_times)
    new_ht_stats = compute_statistics(new_htbuild_times)
    print(f"  Old total: {old_ht_stats['sum']:.2f} ms, mean: {old_ht_stats['mean']:.2f} ms")
    print(f"  New total: {new_ht_stats['sum']:.2f} ms, mean: {new_ht_stats['mean']:.2f} ms")
    print(f"  Diff:      {new_ht_stats['sum'] - old_ht_stats['sum']:+.2f} ms")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <oldtime_dir> <newtime_dir>", file=sys.stderr)
        sys.exit(1)

    oldtime_dir = Path(sys.argv[1])
    newtime_dir = Path(sys.argv[2])

    print("Loading timing data...")
    old_timings = load_all_timings(oldtime_dir)
    new_timings = load_all_timings(newtime_dir)

    print(f"Loaded {len(old_timings)} old timings, {len(new_timings)} new timings")

    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    # Basic statistics
    old_runtimes = [old_timings[q].total_runtime_ms for q in common_queries]
    new_runtimes = [new_timings[q].total_runtime_ms for q in common_queries]
    diffs = [new - old for old, new in zip(old_runtimes, new_runtimes)]
    speedups = [old / new if new > 0 else 0 for old, new in zip(old_runtimes, new_runtimes)]

    print(f"\n{'='*70}")
    print("OVERALL STATISTICS")
    print(f"{'='*70}")

    print_stats("Old Runtimes", compute_statistics(old_runtimes))
    print_stats("New Runtimes", compute_statistics(new_runtimes))
    print_stats("Runtime Differences (new - old)", compute_statistics(diffs))
    print_stats("Speedup Factors (old / new)", compute_statistics(speedups))

    # Additional analyses
    analyze_by_runtime_range(old_timings, new_timings)
    analyze_compile_time_impact(old_timings, new_timings)
    analyze_execution_patterns(old_timings, new_timings)


if __name__ == '__main__':
    main()
