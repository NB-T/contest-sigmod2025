#!/usr/bin/env python3
"""
Run all comparison and analysis scripts.
"""

import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent

scripts = [
    ('compare_runtime.py', 'Comparing runtime between old and new approaches'),
    ('stats_analysis.py', 'Statistical analysis'),
    ('analyze_breakdown.py', 'Operation breakdown analysis'),
    ('export_csv.py', 'Exporting CSV files'),
]


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <oldtime_dir> <newtime_dir> <output_dir>", file=sys.stderr)
        sys.exit(1)

    oldtime_dir = str(Path(sys.argv[1]).resolve())
    newtime_dir = str(Path(sys.argv[2]).resolve())
    output_dir = Path(sys.argv[3]).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    for script, desc in scripts:
        print(f"\n{'#' * 80}")
        print(f"# {desc}")
        print(f"# Running: {script}")
        print(f"{'#' * 80}\n")

        result = subprocess.run(
            [sys.executable, SCRIPT_DIR / script, oldtime_dir, newtime_dir, str(output_dir)],
            capture_output=False,
            cwd=SCRIPT_DIR
        )

        if result.returncode != 0:
            print(f"Error running {script}")
            sys.exit(1)

    print(f"\n{'=' * 80}")
    print("All analyses complete!")
    print(f"{'=' * 80}")
    print(f"\nGenerated files in {output_dir}:")
    for f in ['comparison.csv', 'detailed.csv', 'summary_by_group.csv']:
        path = output_dir / f
        if path.exists():
            print(f"  - {path}")


if __name__ == '__main__':
    main()
