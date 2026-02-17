#!/usr/bin/env python3
"""Analyze build and probe timing from oldtime format output files."""

import re
import sys
import os
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class PipelineStats:
    pipeline_id: int = 0
    scan_table: str = ""
    # Build stats (ms)
    scan_build_ms: float = 0.0
    ht_alloc_ms: float = 0.0
    ht_alloc_tuples: int = 0
    ht_build_ms: float = 0.0
    ht_build_partitions: int = 0
    ht_build_total_ms: float = 0.0
    ht_build_total_tuples: int = 0
    # Compile
    pipeline_compile_ms: float = 0.0
    pipeline_compile_sig: str = ""
    # Exec
    pipeline_exec_ms: float = 0.0
    exec_num_probes: int = 0
    # Optimization
    join_optimize_ms: float = 0.0
    join_optimize_inputs: int = 0
    eliminate_singletons_ms: float = 0.0
    compute_samples_ms: float = 0.0

    @property
    def has_build(self):
        return self.ht_build_total_ms > 0 or self.ht_alloc_tuples > 0

    @property
    def build_total_ms(self):
        return self.scan_build_ms + self.ht_alloc_ms + self.ht_build_ms + self.ht_build_total_ms

    @property
    def overhead_ms(self):
        return self.join_optimize_ms + self.eliminate_singletons_ms + self.compute_samples_ms


@dataclass
class QueryStats:
    name: str = ""
    total_runtime_ms: float = 0.0
    pipelines: dict = field(default_factory=dict)  # pipeline_id -> PipelineStats


def parse_detail(detail: str, key: str) -> str:
    """Extract a value from a detail string like 'tuples=500 partitions=128'."""
    m = re.search(rf'{key}=(\S+)', detail)
    return m.group(1) if m else ""


def parse_timing_file(filepath: str) -> QueryStats:
    qs = QueryStats()
    entry_re = re.compile(r'\[Pipeline\s+(\d+)\]\s+(\w+)(?:\s+\(([^)]*)\))?:\s*([\d.]+)\s*ms')

    with open(filepath) as f:
        for line in f:
            line = line.rstrip()
            if line.startswith("Query:"):
                qs.name = line.split(":", 1)[1].strip()
            elif line.startswith("Total runtime:"):
                m = re.search(r'([\d.]+)\s*ms', line)
                if m:
                    qs.total_runtime_ms = float(m.group(1))
            else:
                m = entry_re.match(line)
                if not m:
                    continue
                pid = int(m.group(1))
                phase = m.group(2)
                detail = m.group(3) or ""
                duration = float(m.group(4))

                if pid not in qs.pipelines:
                    qs.pipelines[pid] = PipelineStats(pipeline_id=pid)
                ps = qs.pipelines[pid]

                if phase == "scanBuild":
                    ps.scan_build_ms = duration
                    ps.scan_table = detail.strip()
                elif phase == "pipelineCompile":
                    ps.pipeline_compile_ms = duration
                    ps.pipeline_compile_sig = detail.strip()
                elif phase == "htAlloc":
                    ps.ht_alloc_ms = duration
                    v = parse_detail(detail, "tuples")
                    if v: ps.ht_alloc_tuples = int(v)
                elif phase == "htBuild":
                    ps.ht_build_ms = duration
                    v = parse_detail(detail, "partitions")
                    if v: ps.ht_build_partitions = int(v)
                elif phase == "htBuildTotal":
                    ps.ht_build_total_ms = duration
                    v = parse_detail(detail, "tuples")
                    if v: ps.ht_build_total_tuples = int(v)
                elif phase == "pipelineExec":
                    ps.pipeline_exec_ms = duration
                    v = parse_detail(detail, "probes")
                    if v: ps.exec_num_probes = int(v)
                    # extract scan table name
                    parts = detail.split(" probes=")
                    if parts and not ps.scan_table:
                        ps.scan_table = parts[0].strip()
                elif phase == "joinOptimize":
                    ps.join_optimize_ms = duration
                    v = parse_detail(detail, "inputs")
                    if v: ps.join_optimize_inputs = int(v)
                elif phase == "eliminateSingletons":
                    ps.eliminate_singletons_ms = duration
                elif phase == "computeSamples":
                    ps.compute_samples_ms = duration

    return qs


def detect_rounds(pipelines):
    """Detect repeated rounds of pipeline execution.

    Returns list of rounds, each round is a list of pipeline IDs.
    A new round starts when we see eliminateSingletons/computeSamples/joinOptimize
    with no build, or when the scan table pattern repeats.
    """
    pids = sorted(pipelines.keys())
    if not pids:
        return []

    # Find round boundaries: a round starts with an optimization-only pipeline
    # (eliminateSingletons/computeSamples/joinOptimize without build or exec)
    rounds = []
    current_round = []
    for pid in pids:
        ps = pipelines[pid]
        is_opt_only = (ps.eliminate_singletons_ms >= 0 or ps.compute_samples_ms >= 0 or ps.join_optimize_ms >= 0) \
                      and not ps.has_build and ps.pipeline_exec_ms == 0 and ps.pipeline_compile_ms == 0 \
                      and not ps.scan_table
        if is_opt_only and current_round:
            # Check if this starts a new round pattern
            rounds.append(current_round)
            current_round = [pid]
        else:
            current_round.append(pid)
    if current_round:
        rounds.append(current_round)

    return rounds


def print_query(qs: QueryStats):
    pids = sorted(qs.pipelines.keys())
    print(f"\n{'='*70}")
    print(f"Query: {qs.name}    Total runtime: {qs.total_runtime_ms:.2f} ms")
    print(f"{'='*70}")

    rounds = detect_rounds(qs.pipelines)

    for round_idx, round_pids in enumerate(rounds):
        total_compile_ms = 0.0
        total_build_ms = 0.0
        total_exec_ms = 0.0
        total_overhead_ms = 0.0

        print(f"\n  --- Round {round_idx + 1} (pipelines {round_pids[0]}-{round_pids[-1]}) ---")

        for pid in round_pids:
            ps = qs.pipelines[pid]
            total_compile_ms += ps.pipeline_compile_ms
            total_build_ms += ps.build_total_ms
            total_exec_ms += ps.pipeline_exec_ms
            total_overhead_ms += ps.overhead_ms

            parts = []
            if ps.has_build:
                parts.append(f"build {ps.ht_build_total_tuples} tuples, {ps.ht_build_partitions} parts")
            if ps.exec_num_probes > 0:
                parts.append(f"probe {ps.exec_num_probes} HT(s)")
            elif ps.pipeline_exec_ms > 0:
                parts.append("exec only")

            label = f"Pipeline {pid:>2d} [{ps.scan_table or 'opt'}]"
            info = ", ".join(parts) if parts else ""

            # Only print lines with nonzero time or notable info
            if ps.pipeline_compile_ms > 0 or ps.build_total_ms > 0 or ps.pipeline_exec_ms > 0 or ps.overhead_ms > 0:
                time_parts = []
                if ps.pipeline_compile_ms > 0:
                    time_parts.append(f"compile={ps.pipeline_compile_ms:.0f}")
                if ps.build_total_ms > 0:
                    time_parts.append(f"build={ps.build_total_ms:.0f}")
                if ps.pipeline_exec_ms > 0:
                    time_parts.append(f"exec={ps.pipeline_exec_ms:.0f}")
                if ps.overhead_ms > 0:
                    time_parts.append(f"opt={ps.overhead_ms:.0f}")
                print(f"    {label:40s}  {', '.join(time_parts):30s}  {info}")

        print(f"    {'':40s}  compile={total_compile_ms:.0f}  build={total_build_ms:.0f}  exec={total_exec_ms:.0f}  opt={total_overhead_ms:.0f} ms")

    # Totals across all pipelines
    total_compile = sum(ps.pipeline_compile_ms for ps in qs.pipelines.values())
    total_build = sum(ps.build_total_ms for ps in qs.pipelines.values())
    total_exec = sum(ps.pipeline_exec_ms for ps in qs.pipelines.values())
    total_overhead = sum(ps.overhead_ms for ps in qs.pipelines.values())

    print(f"\n  SUMMARY:")
    print(f"    Compilation:   {total_compile:8.0f} ms")
    print(f"    Build (HT):    {total_build:8.0f} ms")
    print(f"    Exec (probe):  {total_exec:8.0f} ms")
    print(f"    Optimization:  {total_overhead:8.0f} ms")
    print(f"    Total runtime: {qs.total_runtime_ms:8.2f} ms")
    if total_compile > 0:
        print(f"    Runtime w/o compile: {qs.total_runtime_ms - total_compile:8.2f} ms (estimated)")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <timing_dir_or_file>", file=sys.stderr)
        sys.exit(1)

    target = sys.argv[1]

    if os.path.isfile(target):
        files = [Path(target)]
    elif os.path.isdir(target):
        files = sorted(Path(target).glob("*_timing.txt"))
        if not files:
            print(f"No *_timing.txt files found in {target}", file=sys.stderr)
            sys.exit(1)
    else:
        print(f"Error: {target} not found", file=sys.stderr)
        sys.exit(1)

    all_queries = []
    for f in files:
        qs = parse_timing_file(str(f))
        if qs.name:
            all_queries.append(qs)

    # Per-query output
    for qs in all_queries:
        print_query(qs)

    # Grand summary
    grand_compile_ms = 0.0
    grand_build_ms = 0.0
    grand_exec_ms = 0.0
    grand_overhead_ms = 0.0
    grand_runtime_ms = 0.0

    # Per-table build stats
    table_build_tuples = {}  # table -> total tuples
    table_build_count = {}   # table -> number of builds

    for qs in all_queries:
        grand_runtime_ms += qs.total_runtime_ms
        for ps in qs.pipelines.values():
            grand_compile_ms += ps.pipeline_compile_ms
            grand_build_ms += ps.build_total_ms
            grand_exec_ms += ps.pipeline_exec_ms
            grand_overhead_ms += ps.overhead_ms
            if ps.has_build and ps.scan_table:
                table_build_tuples[ps.scan_table] = table_build_tuples.get(ps.scan_table, 0) + ps.ht_build_total_tuples
                table_build_count[ps.scan_table] = table_build_count.get(ps.scan_table, 0) + 1

    print(f"\n{'='*70}")
    print(f"GRAND SUMMARY ({len(all_queries)} queries)")
    print(f"{'='*70}")
    print(f"  Total runtime:           {grand_runtime_ms:8.2f} ms")
    print(f"  Total compilation:       {grand_compile_ms:8.0f} ms")
    print(f"  Total build (HT):        {grand_build_ms:8.0f} ms")
    print(f"  Total exec (probe):      {grand_exec_ms:8.0f} ms")
    print(f"  Total optimization:      {grand_overhead_ms:8.0f} ms")
    print(f"  Runtime w/o compile:     {grand_runtime_ms - grand_compile_ms:8.2f} ms (estimated)")

    if table_build_tuples:
        print(f"\n  Build table stats:")
        for table in sorted(table_build_tuples.keys()):
            avg = table_build_tuples[table] / table_build_count[table] if table_build_count[table] > 0 else 0
            print(f"    {table:30s}  builds={table_build_count[table]:>4d}  total_tuples={table_build_tuples[table]:>10d}  avg={avg:>10.0f}")

    # Per-query summary table
    print(f"\n  Per-query breakdown:")
    print(f"    {'Query':<8s}  {'Runtime':>8s}  {'Compile':>8s}  {'Build':>8s}  {'Exec':>8s}  {'Opt':>8s}  {'w/o Compile':>12s}")
    for qs in all_queries:
        compile_ms = sum(ps.pipeline_compile_ms for ps in qs.pipelines.values())
        build_ms = sum(ps.build_total_ms for ps in qs.pipelines.values())
        exec_ms = sum(ps.pipeline_exec_ms for ps in qs.pipelines.values())
        overhead_ms = sum(ps.overhead_ms for ps in qs.pipelines.values())
        print(f"    {qs.name:<8s}  {qs.total_runtime_ms:>8.2f}  {compile_ms:>8.0f}  {build_ms:>8.0f}  {exec_ms:>8.0f}  {overhead_ms:>8.0f}  {qs.total_runtime_ms - compile_ms:>12.2f}")


if __name__ == "__main__":
    main()
