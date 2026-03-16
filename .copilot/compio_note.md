# Compio Crash Protection & Data Durability

Compio now implements a robust crash protection system using two complementary mechanisms: **Double-Buffered Headers** for metadata and **Write-Ahead Logging (WAL)** for data consistency.

## 1. Double-Buffered Headers (Metadata Atomicity)

To prevent metadata corruption during a crash (e.g., power loss while writing the header), the archive header is stored in two locations:
1.  **Slot A**: At the beginning of the file (offset 0).
2.  **Slot B**: At the end of the file (offset `disk_size`).

### Mechanism:
-   Each header includes a **Sequence ID** and a **SHA-256 Checksum**.
-   **Write**: When updating metadata, Compio writes to the slot that currently holds the *older* sequence ID (or is invalid), ensuring the previous valid header remains intact until the new write completes.
-   **Read/Open**:
    1.  Compio reads both slots.
    2.  Validates checksums for both.
    3.  Selects the header with the **highest valid Sequence ID**.
    4.  If one slot is corrupted (invalid checksum), the other valid slot is used seamlessly.
    5.  If both are corrupted, the archive is considered broken (fail-fast).

This ensures that at any point in time, at least one valid header exists on disk.

## 2. Write-Ahead Logging (WAL) (Data Durability)

To ensure data integrity for file contents and structural changes, Compio uses a Write-Ahead Log.

### Mechanism:
-   **Log File**: A separate file (`.wal`) alongside the archive.
-   **Write Flow**:
    1.  All structural changes (allocator updates) and data writes are first appended to the WAL.
    2.  Only after the WAL entry is flushed to disk does the operation proceed to modify the main archive file.
    3.  Once the operation completes successfully, the WAL is truncated or marked as committed.
-   **Recovery**:
    -   On `compio_open_archive`, the system checks for a non-empty `.wal` file.
    -   If found, it replays the logged operations (redo log) to restore the archive to a consistent state.
    -   This handles cases where the process crashed *after* writing to the log but *before* or *during* the write to the main archive.

## Summary

-   **Headers**: Atomic updates via double buffering. Protects global archive state (file count, root node, etc.).
-   **Data**: Durability via WAL. Protects file contents and allocation tables.

This architecture ensures Compio is resilient to power failures and application crashes, maintaining data integrity automatically.
