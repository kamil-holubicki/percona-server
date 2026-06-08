# Binlog Server &mdash; Documentation

A Percona Server 9.6 add-on that turns a MySQL instance into a **binlog
server**: it connects to one or many upstream MySQL sources as a
standard asynchronous replica, intercepts the raw binary log stream via
the IO thread, materialises it onto local storage, and **serves** it to
downstream replicas via standard MySQL replication protocol.

It is not a SQL applier. It never opens a transaction, never runs DDL,
never creates a single row. From the upstream's point of view it is an
ordinary asynchronous replica that only runs the IO thread. From a
downstream replica's point of view it is a standard MySQL source.

```mermaid
flowchart LR
    classDef src fill:#e3f2fd,stroke:#1976d2,color:#0d47a1
    classDef hub fill:#fff8e1,stroke:#f9a825,color:#5d4037
    classDef disk fill:#fafafa,stroke:#9e9e9e,color:#424242
    classDef rep fill:#e8f5e9,stroke:#43a047,color:#1b5e20

    SRC1["mysqld<br/>source A"]:::src
    SRC2["mysqld<br/>source B"]:::src

    subgraph Hub["Binlog Server (this Percona Server instance)"]
        direction TB
        REL["binlog_server_relay<br/>(plugin)<br/>IO-thread observer"]:::hub
        STO["component_binlog_server<br/>BinlogArchive + ArchiveSender"]:::hub
        DISK[("on-disk archive<br/>file://root/channel/...")]:::disk

        REL -- "append_event" --> STO
        STO --> DISK
    end

    REP1["mysqld<br/>replica 1"]:::rep
    REP2["mysqld<br/>replica 2"]:::rep

    SRC1 -- "COM_BINLOG_DUMP_GTID<br/>channel='src_a'" --> REL
    SRC2 -- "COM_BINLOG_DUMP_GTID<br/>channel='src_b'" --> REL
    DISK -- "serve via<br/>ArchiveSender" --> REP1
    DISK -- "serve via<br/>ArchiveSender" --> REP2
```

## Documentation map

| Page | Audience | What you'll find |
| --- | --- | --- |
| [Overview](overview.md) | Everyone | The problem, the use cases, the moving parts, the glossary. Read this first. |
| [User guide](user_guide.md) | Operators | System variables, `CHANGE REPLICATION SOURCE` syntax, installation, day-2 operations. |
| [Architecture](architecture.md) | Architects, reviewers | Component / plugin / server split, data-flow diagrams, threading model, failure model. |
| [Low-level design](design.md) | Developers | Module layout, locking discipline, on-disk file format, deduplication watermark, crash recovery, observer lifecycle, error code catalogue. |

## Quickstart

```sql
-- 1) Load the moving parts (run on the binlog-server node).
INSTALL COMPONENT 'file://component_binlog_server';
INSTALL PLUGIN binlog_server_relay SONAME 'binlog_server_relay.so';

-- 2) Tell it where to put the archive.
SET GLOBAL binlog_server.default_storage_uri = 'file:///var/binlog-store/';

-- 3) Point a channel at an upstream and start collecting. GTID_MODE
--    must be ON and SOURCE_AUTO_POSITION must be 1 for BINLOG_SERVER.
CHANGE REPLICATION SOURCE TO
    SOURCE_HOST           = 'src-a.example.com',
    SOURCE_PORT           = 3306,
    SOURCE_USER           = 'repl',
    SOURCE_PASSWORD       = '...',
    SOURCE_AUTO_POSITION  = 1,
    BINLOG_SERVER         = 1
    FOR CHANNEL 'src_a';

START REPLICA IO_THREAD FOR CHANNEL 'src_a';

-- 4) Enable serving: downstream replicas connecting to this node
--    will be served from the 'src_a' archive.
SET GLOBAL binlog_server.default_serve_channel = 'src_a';

-- 5) On a downstream replica, point at the binlog-server node:
--    CHANGE REPLICATION SOURCE TO
--      SOURCE_HOST = 'binlog-server.example.com',
--      SOURCE_PORT = 3306,
--      SOURCE_USER = 'repl',
--      SOURCE_AUTO_POSITION = 1;
--    START REPLICA;
```

## At a glance

| Aspect | What it means |
| --- | --- |
| **Single binary, two pieces** | `component_binlog_server` owns storage and serving; `binlog_server_relay` is the IO-thread observer plugin that ships events into it. |
| **One channel per upstream** | Each channel has its own on-disk subdirectory, its own mutex, its own watermark. No global serialisation between channels. |
| **Serve to multiple downstreams** | The `ArchiveSender` intercepts `COM_BINLOG_DUMP_GTID` and streams archived events to any number of concurrent downstream replicas. |
| **No SQL applier** | `BINLOG_SERVER=1` strips the SQL thread off the channel. The binlog server is a write-back buffer, never an applier. |
| **GTID auto-position only** | `BINLOG_SERVER` requires `SOURCE_AUTO_POSITION=1`. |
| **Crash-safe** | On startup, the archive walks the last file to find the last fully-written event, truncates any partial tail, and restores the watermark. |
| **Durable at transaction boundaries** | Every `Xid` / `XA_prepare` event triggers an `fsync(2)`, ensuring committed transactions survive a power loss. |

## Current status (Phase 1 + Phase 2)

**Phase 1** — collection path:

- Receive binlogs from upstream sources via the IO thread
- Store them as local files mirroring the source's binlog naming
- Maintain a `binlog.index` per channel
- Crash recovery with partial-event truncation
- Transaction-boundary fsync for durability

**Phase 2** — serve path:

- Downstream replicas connect with standard `CHANGE REPLICATION SOURCE` + `START REPLICA`
- `COM_BINLOG_DUMP_GTID` intercepted via atomic dispatch slot in `mysql_binlog_send()`
- GTID AUTO_POSITION fully supported (reverse-walk archive index to find starting file)
- GTID filtering: transactions already in the replica's executed set are skipped
- Tail-follow on the actively-written file with heartbeat every 5 seconds
- Multiple concurrent downstream replicas supported
- `default_serve_channel` sysvar routes dump connections to a named channel archive

**Not yet implemented** (planned for later phases):
- Per-user channel routing (`user_channel_map` sysvar)
- S3-compatible object storage backend
- Performance Schema observability tables
- Binlog purging
- Local rotation / rewriting

## License

GPLv2, matching the rest of Percona Server.
