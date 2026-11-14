# Command-line tools

Two standalone executables ship with compio for getting data **out** of an
archive. They are built together with the library (targets `compio_unpack` and
`compio_repair`) and land in `build/util/`.

| Tool | Use it when | Reads damaged metadata? |
|------|-------------|--------------------------|
| `compio_unpack` | The archive is **healthy** and you want every file extracted. | No — needs a valid header/index. |
| `compio_repair` | The archive is **corrupted** (bad header/index) and you want to salvage whatever is still intact. | Yes — bypasses metadata. |

Both print full help with `-h` / `--help`, validate their arguments, and use
standard exit codes: `0` on success, `1` on error or bad usage.

---

## `compio_unpack` — extract all files

```
compio_unpack <archive_path> <output>
```

Streams every file out of a valid archive in 1 MiB chunks, so memory use does
not depend on file size.

**Arguments**

- `archive_path` — archive to read.
- `output` — destination, interpreted by its trailing character:
  - ends with a path separator (e.g. `out/`) → **directory mode**: files keep
    their archived names inside that directory;
  - otherwise (e.g. `out_`) → **prefix mode**: each file is written as
    `<prefix><name>`.

**Options**

- `-h`, `--help` — show help and exit.

**Safety**: archived names containing `..` or absolute paths are rejected
(path-traversal guard).

**Examples**

```bash
# extract into a directory, preserving names
compio_unpack data.compio extracted/

# extract with a filename prefix into the current directory
compio_unpack data.compio dump_
```

---

## `compio_repair` — salvage a corrupted archive

```
compio_repair <archive_path> <output_dir> [--force]
```

Scans the archive sequentially by block/index signatures and extracts whatever
intact data it finds, bypassing damaged metadata. This is **best-effort**
recovery, not a guarantee.

**Arguments**

- `archive_path` — the corrupted archive.
- `output_dir` — where recovered files are written; created if missing, and
  required to be empty unless `--force` is given.

**Options**

- `-f`, `--force` — allow writing into a non-empty output directory.
- `-h`, `--help` — show help and exit.

**Limitations** (inherited from `compio_repair`):

- small files stored inline in the header may not be recoverable;
- the original directory structure is not reconstructed;
- recovery is proportional to archive size (full sequential scan).

**Example**

```bash
compio_repair broken.compio recovered/
# -> Success! Recovered N file(s) to 'recovered/'.
```

---

See [`examples/`](../examples/) for minimal programs that call the underlying
`compio_repair()` API and the extraction loop directly.
