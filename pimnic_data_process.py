#!/usr/bin/env python3
import sys
import re
import csv
from collections import OrderedDict, defaultdict
from typing import List, Dict, Tuple

num_split = re.compile(r'[,\s]+')
dpu_hdr = re.compile(r'^\s*DPU\s+(\d+)\s*$', re.IGNORECASE)

def parse_file(path: str) -> "OrderedDict[int, List[List[float]]]":
    sections: "OrderedDict[int, List[List[float]]]" = OrderedDict()
    cur = None
    with open(path, 'r') as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith('==='):
                continue
            m = dpu_hdr.match(s)
            if m:
                cur = int(m.group(1))
                sections.setdefault(cur, [])
                continue
            if cur is None:
                continue
            parts = [p for p in num_split.split(s) if p]
            if len(parts) < 2:
                # 需要至少 n 和一个数值
                continue
            try:
                n = int(float(parts[0]))
                vals = [float(x) for x in parts[1:]]
            except ValueError:
                continue
            sections[cur].append([float(n)] + vals)
    return sections

def average_by_first(rows: List[List[float]]) -> List[List[float]]:
    # rows: [n, v1, v2, ...]，按 n 分组，对每个位置分别求平均（按存在的行计数）
    sums: Dict[int, List[float]] = {}
    cnts: Dict[int, List[int]] = {}
    for row in rows:
        if len(row) < 2:
            continue
        n = int(row[0])
        vals = row[1:]
        if n not in sums:
            sums[n] = [0.0] * len(vals)
            cnts[n] = [0] * len(vals)
        # 若该行更长，扩展聚合器
        if len(vals) > len(sums[n]):
            extend = len(vals) - len(sums[n])
            sums[n].extend([0.0] * extend)
            cnts[n].extend([0] * extend)
        # 累加到对应位置
        for i, v in enumerate(vals):
            sums[n][i] += v
            cnts[n][i] += 1
    # 生成按 n 排序的结果
    out: List[List[float]] = []
    for n in sorted(sums.keys()):
        avgs = []
        for s, c in zip(sums[n], cnts[n]):
            avgs.append(s / c if c > 0 else 0.0)
        out.append([float(n)] + avgs)
    return out

def write_csv(out_path: str, sections: "OrderedDict[int, List[List[float]]]") -> None:
    with open(out_path, 'w', newline='') as f:
        w = csv.writer(f)
        first = True
        for dpu_id, rows in sections.items():
            if not first:
                w.writerow([])  # 分隔空行
            first = False
            w.writerow([f"DPU {dpu_id}"])
            for row in average_by_first(rows):
                # 第一列 n 按整数输出，后面 6 位小数
                n = int(row[0])
                rest = [f"{v:.6f}" for v in row[1:]]
                w.writerow([n] + rest)

def main():
    in_path = sys.argv[1] if len(sys.argv) > 1 else "kvstore_pim_latency_varidpu.txt"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "kvstore_pim_varidpu.csv"
    sections = parse_file(in_path)
    write_csv(out_path, sections)
    print(f"Written: {out_path}")

if __name__ == "__main__":
    main()