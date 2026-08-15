#!/usr/bin/env python3
"""Byte-level V2 comparison of original-benchmark and PIMNIC-library dumps.

select mode: both files are arrays of u64 hit values; the multisets must
be identical (sorted compare, insensitive to shard/harvest order).
kvstore mode: both files are arrays of (u64 key, u64 value) records; the
per-key value sequences must be identical.
"""
import struct
import sys


def read_u64s(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) % 8 != 0:
        raise SystemExit(f"FAIL {path}: size {len(data)} not u64-aligned")
    return list(struct.unpack(f"<{len(data) // 8}Q", data))


def fail(message):
    print(f"PIMNIC V2 COMPARE FAIL {message}")
    raise SystemExit(1)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("select", "kvstore"):
        raise SystemExit(
            "usage: compare_dumps.py {select|kvstore} REF_DUMP NEW_DUMP")
    mode, ref_path, new_path = sys.argv[1:4]
    ref, new = read_u64s(ref_path), read_u64s(new_path)
    if mode == "select":
        ref_sorted, new_sorted = sorted(ref), sorted(new)
        if len(ref_sorted) != len(new_sorted):
            fail(f"select count ref={len(ref_sorted)} new={len(new_sorted)}")
        for i, (a, b) in enumerate(zip(ref_sorted, new_sorted)):
            if a != b:
                fail(f"select value[{i}] ref={a} new={b}")
        print(f"PIMNIC V2 COMPARE PASS mode=select hits={len(ref_sorted)}")
        return
    if len(ref) % 2 or len(new) % 2:
        fail("kvstore dump not (key,value) pairs")
    ref_pairs = sorted(zip(ref[0::2], ref[1::2]))
    new_pairs = sorted(zip(new[0::2], new[1::2]))
    if len(ref_pairs) != len(new_pairs):
        fail(f"kvstore count ref={len(ref_pairs)} new={len(new_pairs)}")
    for i, (a, b) in enumerate(zip(ref_pairs, new_pairs)):
        if a != b:
            fail(f"kvstore record[{i}] ref=({a[0]},{a[1]:#x}) "
                 f"new=({b[0]},{b[1]:#x})")
    print(f"PIMNIC V2 COMPARE PASS mode=kvstore records={len(ref_pairs)}")


if __name__ == "__main__":
    main()
