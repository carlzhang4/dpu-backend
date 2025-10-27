#!/usr/bin/env python3
import argparse
import csv
from collections import defaultdict

def parse_args():
    p = argparse.ArgumentParser(description="Group by N-th number and average each column.")
    p.add_argument("--input", "-i", required=True, help="Path to input txt file")
    p.add_argument("--output", "-o", required=True, help="Path to output csv file")
    p.add_argument("--column", "-c", type=int, required=True, help="1-based column index to group by")
    return p.parse_args()

def main():
    args = parse_args()
    key_idx = args.column - 1

    sums = {}              # key -> [sum per column]
    counts = defaultdict(int)
    num_cols = None

    with open(args.input, "r", encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if num_cols is None:
                num_cols = len(parts)
                if key_idx < 0 or key_idx >= num_cols:
                    raise ValueError(f"column {args.column} out of range (1..{num_cols})")
            if len(parts) != num_cols:
                raise ValueError(f"Inconsistent column count at line {ln}: got {len(parts)}, expected {num_cols}")

            vals = [float(x) for x in parts]
            key = parts[key_idx]  # use original token to avoid float equality issues

            if key not in sums:
                sums[key] = [0.0] * num_cols
            for i, v in enumerate(vals):
                sums[key][i] += v
            counts[key] += 1

    # Prepare rows: group key + averaged columns
    # Try numeric sort of keys; fallback to lexicographic
    def try_float(x):
        try:
            return float(x)
        except ValueError:
            return None

    keys = list(sums.keys())
    if all(try_float(k) is not None for k in keys):
        keys.sort(key=lambda k: float(k))
    else:
        keys.sort()

    header = [f"group_col_{args.column}"] + [f"col{i+1}" for i in range(num_cols)]
    with open(args.output, "w", newline="", encoding="utf-8") as out:
        w = csv.writer(out)
        w.writerow(header)
        for k in keys:
            c = counts[k]
            avgs = [s / c for s in sums[k]]
            w.writerow([k] + avgs)

if __name__ == "__main__":
    main()

# python3 analyze.py --input select_pim_throughput.txt --output build/agg.csv --column 1