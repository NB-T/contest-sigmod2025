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
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <oldtime_dir> <newtime_dir>", file=sys.stderr)
        sys.exit(1)

    oldtime_dir = sys.argv[1]
    newtime_dir = sys.argv[2]

    for script, desc in scripts:
        print(f"\n{'#' * 80}")
        print(f"# {desc}")
        print(f"# Running: {script}")
        print(f"{'#' * 80}\n")

        result = subprocess.run(
            [sys.executable, SCRIPT_DIR / script, oldtime_dir, newtime_dir],
            capture_output=False,
            cwd=SCRIPT_DIR
        )

        if result.returncode != 0:
            print(f"Error running {script}")
            sys.exit(1)

    print(f"\n{'=' * 80}")
    print("All analyses complete!")
    print(f"{'=' * 80}")
    print("\nGenerated files:")
    for f in ['comparison.csv', 'detailed.csv', 'summary_by_group.csv']:
        path = SCRIPT_DIR / f
        if path.exists():
            print(f"  - {f}")


if __name__ == '__main__':
    main()
