#!/usr/bin/env python3
"""
Export timing comparison data to CSV for further analysis or visualization.
"""

import csv
import re
import sys
from pathlib import Path
from parse_timing import load_all_timings


def export_comparison_csv(old_timings: dict, new_timings: dict, output_file: Path):
    """Export query-by-query comparison to CSV."""
    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    with open(output_file, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow([
            'query',
            'old_total_ms',
            'old_compile_ms',
            'old_exec_ms',
            'old_htbuild_ms',
            'new_total_ms',
            'new_compile_ms',
            'new_exec_ms',
            'new_htbuild_ms',
            'diff_ms',
            'speedup'
        ])

        for query in common_queries:
            old_t = old_timings[query]
            new_t = new_timings[query]

            old_total = old_t.total_runtime_ms
            old_compile = old_t.get_total_compile_time()
            old_exec = old_t.get_total_exec_time()
            old_htbuild = old_t.get_total_ht_build_time()

            new_total = new_t.total_runtime_ms
            new_compile = new_t.get_total_compile_time()
            new_exec = new_t.get_total_exec_time()
            new_htbuild = new_t.get_total_ht_build_time()

            diff = new_total - old_total
            speedup = old_total / new_total if new_total > 0 else 0

            writer.writerow([
                query,
                f"{old_total:.3f}",
                f"{old_compile:.3f}",
                f"{old_exec:.3f}",
                f"{old_htbuild:.3f}",
                f"{new_total:.3f}",
                f"{new_compile:.3f}",
                f"{new_exec:.3f}",
                f"{new_htbuild:.3f}",
                f"{diff:.3f}",
                f"{speedup:.3f}"
            ])

    print(f"Exported comparison to {output_file}")


def export_detailed_csv(old_timings: dict, new_timings: dict, output_file: Path):
    """Export detailed per-pipeline data to CSV."""
    all_old_ops = set()
    all_new_ops = set()

    for timing in old_timings.values():
        for pipeline in timing.pipelines:
            all_old_ops.update(pipeline.operations.keys())

    for timing in new_timings.values():
        for pipeline in timing.pipelines:
            all_new_ops.update(pipeline.operations.keys())

    # Combine and sort operations
    all_ops = sorted(all_old_ops | all_new_ops)

    with open(output_file, 'w', newline='') as f:
        writer = csv.writer(f)
        header = ['query', 'approach', 'pipeline_id'] + all_ops
        writer.writerow(header)

        for query, timing in sorted(old_timings.items()):
            for pipeline in timing.pipelines:
                row = [query, 'old', pipeline.pipeline_id]
                for op in all_ops:
                    row.append(f"{pipeline.operations.get(op, 0):.3f}")
                writer.writerow(row)

        for query, timing in sorted(new_timings.items()):
            for pipeline in timing.pipelines:
                row = [query, 'new', pipeline.pipeline_id]
                for op in all_ops:
                    row.append(f"{pipeline.operations.get(op, 0):.3f}")
                writer.writerow(row)

    print(f"Exported detailed data to {output_file}")


def export_summary_csv(old_timings: dict, new_timings: dict, output_file: Path):
    """Export summary statistics by query group."""
    common_queries = sorted(set(old_timings.keys()) & set(new_timings.keys()))

    # Group queries by number prefix (e.g., 1a, 1b, 1c -> group 1)
    groups = {}
    for query in common_queries:
        # Extract numeric prefix
        match = re.match(r'(\d+)', query)
        if match:
            group = match.group(1)
            if group not in groups:
                groups[group] = []
            groups[group].append(query)

    with open(output_file, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow([
            'query_group',
            'num_variants',
            'old_avg_ms',
            'new_avg_ms',
            'avg_diff_ms',
            'avg_speedup'
        ])

        for group in sorted(groups.keys(), key=int):
            queries = groups[group]
            old_times = [old_timings[q].total_runtime_ms for q in queries]
            new_times = [new_timings[q].total_runtime_ms for q in queries]

            old_avg = sum(old_times) / len(old_times)
            new_avg = sum(new_times) / len(new_times)
            avg_diff = new_avg - old_avg
            avg_speedup = old_avg / new_avg if new_avg > 0 else 0

            writer.writerow([
                group,
                len(queries),
                f"{old_avg:.3f}",
                f"{new_avg:.3f}",
                f"{avg_diff:.3f}",
                f"{avg_speedup:.3f}"
            ])

    print(f"Exported summary to {output_file}")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <oldtime_dir> <newtime_dir>", file=sys.stderr)
        sys.exit(1)

    oldtime_dir = Path(sys.argv[1])
    newtime_dir = Path(sys.argv[2])
    output_dir = Path(__file__).parent

    print("Loading timing data...")
    old_timings = load_all_timings(oldtime_dir)
    new_timings = load_all_timings(newtime_dir)

    print(f"Loaded {len(old_timings)} old timings, {len(new_timings)} new timings")

    export_comparison_csv(old_timings, new_timings, output_dir / 'comparison.csv')
    export_detailed_csv(old_timings, new_timings, output_dir / 'detailed.csv')
    export_summary_csv(old_timings, new_timings, output_dir / 'summary_by_group.csv')

    print("\nDone! Generated CSV files for further analysis.")


if __name__ == '__main__':
    main()
