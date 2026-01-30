#!/usr/bin/env python3
"""
Analyze the breakdown of runtime components for old vs new approaches.
This shows where time is spent in each approach.
"""

import sys
from pathlib import Path
from collections import defaultdict
from parse_timing import load_all_timings


def aggregate_operations(timings: dict, exclude_compile: bool = False) -> dict:
    """Aggregate all operation timings across all queries."""
    totals = defaultdict(float)
    counts = defaultdict(int)

    for query, timing in timings.items():
        for pipeline in timing.pipelines:
            for op, time_ms in pipeline.operations.items():
                if exclude_compile and op == 'pipelineCompile':
                    continue
                totals[op] += time_ms
                counts[op] += 1

    return dict(totals), dict(counts)


def print_operation_breakdown(name: str, totals: dict, counts: dict, total_runtime: float):
    """Print breakdown of operations."""
    print(f"\n{'='*70}")
    print(f"{name} OPERATION BREAKDOWN")
    print(f"{'='*70}")
    print(f"{'Operation':<30} {'Total (ms)':>12} {'Count':>8} {'Avg (ms)':>12}")
    print(f"{'-'*30} {'-'*12} {'-'*8} {'-'*12}")

    # Sort by total time descending
    sorted_ops = sorted(totals.items(), key=lambda x: -x[1])

    for op, total in sorted_ops:
        if total > 0 or counts[op] > 0:
            avg = total / counts[op] if counts[op] > 0 else 0
            print(f"{op:<30} {total:>12.2f} {counts[op]:>8} {avg:>12.2f}")

    total_accounted = sum(totals.values())
    print(f"{'-'*30} {'-'*12} {'-'*8} {'-'*12}")
    print(f"{'Sum of ops':<30} {total_accounted:>12.2f}")
    print(f"{'Reported total runtime':<30} {total_runtime:>12.2f}")


def compare_ht_operations(old_timings: dict, new_timings: dict):
    """Compare hashtable-specific operations between approaches."""
    print(f"\n{'='*70}")
    print("HASHTABLE OPERATION COMPARISON")
    print(f"{'='*70}")

    # Old HT operations: htAlloc, htBuild, htBuildTotal
    # New HT operations: htCollectAndSort, htCountUniqueKeys, htBuildBloomFilter, htBuildTree, htBuildTotal

    old_ht_ops = ['htAlloc', 'htBuild', 'htBuildTotal']
    new_ht_ops = ['htCollectAndSort', 'htCountUniqueKeys', 'htBuildBloomFilter', 'htBuildTree', 'htBuildTotal']

    old_totals = defaultdict(float)
    new_totals = defaultdict(float)
    old_counts = defaultdict(int)
    new_counts = defaultdict(int)

    for timing in old_timings.values():
        for pipeline in timing.pipelines:
            for op, time_ms in pipeline.operations.items():
                if op in old_ht_ops:
                    old_totals[op] += time_ms
                    old_counts[op] += 1

    for timing in new_timings.values():
        for pipeline in timing.pipelines:
            for op, time_ms in pipeline.operations.items():
                if op in new_ht_ops:
                    new_totals[op] += time_ms
                    new_counts[op] += 1

    print("\nOld Approach HT Operations:")
    print(f"{'Operation':<25} {'Total (ms)':>12} {'Count':>8}")
    print(f"{'-'*25} {'-'*12} {'-'*8}")
    for op in old_ht_ops:
        print(f"{op:<25} {old_totals[op]:>12.2f} {old_counts[op]:>8}")

    print("\nNew Approach HT Operations:")
    print(f"{'Operation':<25} {'Total (ms)':>12} {'Count':>8}")
    print(f"{'-'*25} {'-'*12} {'-'*8}")
    for op in new_ht_ops:
        print(f"{op:<25} {new_totals[op]:>12.2f} {new_counts[op]:>8}")

    # Summary
    old_ht_total = old_totals['htBuildTotal']
    new_ht_total = new_totals['htBuildTotal']
    print(f"\nHT Build Total Comparison:")
    print(f"  Old htBuildTotal sum: {old_ht_total:.2f} ms")
    print(f"  New htBuildTotal sum: {new_ht_total:.2f} ms")
    print(f"  Difference: {new_ht_total - old_ht_total:+.2f} ms")


def compare_exec_times(old_timings: dict, new_timings: dict):
    """Compare pipeline execution times."""
    print(f"\n{'='*70}")
    print("PIPELINE EXECUTION TIME COMPARISON")
    print(f"{'='*70}")

    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    old_exec_total = 0.0
    new_exec_total = 0.0
    old_scan_total = 0.0
    new_scan_total = 0.0
    old_optimize_total = 0.0
    new_optimize_total = 0.0

    for query in common_queries:
        for pipeline in old_timings[query].pipelines:
            old_exec_total += pipeline.operations.get('pipelineExec', 0)
            old_scan_total += pipeline.operations.get('scanBuild', 0)
            old_optimize_total += pipeline.operations.get('joinOptimize', 0)

        for pipeline in new_timings[query].pipelines:
            new_exec_total += pipeline.operations.get('pipelineExec', 0)
            new_scan_total += pipeline.operations.get('scanBuild', 0)
            new_optimize_total += pipeline.operations.get('joinOptimize', 0)

    print(f"{'Component':<25} {'Old (ms)':>12} {'New (ms)':>12} {'Diff':>12}")
    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12}")
    print(f"{'pipelineExec':<25} {old_exec_total:>12.2f} {new_exec_total:>12.2f} {new_exec_total - old_exec_total:>+12.2f}")
    print(f"{'scanBuild':<25} {old_scan_total:>12.2f} {new_scan_total:>12.2f} {new_scan_total - old_scan_total:>+12.2f}")
    print(f"{'joinOptimize':<25} {old_optimize_total:>12.2f} {new_optimize_total:>12.2f} {new_optimize_total - old_optimize_total:>+12.2f}")


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <oldtime_dir> <newtime_dir>", file=sys.stderr)
        sys.exit(1)

    oldtime_dir = Path(sys.argv[1])
    newtime_dir = Path(sys.argv[2])

    print("Loading timing data...")
    old_timings = load_all_timings(oldtime_dir)
    new_timings = load_all_timings(newtime_dir)

    print(f"Loaded {len(old_timings)} old timings, {len(new_timings)} new timings")

    # Calculate total runtimes
    old_total_runtime = sum(t.total_runtime_ms for t in old_timings.values())
    new_total_runtime = sum(t.total_runtime_ms for t in new_timings.values())

    print(f"\nOld total runtime: {old_total_runtime:.2f} ms")
    print(f"New total runtime: {new_total_runtime:.2f} ms")

    # Operation breakdown (excluding compile for cleaner view)
    old_totals, old_counts = aggregate_operations(old_timings, exclude_compile=True)
    new_totals, new_counts = aggregate_operations(new_timings, exclude_compile=True)

    print_operation_breakdown("OLD APPROACH (excl. compile)", old_totals, old_counts, old_total_runtime)
    print_operation_breakdown("NEW APPROACH (excl. compile)", new_totals, new_counts, new_total_runtime)

    # HT operations comparison
    compare_ht_operations(old_timings, new_timings)

    # Execution time comparison
    compare_exec_times(old_timings, new_timings)


if __name__ == '__main__':
    main()
