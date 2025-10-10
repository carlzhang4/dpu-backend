import argparse
import csv
import os

def parse_blocks(lines):
    blocks = []
    cur = None
    for raw in lines:
        s = raw.strip()
        if not s or s.startswith("="):
            continue
        if s.startswith("KEY"):
            parts = s.split()
            # Expect: KEY <k> VALUE <v>
            try:
                k = int(parts[1])
                v = int(parts[3])
            except (IndexError, ValueError):
                continue
            if cur:
                blocks.append(cur)
            cur = {"key": k, "value": v, "rows": []}
        else:
            toks = s.split()
            try:
                nums = [float(t) for t in toks]
            except ValueError:
                continue
            if cur is not None:
                cur["rows"].append(nums)
    if cur:
        blocks.append(cur)
    return blocks

def avg_triplets(rows):
    out = []
    i = 0
    while i < len(rows):
        chunk = rows[i:i+3]
        if not chunk:
            break
        cols = len(chunk[0])
        # 只处理列数一致的行
        if any(len(r) != cols for r in chunk):
            i += 3
            continue
        n = len(chunk)
        sums = [0.0] * cols
        for r in chunk:
            for c in range(cols):
                sums[c] += r[c]
        avg = [s / n for s in sums]
        out.append(avg)
        i += 3
    return out

def write_csv(block, out_dir):
    key = block["key"]
    val = block["value"]
    rows = block["rows"]
    avgs = avg_triplets(rows)

    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"kvstore_latency_KEY{key}_VALUE{val}.csv")
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        # 表头行：KEY XXX VALUE XXX
        w.writerow([f"KEY {key} VALUE {val}"])
        # 可选：写列名
        # w.writerow([f"col{c}" for c in range(len(avgs[0]))] if avgs else [])
        for row in avgs:
            if not row:
                continue
            first = int(round(row[0]))
            rest = ["{:.6f}".format(x) for x in row[1:]]
            w.writerow([first] + rest)
    return out_path

def main():
    ap = argparse.ArgumentParser(description="Process kvstore_pim_latency.txt into CSVs.")
    ap.add_argument("input", help="Path to kvstore_pim_latency.txt")
    ap.add_argument("-o", "--out-dir", default="build/latency_csv", help="Output directory for CSVs")
    args = ap.parse_args()

    with open(args.input, "r") as f:
        lines = f.readlines()

    blocks = parse_blocks(lines)
    if not blocks:
        print("No blocks found.")
        return

    for b in blocks:
        path = write_csv(b, args.out_dir)
        print(f"Wrote {path}")

if __name__ == "__main__":
    main()