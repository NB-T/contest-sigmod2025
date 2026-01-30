#!/usr/bin/env python3
import csv
import sys

# Configuration
INPUT_FILE = "detailed.csv"
OUTPUT_FILE = "detailed_sorted.csv"
SORT_COLUMN = "pipelineExec"  # Change this to sort by a different column
ASCENDING = False  # Set to True for ascending order

def main():
    sort_col = sys.argv[1] if len(sys.argv) > 1 else SORT_COLUMN

    with open(INPUT_FILE, 'r') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        columns = reader.fieldnames

    if sort_col not in columns:
        print(f"Error: Column '{sort_col}' not found.")
        print(f"Available columns: {', '.join(columns)}")
        sys.exit(1)

    rows.sort(key=lambda r: float(r[sort_col]), reverse=not ASCENDING)

    with open(OUTPUT_FILE, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    print(f"Sorted by '{sort_col}' ({'ascending' if ASCENDING else 'descending'})")
    print(f"Output written to {OUTPUT_FILE}")

if __name__ == "__main__":
    main()
