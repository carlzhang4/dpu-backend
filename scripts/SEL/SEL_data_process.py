#!/usr/bin/env python3
import sys
import csv

def parse_line(line: str):
    parts = line.strip().split()
    if not parts:
        return None
    try:
        nums = [float(x) for x in parts]
    except ValueError:
        return None
    return nums

def aggregate_by_first(path: str):
    # key -> (sums:list[float], counts:list[int])
    agg = {}
    with open(path, 'r') as f:
        for line in f:
            row = parse_line(line)
            if not row or len(row) < 2:
                continue
            key = int(row[0])
            vals = row[1:]
            sums, cnts = agg.get(key, ([], []))
            if len(sums) < len(vals):
                extend = len(vals) - len(sums)
                sums.extend([0.0] * extend)
                cnts.extend([0] * extend)
            for i, v in enumerate(vals):
                sums[i] += v
                cnts[i] += 1
            agg[key] = (sums, cnts)
    # produce averaged rows
    result = []
    for key in sorted(agg.keys()):
        sums, cnts = agg[key]
        avgs = [(s / c if c else 0.0) for s, c in zip(sums, cnts)]
        result.append([key] + avgs)
    return result

def write_csv(out_path: str, rows):
    # Header: size, avg_1, avg_2, ...
    max_cols = max((len(r) for r in rows), default=1) - 1
    header = ["size"] + [f"avg_{i+1}" for i in range(max_cols)]
    with open(out_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(header)
        for r in rows:
            key = int(r[0])
            vals = [f"{v:.6f}" for v in r[1:]]
            w.writerow([key] + vals)

def main():
    in_path = sys.argv[1] if len(sys.argv) > 1 else "log/sel_cpu_latency.txt"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "sel_cpu_latency_agg.csv"
    rows = aggregate_by_first(in_path)
    write_csv(out_path, rows)
    print(f"Wrote {out_path}")

if __name__ == "__main__":
    main()