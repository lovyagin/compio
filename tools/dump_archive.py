#!/usr/bin/env python3
"""Print the on-disk layout of a compio v5 archive.

The parser is written from the format description alone and shares no code with
the library, so decoding a real archive without contradictions is a check of
both. Usage: dump_archive.py ARCHIVE [ARCHIVE ...]
"""
import struct
import sys

MAGIC_V5 = 27110662
HEADER_SIZE = 108
NAME_SIZE = 32
TABLE_ENTRY_SIZE = NAME_SIZE + 8 + 8
NODE_SIGNATURE = 67
BLOCK_SIGNATURES_WITH_BACKREF = (173, 174)
STATE_TRAILER_SIZE = 8 + 8 + 4

HEADER_FIELDS = ('magic', 'sha256', 'sequence_id', 'index_root', 'file_size', 'alloc_off',
                 'alloc_size', 'compression_type', 'block_size', 'b_tree_degree',
                 'files_table_addr', 'files_table_capacity', 'n_files')


def read_header(buf, off):
    return dict(zip(HEADER_FIELDS, struct.unpack_from('<i32sQQQQQIIIQIQ', buf, off)))


def read_node(buf, off, out, depth=0, shift=0):
    sig, leaf, n = struct.unpack_from('<BBI', buf, off)
    assert sig == NODE_SIGNATURE, f'bad node signature {sig} at {off}'
    p = off + 6
    keys = [struct.unpack_from('<QQ', buf, p + 16 * i) for i in range(n)]
    p += 16 * n
    vals = [struct.unpack_from('<QQ', buf, p + 16 * i) for i in range(n)]
    p += 16 * n
    children, shifts = [], []
    if not leaf:
        children = list(struct.unpack_from(f'<{n + 1}Q', buf, p))
        p += 8 * (n + 1)
        shifts = list(struct.unpack_from(f'<{n + 1}q', buf, p))
        p += 8 * (n + 1)
    out['nodes'].append((off, depth, leaf, n, p - off))
    for i in range(n + 1):
        if not leaf:
            read_node(buf, children[i], out, depth + 1, shift + shifts[i])
        if i < n:
            out['entries'].append(((keys[i][0], keys[i][1] + shift), vals[i]))
    return out


def read_block(buf, off):
    sig, compressed, size, original = struct.unpack_from('<BBQQ', buf, off)
    p = off + 18
    backref = None
    if sig in BLOCK_SIGNATURES_WITH_BACKREF:
        backref = struct.unpack_from('<QQ', buf, p)
        p += 16
    p += 4  # checksum
    return dict(sig=sig, compressed=compressed, size=size, original=original, backref=backref,
                meta=p - off, total=p - off + size)


def read_allocator_state(buf, off, size):
    (count,) = struct.unpack_from('<Q', buf, off)
    list_size = 8 + 16 * count
    assert size >= list_size, 'allocator state is shorter than its entry count implies'
    free = [struct.unpack_from('<QQ', buf, off + 8 + 16 * i) for i in range(count)]
    spare = None
    if size != list_size:
        assert size >= list_size + STATE_TRAILER_SIZE
        spare = struct.unpack_from('<QQ', buf, off + list_size)
    return free, spare


def dump(path):
    buf = open(path, 'rb').read()
    print(f'== {path}: {len(buf)} bytes')
    slots = [read_header(buf, 0), read_header(buf, HEADER_SIZE)]
    for i, h in enumerate(slots):
        print(f' header slot {i} @ {i * HEADER_SIZE}: magic={h["magic"]} seq={h["sequence_id"]} '
              f'root={h["index_root"]} file_size={h["file_size"]} '
              f'alloc=({h["alloc_off"]},{h["alloc_size"]}) comp={h["compression_type"]} '
              f'block={h["block_size"]} degree={h["b_tree_degree"]} '
              f'ft=({h["files_table_addr"]},cap={h["files_table_capacity"]}) n_files={h["n_files"]}')
    h = max((s for s in slots if s['magic'] == MAGIC_V5), key=lambda s: s['sequence_id'])
    print(f' active header: seq={h["sequence_id"]}')

    node_size = 96 * h['b_tree_degree'] - 26
    regions = [(0, 2 * HEADER_SIZE, 'header x2')]

    table, capacity = h['files_table_addr'], h['files_table_capacity']
    if table:
        regions.append((table, capacity * TABLE_ENTRY_SIZE, f'files table (capacity {capacity})'))
        for i in range(h['n_files']):
            name, size, file_id = struct.unpack_from(f'<{NAME_SIZE}sQQ', buf,
                                                     table + i * TABLE_ENTRY_SIZE)
            print(f'  file[{i}] name={name.split(b"\0")[0].decode()!r} size={size} file_id={file_id}')

    index = read_node(buf, h['index_root'], {'nodes': [], 'entries': []})
    for off, depth, leaf, n, used in index['nodes']:
        regions.append((off, node_size, f'index node depth={depth} leaf={leaf} keys={n} used={used}'))

    stored = 0
    for (file_id, pos), (addr, original) in index['entries']:
        blk = read_block(buf, addr)
        assert blk['original'] == original, (blk, original)
        assert blk['backref'] is None or blk['backref'][0] == file_id
        stored += blk['total']
        regions.append((addr, blk['total'],
                        f'block file_id={file_id} pos={pos} size={original} stored={blk["size"]} '
                        f'compressed={blk["compressed"]} sig={blk["sig"]}'))

    if h['alloc_off']:
        free, spare = read_allocator_state(buf, h['alloc_off'], h['alloc_size'])
        regions.append((h['alloc_off'], h['alloc_size'], f'allocator state: {len(free)} free regions'))
        if spare and spare[1]:
            regions.append((spare[0], spare[1], 'allocator state, spare slot'))
        regions += [(off, size, 'free') for off, size in free]

    regions.sort()
    print(' layout:')
    end = 0
    for off, size, what in regions:
        if off > end:
            print(f'  [{end:>10} .. {off:>10})  {off - end:>9}  ??? unaccounted')
        print(f'  [{off:>10} .. {off + size:>10})  {size:>9}  {what}')
        end = max(end, off + size)
    if end < len(buf):
        print(f'  [{end:>10} .. {len(buf):>10})  {len(buf) - end:>9}  ??? unaccounted tail')
    print(f' blocks={len(index["entries"])} nodes={len(index["nodes"])} node_size={node_size} '
          f'stored_block_bytes={stored}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for archive_path in sys.argv[1:]:
        dump(archive_path)
