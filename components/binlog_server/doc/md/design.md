# Low-level design

[&larr; Back to index](index.md) &middot;
[Overview](overview.md) &middot;
[User guide](user_guide.md) &middot;
[Architecture](architecture.md) &middot;
**Low-level design**

## Contents

- [Source map](#source-map)
- [Locking discipline](#locking-discipline)
- [ChannelState anatomy](#channelstate-anatomy)
- [On-disk archive format](#on-disk-archive-format)
- [Append path](#append-path)
- [Deduplication watermark](#deduplication-watermark)
- [Durability boundaries](#durability-boundaries)
- [Crash recovery](#crash-recovery)
- [Rotate handling](#rotate-handling)
- [FDE handling](#fde-handling)
- [Index management](#index-management)
- [Sanitize binlog name](#sanitize-binlog-name)
- [Observer plugin lifecycle](#observer-plugin-lifecycle)
- [Configure-time notify path](#configure-time-notify-path)
- [Error code catalogue](#error-code-catalogue)
- [Build wiring](#build-wiring)

## Source map

| File | Role |
| --- | --- |
| `components/binlog_server/binlog_server_component.cc` | Component entry point. Publishes `binlog_server_storage` service, acquires logging services. |
| `components/binlog_server/binlog_archive.h` | Declares `BinlogArchive` (singleton), `ChannelState` (per-channel), and all protocol methods. |
| `components/binlog_server/binlog_archive.cc` | Implements configure, append, close, crash recovery, rotation, deduplication. |
| `components/binlog_server/storage_backend.h` | Abstract `StorageBackend` interface. |
| `components/binlog_server/file_storage.h` | `FileStorage` &mdash; concrete backend for `file://` URIs. |
| `components/binlog_server/file_storage.cc` | Directory creation, path resolution. |
| `components/binlog_server/log_helpers.h` | `bslog()`, `bslog_code()` declarations. |
| `components/binlog_server/log_helpers.cc` | Logging implementation using `LogComponentErr`. |
| `plugin/binlog_server_relay/binlog_server_relay.cc` | Plugin: `Binlog_relay_IO_observer` callbacks, RAII service acquisition. |
| `sql/rpl_binlog_server.h` | Notify function declarations + URI shape validator. |
| `sql/rpl_binlog_server.cc` | Service acquisition, `binlog_server_notify_channel_config()`. |
| `include/mysql/components/services/binlog_server_storage.h` | Service ABI (`configure_channel`, `append_event`, `close_channel`). |

## Locking discipline

```
Global:
  No global lock. BinlogArchive's channel map is protected by its
  own mutex (channel_map_mutex_), but only for map
  insert / lookup / erase operations.

Per-channel:
  Each ChannelState has its own std::mutex (mtx). All operations on
  a channel (configure, append, close, rotate) hold this mutex for
  the duration of the call.

Lock ordering:
  channel_map_mutex_ → ChannelState::mtx

  Never hold two ChannelState mutexes simultaneously.
  Never hold ChannelState::mtx while acquiring channel_map_mutex_.
```

The single-mutex-per-channel design means channels cannot deadlock
against each other, and the IO thread (which is per-channel) never
contends with IO threads of other channels.

## ChannelState anatomy

```cpp
struct ChannelState {
  std::mutex mtx;

  // On-disk I/O
  std::ofstream out;
  int fsync_fd = -1;

  // Current file tracking
  std::string current_log_name;
  std::string base_dir;

  // Watermark
  uint64_t last_source_log_pos = 0;

  // Protocol state
  bool wrote_fde = false;
  bool has_checksum = false;

  // Counters
  uint64_t events_appended = 0;
  uint64_t bytes_appended = 0;

  // Index state
  bool index_loaded = false;
  std::set<std::string> indexed_files;
  std::vector<std::string> file_order;
};
```

Key invariants:

- `out.is_open()` iff we are actively collecting into a file.
- `fsync_fd` is a POSIX fd opened on the same path, used exclusively
  for `fsync(2)` calls (the `std::ofstream` doesn't expose its fd).
- `current_log_name` is the basename (e.g. `binlog.000003`), not a
  full path.
- `last_source_log_pos` is always the `log_pos` of the last
  fully-written event in the current file. Zero means no events
  written yet.
- `wrote_fde` is reset on every file rotation. It prevents writing
  a second FDE if the upstream re-sends one on reconnect within the
  same file.

## On-disk archive format

Each archived file is a **byte-identical copy** of the source's binlog
file, starting from the 4-byte magic number (`\xfebin`) through the
last event appended:

```
Offset 0:    fe 62 69 6e              (magic: "\xfebin")
Offset 4:    [FDE - 119+ bytes]       (Format Description Event)
Offset ...:  [event] [event] ...      (data events, in source order)
Offset EOF:  [Rotate event]           (written at file close/rotation)
```

Each event has the standard 19-byte common header:

```
Bytes 0-3:   timestamp (4 bytes, little-endian)
Byte  4:     type_code (1 byte)
Bytes 5-8:   server_id (4 bytes, little-endian)
Bytes 9-12:  event_length (4 bytes, little-endian)
Bytes 13-16: log_pos (4 bytes, little-endian, next event position)
Bytes 17-18: flags (2 bytes, little-endian)
```

If `has_checksum` is true, each event has a 4-byte CRC32 appended
after the declared `event_length - 4` payload bytes.

## Append path

The `append_event` method is the hot path, called once per event by
the IO thread:

```
append_event(channel, buf, len):
  1. Lock ChannelState::mtx
  2. Parse 19-byte header → type_code, event_length, log_pos
  3. Deduplication: if log_pos > 0 && log_pos <= last_source_log_pos → drop
  4. Switch on type_code:
     - FORMAT_DESCRIPTION_EVENT → handle_format_description_event()
     - ROTATE_EVENT → handle_rotate_event()
     - other → write raw bytes
  5. Update watermark: last_source_log_pos = log_pos
  6. Increment counters
  7. If type_code == XID_EVENT || type_code == XA_PREPARE_LOG_EVENT:
     → sync_channel_to_disk()
```

## Deduplication watermark

The watermark is `last_source_log_pos` &mdash; the byte offset in the
source's binlog where the next event would start. Because GTID-based
auto-position can re-deliver events that were already received before
a disconnect:

- If `log_pos == 0`: the event is a pseudo-event (e.g. heartbeat or
  artificial rotate) and is always processed.
- If `log_pos > 0 && log_pos <= last_source_log_pos`: the event is a
  duplicate. Drop it.
- If `log_pos > last_source_log_pos`: new event. Append it.

The watermark is reset to 0 on file rotation (new source file means
new offset space). It is persisted indirectly: on crash recovery,
it is re-derived by walking the last file.

## Durability boundaries

```
Event types that trigger fsync:
  - XID_EVENT (33)           → committed transaction
  - XA_PREPARE_LOG_EVENT (38) → XA prepare

Event types that trigger flush + fsync + close:
  - Rotate (explicit)        → close current file, fsync, open new
  - close_channel()          → IO thread stopping
```

Between transaction boundaries, writes are buffered by the OS page
cache. This trades a small window of potential data loss (events
since the last Xid) for significantly better throughput.

The `fsync_fd` trick: we open a second POSIX file descriptor on the
same path (read-only) specifically for `fsync(2)`, because
`std::ofstream` does not expose its internal fd. The `ofstream::flush()`
ensures data reaches the kernel buffer; `::fsync(fsync_fd)` ensures it
reaches stable storage.

## Crash recovery

Executed in `configure_channel` → `recover_state_from_archive`:

```
recover_state_from_archive(ChannelState &cs):
  1. Read binlog.index line by line → populate indexed_files, file_order
  2. If file_order is empty → fresh channel, return
  3. last_file = file_order.back()
  4. Open last_file for reading
  5. Skip 4-byte magic
  6. last_good_offset = 4
  7. Loop:
     a. Read 19-byte header
     b. If < 19 bytes available → partial header → break
     c. Extract event_length from header
     d. If offset + event_length > file_size → partial event → break
     e. If type == FDE → wrote_fde = true, derive has_checksum
     f. last_source_log_pos = header.log_pos
     g. last_good_offset = offset + event_length
     h. Advance offset
  8. If last_good_offset < file_size:
     → truncate file to last_good_offset
     → log warning about truncated partial tail
  9. Set cs.last_source_log_pos, cs.wrote_fde, cs.has_checksum
```

## Rotate handling

When a `ROTATE_EVENT` arrives from the upstream:

```
handle_rotate_event(ChannelState &cs, buf, len):
  1. Extract new filename from event payload
  2. sanitize_binlog_name(new_name, has_checksum || !wrote_fde)
  3. If new_name == current_log_name && file is open → no-op (same file)
  4. If new_name == current_log_name && file is closed → reopen
  5. Otherwise:
     a. Write a real Rotate event to current file (marks EOF)
     b. Flush + fsync + close current file
     c. Reset: last_source_log_pos = 0, wrote_fde = false
     d. Open new file, write 4-byte magic
     e. Add new file to index (if not already present)
     f. Update current_log_name
```

The "write a real Rotate event" step ensures every archived file
ends with a proper Rotate event pointing to the next file, just like
the source's original binlog files do.

## FDE handling

The Format Description Event is special:

```
handle_format_description_event(ChannelState &cs, buf, len):
  1. If cs.wrote_fde → drop (duplicate, reconnect within same file)
  2. Derive has_checksum from FDE payload (byte at specific offset)
  3. Write event bytes to file
  4. cs.wrote_fde = true
  5. cs.has_checksum = (derived value)
```

The `wrote_fde` flag prevents writing a second FDE if the IO thread
reconnects to the upstream within the lifetime of the same archive
file. Since the FDE is always the first real event after the magic
number, a duplicate would corrupt the file.

## Index management

The `binlog.index` file is a simple text file with one filename per
line, in rotation order:

```
./binlog.000001
./binlog.000002
./binlog.000003
```

Management rules:

- **Load:** On `configure_channel`, read all lines into `indexed_files`
  (set, for O(1) lookup) and `file_order` (vector, preserves order).
- **Add:** On rotate to a new file, append the filename only if it is
  not already in `indexed_files`. This makes the operation idempotent
  across reconnects.
- **Never remove:** Phase 1 does not implement purging. Files
  accumulate until manually deleted.

## Sanitize binlog name

The source may send Rotate events with a CRC32 checksum appended to
the payload. The `sanitize_binlog_name` function strips those trailing
4 bytes when appropriate:

```
sanitize_binlog_name(name, assume_checksum):
  If assume_checksum && name.size() > 4:
    Strip last 4 bytes (they are the CRC32, not part of the filename)
  Also strip any directory prefix (e.g. "./")
  Return the clean basename
```

The `assume_checksum` parameter is `true` when either `has_checksum`
is set or `wrote_fde` is still false (meaning we haven't yet seen the
FDE that tells us the checksum algorithm, so we assume the worst case).

## Observer plugin lifecycle

```mermaid
stateDiagram-v2
    [*] --> Loaded : INSTALL PLUGIN
    Loaded --> Active : thread_start(channel)
    Active --> Active : after_queue_event(channel, buf, len)
    Active --> Loaded : thread_stop(channel)
    Loaded --> [*] : UNINSTALL PLUGIN

    note right of Active
        Service acquired via my_service RAII.
        If service unavailable, callbacks
        log warning and return 0.
    end note
```

Plugin callbacks and their behaviour:

| Callback | Action | Return |
| --- | --- | --- |
| `thread_start` | Call `configure_channel(ch, true, uri)` | Always 0 |
| `after_queue_event` | Call `append_event(ch, buf, len)` | Always 0 |
| `thread_stop` | Call `close_channel(ch)` | Always 0 |
| `after_reset_slave` | Call `close_channel(ch)` | Always 0 |

All callbacks return 0 regardless of the service call result. This
prevents binlog-server failures from crashing or stalling the IO
thread. Errors are logged via the error log with stable error codes.

## Configure-time notify path

When `CHANGE REPLICATION SOURCE TO ... BINLOG_SERVER=1` is executed:

```
change_receive_options() [sql/rpl_replica.cc]
  → binlog_server_notify_channel_config() [sql/rpl_binlog_server.cc]
    → acquire binlog_server_storage service (dynamic, may fail if not loaded)
    → service->configure_channel(channel_name, enabled, storage_uri)
    → release service
```

If the component is not loaded, the notify call logs a warning
(`ER_BINLOG_SERVER_SERVICE_UNAVAILABLE`) and returns. The channel
configuration is still persisted to `Master_info`; when the component
is loaded later, the next `thread_start` will trigger
`configure_channel` again.

## Error code catalogue

| Code | Symbol | Level | Message |
| --- | --- | --- | --- |
| 48350 | `ER_BINLOG_SERVER_CHANNEL_CONFIGURED` | Note | Binlog server channel '%s' configured with storage URI '%s' |
| 48351 | `ER_BINLOG_SERVER_IO_ERROR` | Error | Binlog server I/O error on channel '%s': %s |
| 48352 | `ER_BINLOG_SERVER_CONFIG_REJECTED` | Error | Binlog server configuration rejected for channel '%s': %s |
| 48353 | `ER_BINLOG_SERVER_RECOVERY` | Warning | Binlog server recovery on channel '%s': truncated %lu bytes of partial tail from '%s' |
| 48354 | `ER_BINLOG_SERVER_SERVICE_UNAVAILABLE` | Warning | Binlog server storage service unavailable; channel '%s' notification skipped |
| 48355 | `ER_BINLOG_SERVER_APPEND_FAILED` | Error | Binlog server append failed on channel '%s': %s |

All messages are emitted through `LogComponentErr` (component) or
`LogPluginErrMsg` (plugin) and appear in the MySQL error log with
the standard timestamp and subsystem prefix.

## Build wiring

### Component (`components/binlog_server/CMakeLists.txt`)

```cmake
MYSQL_ADD_COMPONENT(binlog_server
  binlog_server_component.cc
  binlog_archive.cc
  file_storage.cc
  log_helpers.cc
  MODULE_ONLY
)
```

### Plugin (`plugin/binlog_server_relay/CMakeLists.txt`)

```cmake
MYSQL_ADD_PLUGIN(binlog_server_relay
  binlog_server_relay.cc
  MODULE_ONLY
  MODULE_OUTPUT_NAME "binlog_server_relay"
)
```

### Server core

`sql/rpl_binlog_server.cc` is added to the `sql/CMakeLists.txt` source
list. It compiles into `mysqld` directly and uses standard component
service acquisition at runtime.

### System table DDL

`scripts/mysql_system_tables.sql` and `scripts/mysql_system_tables_fix.sql`
add the `Binlog_server` (BOOLEAN) and `Binlog_server_storage_uri` (TEXT)
columns to `mysql.slave_master_info`.

### Error messages

`share/messages_to_error_log.txt` contains the `ER_BINLOG_SERVER_*`
definitions under the "Percona Server 9.6 error log messages" section
starting at error number 48350.

---

[&larr; Back to index](index.md) &middot;
[Overview](overview.md) &middot;
[User guide](user_guide.md) &middot;
[Architecture](architecture.md) &middot;
**Low-level design**
