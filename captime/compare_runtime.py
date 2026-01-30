#!/usr/bin/env python3
"""
Compare runtime behavior between old and new approaches.

The 'Total runtime' in timing files is the measured wall-clock time for query
execution (with compiled/cached pipelines). The pipelineCompile entries track
JIT compilation overhead that is typically a one-time cost.

This script compares the actual measured runtimes between approaches.
"""

import sys
from pathlib import Path
from parse_timing import load_all_timings



def format_ms(val: float) -> str:
    """Format milliseconds for display."""
    return f"{val:.2f}"


def format_diff(old_val: float, new_val: float) -> str:
    """Format difference with speedup/slowdown indicator."""
    if old_val == 0 and new_val == 0:
        return "0.00 (=)"
    diff = new_val - old_val
    if old_val > 0:
        pct = (diff / old_val) * 100
        if diff < 0:
            return f"{diff:+.2f} ({abs(pct):.1f}% faster)"
        elif diff > 0:
            return f"{diff:+.2f} ({pct:.1f}% slower)"
        else:
            return f"{diff:+.2f} (=)"
    return f"{diff:+.2f}"


def compare_queries(old_timings: dict, new_timings: dict):
    """Compare individual queries between old and new approaches."""
    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    print(f"\n{'='*80}")
    print("QUERY-BY-QUERY COMPARISON (Total Runtime)")
    print(f"{'='*80}")
    print(f"\n{'Query':<8} {'Old (ms)':>12} {'New (ms)':>12} {'Diff':>25}")
    print(f"{'-'*8} {'-'*12} {'-'*12} {'-'*25}")

    total_old = 0.0
    total_new = 0.0
    faster_count = 0
    slower_count = 0
    same_count = 0

    results = []

    for query in common_queries:
        old_t = old_timings[query]
        new_t = new_timings[query]

        # Compare total runtime directly
        old_runtime = old_t.total_runtime_ms
        new_runtime = new_t.total_runtime_ms

        diff = new_runtime - old_runtime
        results.append((query, old_runtime, new_runtime, diff))

        total_old += old_runtime
        total_new += new_runtime

        if diff < -0.5:
            faster_count += 1
        elif diff > 0.5:
            slower_count += 1
        else:
            same_count += 1

        print(f"{query:<8} {format_ms(old_runtime):>12} {format_ms(new_runtime):>12} {format_diff(old_runtime, new_runtime):>25}")

    print(f"\n{'-'*60}")
    print(f"{'TOTAL':<8} {format_ms(total_old):>12} {format_ms(total_new):>12} {format_diff(total_old, total_new):>25}")

    return results, total_old, total_new, faster_count, slower_count, same_count


def print_summary(total_old: float, total_new: float, faster: int, slower: int, same: int, query_count: int):
    """Print summary statistics."""
    print(f"\n{'='*80}")
    print("SUMMARY")
    print(f"{'='*80}")
    print(f"Total queries compared: {query_count}")
    print(f"New approach faster:    {faster} ({100*faster/query_count:.1f}%)")
    print(f"New approach slower:    {slower} ({100*slower/query_count:.1f}%)")
    print(f"Approximately same:     {same} ({100*same/query_count:.1f}%)")
    print()
    print(f"Total old runtime:      {total_old:.2f} ms")
    print(f"Total new runtime:      {total_new:.2f} ms")
    print(f"Difference:             {total_new - total_old:+.2f} ms")
    if total_old > 0 and total_new > 0:
        speedup = total_old / total_new
        print(f"Speedup factor:         {speedup:.3f}x")
        if speedup > 1:
            print(f"                        -> New is {(speedup-1)*100:.1f}% faster overall")
        else:
            print(f"                        -> New is {(1/speedup-1)*100:.1f}% slower overall")


def print_biggest_differences(results: list, n: int = 10):
    """Print queries with biggest runtime differences."""
    # Sort by absolute difference
    sorted_by_diff = sorted(results, key=lambda x: x[3])  # diff = new - old

    print(f"\n{'='*80}")
    print(f"TOP {n} QUERIES WHERE NEW IS FASTER")
    print(f"{'='*80}")
    print(f"{'Query':<8} {'Old (ms)':>12} {'New (ms)':>12} {'Improvement':>15}")
    print(f"{'-'*8} {'-'*12} {'-'*12} {'-'*15}")
    count = 0
    for query, old, new, diff in sorted_by_diff:
        if diff < 0 and count < n:
            print(f"{query:<8} {old:>12.2f} {new:>12.2f} {-diff:>15.2f}")
            count += 1

    print(f"\n{'='*80}")
    print(f"TOP {n} QUERIES WHERE NEW IS SLOWER")
    print(f"{'='*80}")
    print(f"{'Query':<8} {'Old (ms)':>12} {'New (ms)':>12} {'Regression':>15}")
    print(f"{'-'*8} {'-'*12} {'-'*12} {'-'*15}")
    count = 0
    for query, old, new, diff in reversed(sorted_by_diff):
        if diff > 0 and count < n:
            print(f"{query:<8} {old:>12.2f} {new:>12.2f} {diff:>15.2f}")
            count += 1


def print_compile_time_comparison(old_timings: dict, new_timings: dict):
    """Compare JIT compilation overhead between approaches."""
    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    print(f"\n{'='*80}")
    print("JIT COMPILATION TIME COMPARISON")
    print(f"{'='*80}")
    print("(pipelineCompile entries - typically one-time overhead, not part of hot-path)")

    old_compile_total = sum(old_timings[q].get_total_compile_time() for q in common_queries)
    new_compile_total = sum(new_timings[q].get_total_compile_time() for q in common_queries)

    print(f"\nOld total compile time: {old_compile_total:.2f} ms")
    print(f"New total compile time: {new_compile_total:.2f} ms")
    print(f"Difference:             {new_compile_total - old_compile_total:+.2f} ms")


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

    if not old_timings or not new_timings:
        print("Error: Could not load timing data")
        sys.exit(1)

    # Compare total runtimes
    results, total_old, total_new, faster, slower, same = compare_queries(
        old_timings, new_timings
    )

    print_summary(total_old, total_new, faster, slower, same, len(results))
    print_biggest_differences(results)
    print_compile_time_comparison(old_timings, new_timings)


if __name__ == '__main__':
    main()
