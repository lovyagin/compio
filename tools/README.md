# Tools

## Format inspection

Both scripts are independent parsers written from the format description; they
need only the Python standard library.

```
python3 tools/dump_archive.py ARCHIVE     # headers, files table, index, blocks, free regions as a layout map
python3 tools/dump_wal.py ARCHIVE.wal     # record counts by type, transactions, checksum check
```

`dump_archive.py` reports every byte range of the archive it can attribute and
marks the rest as unaccounted, which makes leaked or doubly used space visible.

## Plotting benchmark results

Install Python dependencies:

```
pip install -r requirements.txt
```
