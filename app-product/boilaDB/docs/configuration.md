# Configuration

boilaDB is configured with environment variables. There is no config
file in v1. Unset variables use the defaults below.

## Paths and listeners

| Variable | Default | Used by | Meaning |
|----------|---------|---------|---------|
| `BOILA_PATH` | `/tmp/baga_boila` | HTTP, PG, shell | Server root on disk |
| `BOILA_PORT` | `6570` | `tools/serve.baga` | HTTP listen port |
| `BOILA_PGPORT` | `6575` | `tools/serve_pg.baga` | PostgreSQL wire port |

`BOILA_PGPORT` never inherits `PGPORT`. Client packages
([boilabaga](../../boilabaga/README.md)) follow the same rule.

## Storage

| Variable | Default | Meaning |
|----------|---------|---------|
| `BOILA_SHARDS` | `4` | rocksbaga instances per database (fixed at first open; max 64) |
| `BOILA_FLUSH_AT` | `256` | Memtable flush threshold (passed to rocksbaga) |
| `BOILA_COMPACT_AT` | `4` | Compaction pick threshold |
| `BOILA_TARGET_BYTES` | `1048576` (1 MiB) | L0 byte target → L1=4M, L2=16M, L3=64M. `0` disables byte targets |
| `BOILA_MERGE_PICK` | (unset) | Override rocksbaga merge-pick if set |
| `BOILA_MAX_DB` | `64` | Open-database LRU; FIFO eviction of idle DBs |

## Concurrency

| Variable | Default | Meaning |
|----------|---------|---------|
| `BOILA_WORKERS` | `4` | Bounded pool (cap 64). `0` = `go_bg` per connection |
| `BOILA_MAX_CONN` | `64` | Admission cap. Over → HTTP 503 / PG `53300` |

A full worker queue blocks `accept` (backpressure). Keep-alive holds
the worker for the life of the connection.

## Query budget and transactions

| Variable | Default | SQLSTATE | Meaning |
|----------|---------|----------|---------|
| `BOILA_BUDGET_MS` | `5000` | `57014` | Wall deadline (cooperative tick every 64 keys). `0` = already expired (tests) |
| — | 100000 | `54000` | `max_scan` / `max_rows` (compile-time constants in `core/budget.baga`) |
| `BOILA_TXN_MAX` | `100000` | `54000` | Max distinct keys in a `BEGIN` buffer. `0` = unlimited |
| `BOILA_JOIN_NL_MAX` | `8` | — | Nested-loop join if outer ≤ this; else hash join |

Budget is **not** a preemptive kill inside a rocksbaga GET.

## Time-series sweeper

| Variable | Default | Meaning |
|----------|---------|---------|
| `BOILA_SWEEP_MS` | (off unless set) | Period of the TTL sweeper (`boila_ttl_sweeper`) |
| `BOILA_SWEEP_SYS_ROUNDS` | `500` | Sys-catalog keys per sweep turn |
| `BOILA_SWEEP_DATA_ROUNDS` | `2000` | Data keys per sweep turn |

TTL itself is table metadata: `CREATE TABLE … WITH (ttl_days = N | ttl_sec = N)`.
rocksbaga expires lazily on GET (`lsm_put_ex`); the sweeper flushes and
touches keys so dead rows leave the LSM.

## Auth and HTTP session

| Variable | Default | Meaning |
|----------|---------|---------|
| `BOILA_TOKEN` | (empty) | Shared secret. Empty catalog + empty token = **trust** |
| `BOILA_CSRF` | (off) | `1` / `true` → Cookie + `X-Boila-Db` must match (double-submit) |

See [security.md](security.md) and [http.md](http.md).

## Client-only (boilabaga / ormbaga)

These are **not** read by the server. They only configure the Baga
client:

| Variable | Default | Notes |
|----------|---------|--------|
| `BOILA_PGHOST` | `127.0.0.1` | Falls back to `PGHOST` only |
| `BOILA_PGPORT` | `6575` | Never inherits `PGPORT` |
| `BOILA_PGUSER` | `boila` | Never inherits `PGUSER` |
| `BOILA_PGPASSWORD` | `""` | Trust when empty; never inherits `PGPASSWORD` |
| `BOILA_PGDATABASE` | `boila` | Never inherits `PGDATABASE` |
| `BOILA_TOKEN` | — | If set, sent as the cleartext password |

## On-disk layout

`BOILA_PATH` is the server root:

```
$BOILA_PATH/
  .meta/            # registry (1 rocksbaga shard): databases, users, grants
  boila.db          # default database cluster
  boila.db.s0       # shard 0
  boila.db.s1       # …
  other.db          # CREATE DATABASE other
  other.db.s0
```

Catalogs are **per database**. There is no cross-database query or
transaction (`0A000`). `DROP DATABASE` deletes files by known shard
templates; empty directories may remain (no `rmdir` builtin — gaps S4).

## Version advertised on the wire

HTTP `/health` and `/metrics` (`boila_build_info`) and PG
`server_version` report **boilaDB 0.1.0**.
