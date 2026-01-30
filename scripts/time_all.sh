#!/usr/bin/env bash
# Run timing analysis for all jobs (3 repetitions, median selected)
# Usage: ./scripts/time_all.sh <plans.json> [output_dir]

PLANS_PATH="${1:-}"
OUTPUT_DIR="${2:-timing_output}"
NUM_REPS=3

if [ -z "$PLANS_PATH" ]; then
    echo "Usage: $0 <plans.json> [output_dir]"
    echo ""
    echo "Arguments:"
    echo "  plans.json   Path to the plans JSON file (required)"
    echo "  output_dir   Directory to store timing output (default: timing_output)"
    exit 1
fi

jobs=("1a" "1b" "1c" "1d" "2a" "2b" "2c" "2d" "3a" "3b" "3c" "4a" "4b" "4c" "5a" "5b" "5c" "6a" "6b" "6c" "6d" "6e" "6f" "7a" "7b" "7c" "8a" "8b" "8c" "8d" "9a" "9b" "9c" "9d" "10a" "10b" "10c" "11a" "11b" "11c" "11d" "12a" "12b" "12c" "13a" "13b" "13c" "13d" "14a" "14b" "14c" "15a" "15b" "15c" "15d" "16a" "16b" "16c" "16d" "17a" "17b" "17c" "17d" "17e" "17f" "18a" "18b" "18c" "19a" "19b" "19c" "19d" "20a" "20b" "20c" "21a" "21b" "21c" "22a" "22b" "22c" "22d" "23a" "23b" "23c" "24a" "24b" "25a" "25b" "25c" "26a" "26b" "26c" "27a" "27b" "27c" "28a" "28b" "28c" "29a" "29b" "29c" "30a" "30b" "30c" "31a" "31b" "31c" "32a" "32b" "33a" "33b" "33c")

failed_jobs=()
succeeded_jobs=()

echo "============================================"
echo "Running timing for all ${#jobs[@]} jobs"
echo "Plans: $PLANS_PATH"
echo "Output: $OUTPUT_DIR"
echo "Repetitions per query: $NUM_REPS (median selected)"
echo "============================================"

mkdir -p "$OUTPUT_DIR"

# Extract "Total runtime: XX.XX ms" value from a timing file
extract_runtime() {
    local file="$1"
    grep -oP 'Total runtime: \K[0-9.]+' "$file" 2>/dev/null || echo "-1"
}

for job in "${jobs[@]}"; do
    echo ""
    echo ">>> Timing job: $job ($NUM_REPS repetitions)"

    runtimes=()
    rep_dirs=()
    all_ok=true

    for ((rep=1; rep<=NUM_REPS; rep++)); do
        rep_dir="${OUTPUT_DIR}/.rep_${job}_${rep}"
        rep_dirs+=("$rep_dir")
        mkdir -p "$rep_dir"

        echo "    Run $rep/$NUM_REPS..."
        if ! ./scripts/run_timing.sh "$PLANS_PATH" "$job" "$rep_dir" > /dev/null 2>&1; then
            echo "    !!! Run $rep failed"
            all_ok=false
            break
        fi

        timing_file="$rep_dir/${job}_timing.txt"
        if [ -f "$timing_file" ]; then
            rt=$(extract_runtime "$timing_file")
            runtimes+=("$rt")
            echo "    Run $rep: ${rt} ms"
        else
            echo "    !!! No timing file produced for run $rep"
            all_ok=false
            break
        fi
    done

    if [ "$all_ok" = true ] && [ ${#runtimes[@]} -eq $NUM_REPS ]; then
        # Sort runtimes and pick the median (index 1 of 3 sorted values)
        sorted=($(printf '%s\n' "${runtimes[@]}" | sort -g))
        median_idx=$(( NUM_REPS / 2 ))
        median_val="${sorted[$median_idx]}"

        # Find which rep produced the median value and copy its output
        for ((rep=0; rep<NUM_REPS; rep++)); do
            if [ "${runtimes[$rep]}" = "$median_val" ]; then
                cp "${rep_dirs[$rep]}/${job}_timing.txt" "$OUTPUT_DIR/${job}_timing.txt"
                echo "    Median: ${median_val} ms (run $((rep+1))) [${sorted[0]}, ${sorted[1]}, ${sorted[2]}]"
                break
            fi
        done

        succeeded_jobs+=("$job")
    else
        echo "!!! Job $job failed"
        failed_jobs+=("$job")
    fi

    # Clean up repetition directories
    for rep_dir in "${rep_dirs[@]}"; do
        rm -rf "$rep_dir"
    done
done

echo ""
echo "============================================"
echo "Summary"
echo "============================================"
echo "Succeeded: ${#succeeded_jobs[@]} jobs"
echo "Failed:    ${#failed_jobs[@]} jobs"
if [ ${#failed_jobs[@]} -gt 0 ]; then
    echo "Failed jobs: ${failed_jobs[*]}"
fi
echo "Results in: $OUTPUT_DIR"
echo "============================================"
