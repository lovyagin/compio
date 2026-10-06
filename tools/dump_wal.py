#!/usr/bin/env python3
"""Summarise a compio write-ahead log (the <archive>.wal file).

The parser is written from the format description alone and shares no code with
the library. Record layout, little-endian:
    type u8, address in the archive u64, payload size u64, FNV-1a-32 of the payload u32, payload.
Usage: dump_wal.py WAL [WAL ...]
"""
import collections
import struct
import sys

RECORD_HEADER_SIZE = 1 + 8 + 8 + 4
RECORD_TYPES = {1: 'BLOCK', 2: 'INDEX_NODE', 4: 'ALLOCATOR', 255: 'COMMIT'}
COMMIT = 255


def fnv1a_32(data):
    h = 0x811c9dc5
    for byte in data:
        h = ((h ^ byte) * 0x01000193) & 0xffffffff
    return h


def dump(path):
    buf = open(path, 'rb').read()
    off = 0
    bad_checksums = 0
    records = collections.Counter()
    record_bytes = collections.Counter()
    transactions = []
    in_transaction = 0

    while off + RECORD_HEADER_SIZE <= len(buf):
        kind, _addr, size, checksum = struct.unpack_from('<BQQI', buf, off)
        payload = buf[off + RECORD_HEADER_SIZE: off + RECORD_HEADER_SIZE + size]
        if len(payload) != size or kind not in RECORD_TYPES:
            break
        if fnv1a_32(payload) != checksum:
            bad_checksums += 1
        name = RECORD_TYPES[kind]
        records[name] += 1
        record_bytes[name] += RECORD_HEADER_SIZE + size
        if kind == COMMIT:
            transactions.append(in_transaction)
            in_transaction = 0
        else:
            in_transaction += 1
        off += RECORD_HEADER_SIZE + size

    print(f'{path}: {len(buf)} bytes, parsed {off} bytes, checksum mismatches={bad_checksums}')
    for name in records:
        print(f'  {name:<11} records={records[name]:>6}  bytes={record_bytes[name]:>9}')
    if transactions:
        empty = sum(1 for n in transactions if n == 0)
        print(f'  transactions={len(transactions)}, empty (COMMIT only)={empty}, '
              f'largest={max(transactions)} records')
    if in_transaction:
        print(f'  uncommitted tail: {in_transaction} records (discarded by recovery)')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for wal_path in sys.argv[1:]:
        dump(wal_path)
