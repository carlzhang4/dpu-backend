#!/usr/bin/env python3
import argparse
import csv
import os
from collections import defaultdict

def parse_args():
    p = argparse.ArgumentParser(description="Aggregate latency logs by (DPU_NUM, feature_dim) and output a single CSV.")
    p.add_argument("--input", "-i", default="build/GNN_RDMA_pim_latency_PID.txt", help="Input txt path")
    p.add_argument("--output", "-o", default="build/GNN_RDMA_pim_latency_PID_agg.csv", help="Output csv path")
    return p.parse_args()

def main():
    args = parse_args()
    if not os.path.isfile(args.input):
        raise FileNotFoundError(f"Input file not found: {args.input}")

    sums = {}  # (dpu, feat) -> [sum metrics...]
    counts = defaultdict(int)
    max_metrics = 0

    with open(args.input, "r", encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            try:
                vals = [float(x) for x in parts]
            except ValueError:
                # skip malformed lines
                continue
            if len(vals) < 3:
                continue
            dpu = int(vals[0])
            feat = int(vals[1])
            metrics = vals[2:]
            key = (dpu, feat)
            if key not in sums:
                sums[key] = [0.0] * len(metrics)
            elif len(metrics) != len(sums[key]):
                raise ValueError(f"Inconsistent metric count at line {ln}: got {len(metrics)}, expected {len(sums[key])}")
            for i, v in enumerate(metrics):
                sums[key][i] += v
            counts[key] += 1
            if len(metrics) > max_metrics:
                max_metrics = len(metrics)

    # Build averages grouped by DPU_NUM
    grouped = defaultdict(dict)  # dpu -> { feat: [avg metrics] }
    for (dpu, feat), sumv in sums.items():
        c = counts[(dpu, feat)]
        grouped[dpu][feat] = [v / c for v in sumv]

    # Prepare header
    header = ["DPU_NUM", "feature_dim"] + [f"metric_{i+1}" for i in range(max_metrics)]
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    with open(args.output, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(header)
        for dpu in sorted(grouped.keys()):
            for feat in sorted(grouped[dpu].keys()):
                metrics = grouped[dpu][feat]
                # pad if some groups had fewer metrics (shouldn't happen if input consistent)
                if len(metrics) < max_metrics:
                    metrics = metrics + ["" for _ in range(max_metrics - len(metrics))]
                w.writerow([dpu, feat] + metrics)

    print(f"Wrote aggregated CSV: {args.output}")

if __name__ == "__main__":
    main()