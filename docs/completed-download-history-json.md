# Completed download history interchange format

aMule exports and imports completed-download history as UTF-8 JSON. The same file can be moved between supported aMule clients and operating systems.

```json
{
  "format": "amule.completed-download-history",
  "version": 1,
  "entries": [
    { "hash": "0123456789ABCDEF0123456789ABCDEF", "completed_at": 1781481600 }
  ]
}
```

`hash` is the 32-character eD2k MD4 file hash. `completed_at` is the completion time as whole Unix seconds (UTC). The interchange file does not contain filenames, local paths, shared status, or credentials.

Import mode is chosen by the receiving client and is not stored in the file:

- **Merge** adds unseen hashes and keeps the later completion time when the same hash appears in both histories.
- **Replace** makes the imported entries the receiving device's history.

After either operation, the receiving core applies its configured history-retention period, so older imported entries may be pruned immediately. The portable file has a 64 MiB safety ceiling and the core limits history to 700,000 records. The record ceiling is deliberately below the file ceiling so long timestamps remain exportable. The Web API transfers the file in 512 KiB pieces, so the existing 1 MiB HTTP request-body limit remains unchanged. This requires an aMule API version that provides the `completed_download_history/{import,export}` transfer routes; older API versions do not support chunked transfer.

Imports are staged in order and committed only after every byte arrives. The API scans the JSON document incrementally and sends batches of at most 512 records to the core over EC. The core validates each batch, stages its records, and writes the replacement or merge atomically at commit. Export uses an ordered core cursor and returns bounded record batches; a revision check fails cleanly if the history changes mid-export. Transfer sessions expire after ten minutes, clients can cancel failed transfers, and the API permits one active history transfer at a time. Repeating the same import commit is safe if the response was lost after the core saved the file.

The 64 MiB and 700,000-record ceilings are resource safeguards shared by the core and clients. When the live history reaches 700,000 records, a newly completed download replaces the oldest record; a merge import that would exceed the ceiling is rejected without changing history. Completion records are flushed at most every 30 seconds and on clean shutdown, avoiding a full-file rewrite for every completed download. An abrupt crash can therefore lose records added since the last flush. The Web API transfers bounded batches, and merge commits avoid keeping a second full copy of the existing history for rollback. The core protects history reads and writes with a lock so download completion recording cannot race an import or export. The desktop GUI uses the same portable JSON format and limits.
