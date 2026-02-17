#!/usr/bin/env python3
"""Analyze build and probe timing from timing output files."""

import re
import sys
import os
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class PipelineStats:
    pipeline_id: int = 0
    # Build stats (ms)
    collect_and_sort_ms: float = 0.0
    count_unique_keys_ms: float = 0.0
    build_bloom_filter_ms: float = 0.0
    build_tree_ms: float = 0.0
    build_total_ms: float = 0.0
    # Build details
    build_tuples: int = 0
    build_keys: int = 0
    build_bloom_bits: int = 0
    build_tree_height: int = 0
    build_tree_leaves: int = 0
    # Pipeline exec (ms)
    pipeline_exec_ms: float = 0.0
    exec_scan_table: str = ""
    exec_num_probes: int = 0
    # Probe stats (ns from detail strings)
    probe_bloom_ns: int = 0
    probe_bloom_probes: int = 0
    probe_bloom_rejects: int = 0
    probe_tree_ns: int = 0
    probe_tree_count: int = 0
    probe_leaf_ns: int = 0
    probe_leaf_pages: int = 0
    probe_total_ns: int = 0
    probe_total_probes: int = 0

    @property
    def has_build(self):
        return self.build_total_ms > 0 or self.build_tuples > 0

    @property
    def has_probe(self):
        return self.probe_total_probes > 0


@dataclass
class QueryStats:
    name: str = ""
    total_runtime_ms: float = 0.0
    pipelines: dict = field(default_factory=dict)  # pipeline_id -> PipelineStats


def parse_detail(detail: str, key: str) -> str:
    """Extract a value from a detail string like 'total_ns=500 probes=100'."""
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

                if phase == "htCollectAndSort":
                    ps.collect_and_sort_ms = duration
                    v = parse_detail(detail, "tuples")
                    if v: ps.build_tuples = int(v)
                elif phase == "htCountUniqueKeys":
                    ps.count_unique_keys_ms = duration
                    v = parse_detail(detail, "keys")
                    if v: ps.build_keys = int(v)
                elif phase == "htBuildBloomFilter":
                    ps.build_bloom_filter_ms = duration
                    v = parse_detail(detail, "bits")
                    if v: ps.build_bloom_bits = int(v)
                elif phase == "htBuildTree":
                    ps.build_tree_ms = duration
                    v = parse_detail(detail, "height")
                    if v: ps.build_tree_height = int(v)
                    v = parse_detail(detail, "leaves")
                    if v: ps.build_tree_leaves = int(v)
                elif phase == "htBuildTotal":
                    ps.build_total_ms = duration
                    v = parse_detail(detail, "tuples")
                    if v: ps.build_tuples = int(v)
                elif phase == "pipelineExec":
                    ps.pipeline_exec_ms = duration
                    v = parse_detail(detail, "probes")
                    if v: ps.exec_num_probes = int(v)
                    # extract scan table name (everything before " probes=")
                    parts = detail.split(" probes=")
                    if parts:
                        ps.exec_scan_table = parts[0]
                elif phase == "probeBloomFilter":
                    v = parse_detail(detail, "total_ns")
                    if v: ps.probe_bloom_ns = int(v)
                    v = parse_detail(detail, "probes")
                    if v: ps.probe_bloom_probes = int(v)
                    v = parse_detail(detail, "rejects")
                    if v: ps.probe_bloom_rejects = int(v)
                elif phase == "probeTreeTraversal":
                    v = parse_detail(detail, "total_ns")
                    if v: ps.probe_tree_ns = int(v)
                    v = parse_detail(detail, "count")
                    if v: ps.probe_tree_count = int(v)
                elif phase == "probeLeafSearch":
                    v = parse_detail(detail, "total_ns")
                    if v: ps.probe_leaf_ns = int(v)
                    v = parse_detail(detail, "pages")
                    if v: ps.probe_leaf_pages = int(v)
                elif phase == "probeTotal":
                    v = parse_detail(detail, "total_ns")
                    if v: ps.probe_total_ns = int(v)
                    v = parse_detail(detail, "probes")
                    if v: ps.probe_total_probes = int(v)

    return qs


def fmt_ns_as_ms(ns: int) -> str:
    return f"{ns / 1_000_000:.3f}"


def fmt_avg_ns(total_ns: int, count: int) -> str:
    if count == 0:
        return "N/A"
    return f"{total_ns / count:.1f}"


def print_query(qs: QueryStats):
    pids = sorted(qs.pipelines.keys())
    print(f"\n{'='*60}")
    print(f"Query: {qs.name}    Total runtime: {qs.total_runtime_ms:.2f} ms")
    print(f"{'='*60}")

    total_build_ms = 0.0
    total_probe_ns = 0
    total_probes = 0

    for pid in pids:
        ps = qs.pipelines[pid]

        if ps.has_build:
            total_build_ms += ps.build_total_ms
            print(f"\n  Pipeline {pid} BUILD ({ps.build_tuples} tuples, {ps.build_keys} keys):")
            print(f"    collectAndSort:    {ps.collect_and_sort_ms:6.0f} ms")
            print(f"    countUniqueKeys:   {ps.count_unique_keys_ms:6.0f} ms")
            print(f"    buildBloomFilter:  {ps.build_bloom_filter_ms:6.0f} ms  (bits={ps.build_bloom_bits})")
            print(f"    buildTree:         {ps.build_tree_ms:6.0f} ms  (height={ps.build_tree_height}, leaves={ps.build_tree_leaves})")
            print(f"    buildTotal:        {ps.build_total_ms:6.0f} ms")

        if ps.has_probe:
            total_probe_ns += ps.probe_total_ns
            total_probes += ps.probe_total_probes
            reject_pct = (100.0 * ps.probe_bloom_rejects / ps.probe_bloom_probes) if ps.probe_bloom_probes > 0 else 0
            print(f"\n  Pipeline {pid} PROBE (scan={ps.exec_scan_table}, {ps.exec_num_probes} probe table(s)):")
            print(f"    pipelineExec:      {ps.pipeline_exec_ms:6.0f} ms")
            print(f"    bloomFilter:     {fmt_ns_as_ms(ps.probe_bloom_ns):>8s} ms  "
                  f"(probes={ps.probe_bloom_probes}, rejects={ps.probe_bloom_rejects} [{reject_pct:.1f}%], "
                  f"avg={fmt_avg_ns(ps.probe_bloom_ns, ps.probe_bloom_probes)} ns)")
            print(f"    treeTraversal:   {fmt_ns_as_ms(ps.probe_tree_ns):>8s} ms  "
                  f"(traversals={ps.probe_tree_count}, "
                  f"avg={fmt_avg_ns(ps.probe_tree_ns, ps.probe_tree_count)} ns)")
            non_rejected = ps.probe_bloom_probes - ps.probe_bloom_rejects
            print(f"    leafSearch:      {fmt_ns_as_ms(ps.probe_leaf_ns):>8s} ms  "
                  f"(pages={ps.probe_leaf_pages}, "
                  f"avg={fmt_avg_ns(ps.probe_leaf_ns, non_rejected)} ns/search)")
            print(f"    probeTotal:      {fmt_ns_as_ms(ps.probe_total_ns):>8s} ms  "
                  f"(probes={ps.probe_total_probes}, "
                  f"avg={fmt_avg_ns(ps.probe_total_ns, ps.probe_total_probes)} ns)")
        elif ps.pipeline_exec_ms > 0:
            print(f"\n  Pipeline {pid} EXEC (scan={ps.exec_scan_table}, 0 probes):")
            print(f"    pipelineExec:      {ps.pipeline_exec_ms:6.0f} ms")

    print(f"\n  SUMMARY:")
    print(f"    Total build time:  {total_build_ms:6.0f} ms")
    print(f"    Total probe time:  {fmt_ns_as_ms(total_probe_ns):>8s} ms  ({total_probes} probes)")
    print(f"    Total runtime:     {qs.total_runtime_ms:6.2f} ms")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <timing_dir>", file=sys.stderr)
        sys.exit(1)

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
    grand_probe_ns = 0
    grand_probes = 0
    grand_runtime_ms = 0.0
    build_phases = {"collectAndSort": 0.0, "countUniqueKeys": 0.0, "buildBloomFilter": 0.0, "buildTree": 0.0}

    for qs in all_queries:
        grand_runtime_ms += qs.total_runtime_ms
        for ps in qs.pipelines.values():
            grand_build_ms += ps.build_total_ms
            grand_probe_ns += ps.probe_total_ns
            grand_probes += ps.probe_total_probes
            build_phases["collectAndSort"] += ps.collect_and_sort_ms
            build_phases["countUniqueKeys"] += ps.count_unique_keys_ms
            build_phases["buildBloomFilter"] += ps.build_bloom_filter_ms
            build_phases["buildTree"] += ps.build_tree_ms

    print(f"\n{'='*60}")
    print(f"GRAND SUMMARY ({len(all_queries)} queries)")
    print(f"{'='*60}")
    print(f"  Total runtime:           {grand_runtime_ms:8.2f} ms")
    print(f"  Total build time:        {grand_build_ms:8.0f} ms")
    for phase, ms in build_phases.items():
        print(f"    {phase:24s} {ms:8.0f} ms")
    print(f"  Total probe time:        {fmt_ns_as_ms(grand_probe_ns):>8s} ms  ({grand_probes} probes)")
    if grand_probes > 0:
        print(f"    avg per probe:         {grand_probe_ns / grand_probes:8.1f} ns")


if __name__ == "__main__":
    main()
