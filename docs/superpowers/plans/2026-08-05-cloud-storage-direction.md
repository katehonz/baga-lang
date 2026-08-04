# Cloud & storage direction plan

Date: 2026-08-05. Status: direction plan for discussion (no code yet).
Driver: the target profile is Rust/C++-class systems work — multithreading,
async, performance analysis — plus distributed systems: distributed
transactions and consensus (Paxos/Raft). We build for cloud (Kubernetes)
and storage/DB engines; no embedded, no own datacenter.

## 0. What baga already brings (the assets)

- **Go-class concurrency**: OS threads + CSP (`go`/`join`/`chan`/`select`/
  `pool_map`) under the `!Par` effect; `poll(2)` event loop for many-conn
  servers. No async/await, no colored functions — that is a feature to
  document, not a gap.
- **The differentiator**: the `--verify` oracle already proves fork–join
  determinism (M14), channel content invariants (M16, rely–guarantee) and
  handle protocols. In the target domains nobody at this weight class
  *proves* concurrent protocols — we do.
- Network stack in pure Baga: TLS 1.3 (both suites), HTTP/1.1 + HTTP/2,
  WebSocket, Postgres wire, JSON-RPC, OAuth proxy.
- sandak + registry, Docker multi-stage, decimal money, XML — the app
  scaffolding exists and every feature lands with a product probe.

## 1. Track C — Cloud / Kubernetes services

What a K8s-grade service needs vs what we have:

| Need | Status | Step |
|------|--------|------|
| env config, exit codes | have (`env`, `main -> i64`) | — |
| **graceful shutdown (SIGTERM)** | **missing** — no signal support | **C1**: runtime signal slot + `signal_wait(sig)` builtin; rolling updates depend on it |
| metrics | missing | **C2**: `metbaga` — Prometheus text format, counter/gauge/histogram, `/metrics` handler on httpdbaga |
| structured logs | missing | **C3**: `logbaga` — JSON lines, levels, request ids (std/json exists) |
| health/readiness | trivial on httpdbaga | **C4**: document the idiom in a demo |
| gRPC | missing (H2 exists!) | **C5**: `pbbaga` — protobuf wire codec (varint/len-delimited), then a minimal gRPC server frame over httpdbaga |
| resilience | primitives exist | **C6**: `relbaga` — retry+backoff, circuit breaker, bulkhead via bounded channels |
| tracing (OTel) | big | **C8**: defer, document as later |
| flags/CLI | `arg()` only | **C7**: `flagbaga` — typed CLI flags |

Flagship probe: a 12-factor demo service (config via env, `/metrics`,
JSON logs, graceful shutdown) in the existing Docker image — deployable
manifest included.

## 2. Track S — Storage / DB engine

Foundation-first; an engine without memory control and durable IO is a toy.

| Step | What | Why |
|------|------|-----|
| **S1** | MEM-1/2/3 from `2026-08-05-memory-management.md` | the DB buffer pool/WAL need exact frees and proofs, not leak-tolerance |
| **S2** | bytes mutators (httpdbaga gap G9): `bytes_set/push/builder` | page buffers |
| **S3** | `std/os`: `pread/pwrite/fsync/fdatasync` (+`fallocate`) | WAL + page IO durability |
| **S4** | `std/crypto/crc32c.baga` | WAL/page checksums (Castagnoli, hardware-friendly polynomial) |
| **S5** | page cache package (clock/LRU) | buffer management |
| **S6** | **`lsmbaga`** — the engine probe: WAL → memtable → SSTable flush + compaction-lite, on top of kvbaga's RESP protocol so redis-cli keeps working | the flagship |
| **S7** | **`raftbaga`** — leader election + log replication over channels; the M16 channel-invariant fragment proves what it can (append-only log matching, term monotonicity) and honestly skips the rest | the distributed exam |
| **S8** | 2PC coordinator + MVCC notes | distributed transactions, later |

Performance analysis is part of the profile: add a `bench/` runner for
latency/throughput (clock_gettime, p50/p99) alongside the cbmc oracle —
perf regressions become CI-visible, not folklore.

## 3. Sequencing (probe-per-feature, as the apps-roadmap worked)

1. **C1 signals** + **S2 bytes mutators** + **S3 file IO** + **S4 crc32c** — small, independent, unlock both tracks.
2. **C2 metrics** + **C3 logging** + **C4 demo service** — the K8s story becomes demoable.
3. **MEM-1/2** — the foundation (L3 lands before it, per schedule).
4. **S5+S6 lsmbaga MVP** — the storage credibility probe.
5. **C5 protobuf/gRPC** — microservice interop.
6. **S7 raftbaga** — the consensus exam; then S8.

## 4. Honest boundaries (write them down now)

- **No async/await**: CSP over OS threads + poll loop. Trade: one stack per
  connection (document memory math), no function coloring, verifier-friendly.
- **io_uring**: Linux-only, later; the poll abstraction keeps the door open.
- **The verifier will not prove Raft safe in v1** — it proves local protocol
  fragments (term/append invariants) and says UNKNOWN elsewhere. That is
  still more than the ecosystems we compare against.
- **No datacenter claims**: everything targets containers on someone else's
  cloud; the Docker path is the delivery mechanism.
