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
    # Build phases (ms)
    scan_build_table: str = ""
    pipeline_compile_ms: float = 0.0
    ht_alloc_ms: float = 0.0
    ht_alloc_tuples: int = 0
    ht_build_ms: float = 0.0
    ht_build_partitions: int = 0
    ht_build_total_ms: float = 0.0
    ht_build_total_tuples: int = 0
    # Exec (ms)
    pipeline_exec_ms: float = 0.0
    exec_scan_table: str = ""
    exec_num_probes: int = 0

    @property
    def has_build(self):
        return self.ht_build_total_tuples > 0 or self.ht_alloc_tuples > 0

    @property
    def has_exec(self):
        return self.pipeline_exec_ms > 0 or self.exec_num_probes > 0


@dataclass
class RunStats:
    """One execution run of a query (queries are repeated multiple times)."""
    run_id: int = 0
    pipelines: list = field(default_factory=list)  # list of PipelineStats


@dataclass
class QueryStats:
    name: str = ""
    total_runtime_ms: float = 0.0
    runs: list = field(default_factory=list)  # list of RunStats


def parse_detail(detail: str, key: str) -> str:
    m = re.search(rf'{key}=(\S+)', detail)
    return m.group(1) if m else ""


def parse_timing_file(filepath: str) -> QueryStats:
    """Parse an oldtime file, splitting pipelines into runs.

    Runs are separated by eliminateSingletons markers. The pattern:
    - Pipeline 0: eliminateSingletons/computeSamples/joinOptimize (optimizer start)
    - Pipelines 1..N-1: scanBuild/htBuild/pipelineExec + joinOptimize
    - Pipeline N: scanBuild/pipelineExec + eliminateSingletons/computeSamples/joinOptimize
      (the eliminateSingletons here starts the NEXT run)
    - Last run's final pipeline has no eliminateSingletons.
    """
    qs = QueryStats()
    entry_re = re.compile(r'\[Pipeline\s+(\d+)\]\s+(\w+)(?:\s+\(([^)]*)\))?:\s*([\d.]+)\s*ms')

    # First pass: read header and collect all lines
    lines = []
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
                m_entry = entry_re.match(line)
                if m_entry:
                    lines.append(m_entry)

    # Second pass: split into runs at eliminateSingletons boundaries
    # Each eliminateSingletons marks the start of a new run's optimizer phase
    run_boundaries = []  # line indices where eliminateSingletons appears
    for i, m in enumerate(lines):
        if m.group(2) == "eliminateSingletons":
            run_boundaries.append(i)

    if not run_boundaries:
        run_boundaries = [0]

    # Build runs: each run goes from one eliminateSingletons to (but not including) the next
    for ri, start_idx in enumerate(run_boundaries):
        if ri + 1 < len(run_boundaries):
            end_idx = run_boundaries[ri + 1]
        else:
            end_idx = len(lines)

        run = RunStats(run_id=ri)
        pipelines_by_id = {}

        for i in range(start_idx, end_idx):
            m = lines[i]
            pid = int(m.group(1))
            phase = m.group(2)
            detail = m.group(3) or ""
            duration = float(m.group(4))

            # Skip optimizer phases (eliminateSingletons, computeSamples, joinOptimize)
            if phase in ("eliminateSingletons", "computeSamples", "joinOptimize"):
                continue

            if pid not in pipelines_by_id:
                ps = PipelineStats(pipeline_id=pid)
                pipelines_by_id[pid] = ps
            ps = pipelines_by_id[pid]

            if phase == "scanBuild":
                ps.scan_build_table = detail.strip()
            elif phase == "pipelineCompile":
                ps.pipeline_compile_ms = duration
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
                parts = detail.split(" probes=")
                if parts:
                    ps.exec_scan_table = parts[0]

        run.pipelines = [pipelines_by_id[pid] for pid in sorted(pipelines_by_id)]
        qs.runs.append(run)

    return qs


def print_query(qs: QueryStats):
    num_runs = len(qs.runs)

    print(f"\n{'='*60}")
    print(f"Query: {qs.name}    Total runtime: {qs.total_runtime_ms:.2f} ms")
    print(f"{'='*60}")

    if num_runs == 0:
        print("  (no pipeline data)")
        return

    last_run = qs.runs[-1]
    print(f"  ({num_runs} run(s), showing last run)")

    total_compile_ms = 0.0
    total_build_ms = 0.0
    total_exec_ms = 0.0

    for ps in last_run.pipelines:
        total_compile_ms += ps.pipeline_compile_ms

        if ps.has_build:
            total_build_ms += ps.ht_build_total_ms
            print(f"\n  Pipeline {ps.pipeline_id} BUILD ({ps.scan_build_table}, {ps.ht_build_total_tuples} tuples):")
            if ps.pipeline_compile_ms > 0:
                print(f"    pipelineCompile:   {ps.pipeline_compile_ms:6.0f} ms")
            print(f"    htAlloc:           {ps.ht_alloc_ms:6.0f} ms  (tuples={ps.ht_alloc_tuples})")
            print(f"    htBuild:           {ps.ht_build_ms:6.0f} ms  (partitions={ps.ht_build_partitions})")
            print(f"    htBuildTotal:      {ps.ht_build_total_ms:6.0f} ms")
            if ps.has_exec:
                total_exec_ms += ps.pipeline_exec_ms
                print(f"    pipelineExec:      {ps.pipeline_exec_ms:6.0f} ms  (probes={ps.exec_num_probes})")
        elif ps.has_exec:
            total_exec_ms += ps.pipeline_exec_ms
            print(f"\n  Pipeline {ps.pipeline_id} EXEC ({ps.exec_scan_table}, probes={ps.exec_num_probes}):")
            if ps.pipeline_compile_ms > 0:
                print(f"    pipelineCompile:   {ps.pipeline_compile_ms:6.0f} ms")
            print(f"    pipelineExec:      {ps.pipeline_exec_ms:6.0f} ms")

    print(f"\n  SUMMARY (last run):")
    if total_compile_ms > 0:
        print(f"    Total compile time:  {total_compile_ms:6.0f} ms")
    print(f"    Total build time:    {total_build_ms:6.0f} ms")
    print(f"    Total exec time:     {total_exec_ms:6.0f} ms")
    print(f"    Total runtime:       {qs.total_runtime_ms:6.2f} ms")


def main():
    if len(sys.argv) < 2:
        timing_dir = "times/oldtime"
    else:
        timing_dir = sys.argv[1]

    if not os.path.isdir(timing_dir):
        print(f"Error: {timing_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    files = sorted(Path(timing_dir).glob("*_timing.txt"))
    if not files:
        print(f"No *_timing.txt files found in {timing_dir}", file=sys.stderr)
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
    grand_build_ms = 0.0
    grand_exec_ms = 0.0
    grand_compile_ms = 0.0
    grand_runtime_ms = 0.0
    grand_tuples = 0

    for qs in all_queries:
        grand_runtime_ms += qs.total_runtime_ms
        if not qs.runs:
            continue
        last_run = qs.runs[-1]
        for ps in last_run.pipelines:
            grand_compile_ms += ps.pipeline_compile_ms
            grand_build_ms += ps.ht_build_total_ms
            grand_tuples += ps.ht_build_total_tuples
            grand_exec_ms += ps.pipeline_exec_ms

    print(f"\n{'='*60}")
    print(f"GRAND SUMMARY ({len(all_queries)} queries, last run of each)")
    print(f"{'='*60}")
    print(f"  Total runtime:           {grand_runtime_ms:8.2f} ms")
    if grand_compile_ms > 0:
        print(f"  Total compile time:      {grand_compile_ms:8.0f} ms")
    print(f"  Total build time:        {grand_build_ms:8.0f} ms  ({grand_tuples} tuples)")
    print(f"  Total exec time:         {grand_exec_ms:8.0f} ms")
    non_accounted = grand_runtime_ms - grand_build_ms - grand_exec_ms - grand_compile_ms
    print(f"  Other/overhead:          {non_accounted:8.2f} ms")


if __name__ == "__main__":
    main()
