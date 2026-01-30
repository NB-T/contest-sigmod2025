#!/usr/bin/env python3
"""
Parse timing files from old and new approaches.
"""

import re
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class PipelineTiming:
    pipeline_id: int
    operations: dict = field(default_factory=dict)


@dataclass
class QueryTiming:
    query: str
    total_runtime_ms: float
    pipelines: list = field(default_factory=list)

    def get_total_compile_time(self) -> float:
        """Sum of all pipelineCompile times."""
        total = 0.0
        for p in self.pipelines:
            if 'pipelineCompile' in p.operations:
                total += p.operations['pipelineCompile']
        return total

    def get_total_exec_time(self) -> float:
        """Sum of all pipelineExec times."""
        total = 0.0
        for p in self.pipelines:
            if 'pipelineExec' in p.operations:
                total += p.operations['pipelineExec']
        return total

    def get_total_ht_build_time(self) -> float:
        """Sum of all htBuildTotal times."""
        total = 0.0
        for p in self.pipelines:
            if 'htBuildTotal' in p.operations:
                total += p.operations['htBuildTotal']
        return total

    def get_operation_sum(self, exclude_compile: bool = False) -> float:
        """
        Sum all operation times.

        Note: This may not equal total_runtime_ms due to:
        - Timing resolution (many ops show 0 ms)
        - Caching effects
        - Overhead not captured in individual ops
        """
        total = 0.0
        for p in self.pipelines:
            for op, time_ms in p.operations.items():
                if exclude_compile and op == 'pipelineCompile':
                    continue
                total += time_ms
        return total


def parse_timing_file(filepath: Path) -> Optional[QueryTiming]:
    """Parse a single timing file and return QueryTiming object."""
    try:
        with open(filepath, 'r') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return None

    lines = content.strip().split('\n')

    # Parse query name
    query_match = re.search(r'Query:\s*(\S+)', lines[0])
    if not query_match:
        return None
    query = query_match.group(1)

    # Parse total runtime
    runtime_match = re.search(r'Total runtime:\s*([\d.]+)\s*ms', lines[1])
    if not runtime_match:
        return None
    total_runtime = float(runtime_match.group(1))

    # Parse pipeline operations
    pipelines = {}
    pipeline_pattern = re.compile(r'\[Pipeline\s+(\d+)\]\s+(\w+)(?:\s+\([^)]*\))?:\s*([\d.]+)\s*ms')

    for line in lines:
        match = pipeline_pattern.search(line)
        if match:
            pipeline_id = int(match.group(1))
            operation = match.group(2)
            time_ms = float(match.group(3))

            if pipeline_id not in pipelines:
                pipelines[pipeline_id] = PipelineTiming(pipeline_id=pipeline_id)
            pipelines[pipeline_id].operations[operation] = time_ms

    timing = QueryTiming(
        query=query,
        total_runtime_ms=total_runtime,
        pipelines=list(pipelines.values())
    )

    return timing


def load_all_timings(directory: Path) -> dict:
    """Load all timing files from a directory."""
    timings = {}
    for filepath in sorted(directory.glob('*_timing.txt')):
        timing = parse_timing_file(filepath)
        if timing:
            timings[timing.query] = timing
    return timings


if __name__ == '__main__':
    # Test parsing
    import sys
    if len(sys.argv) > 1:
        timing = parse_timing_file(Path(sys.argv[1]))
        if timing:
            print(f"Query: {timing.query}")
            print(f"Total runtime: {timing.total_runtime_ms} ms")
            print(f"Compile time (sum of pipelineCompile): {timing.get_total_compile_time()} ms")
            print(f"Exec time (sum of pipelineExec): {timing.get_total_exec_time()} ms")
            print(f"HT build time (sum of htBuildTotal): {timing.get_total_ht_build_time()} ms")
            print(f"Operation sum (with compile): {timing.get_operation_sum()} ms")
            print(f"Operation sum (without compile): {timing.get_operation_sum(exclude_compile=True)} ms")
