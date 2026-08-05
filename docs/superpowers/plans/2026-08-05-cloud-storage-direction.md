# Cloud & storage direction plan

Date: 2026-08-05. Status: **Track S + C core complete** — S1–S8 + C1–C7 +
MEM-3 lite + gRPC unary + latency bench shipped; remaining optional:
C8 OTel (defer), full arena region tags, H2 trailers-native gRPC.
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
| **graceful shutdown (SIGTERM)** | **shipped** | **C1**: `signal_watch`/`check`/`clear`/`wait`/`raise` builtins (2026-08-05) |
| metrics | **shipped** | **C2**: `metbaga` — Prometheus text, counter/gauge, `met_render` |
| structured logs | **shipped** | **C3**: `logbaga` — JSON lines, levels, request ids |
| health/readiness | **shipped** | **C4**: `cloudbaga` demo — `/healthz` `/readyz` `/metrics` + SIGTERM drain |
| gRPC | **codec+frame+unary shipped** | **C5**: `pbbaga` + `grpc_unary` (binary HTTP body; H2 trailers still approximate) |
| resilience | **shipped** | **C6**: `relbaga` — retry+backoff, circuit breaker, bulkhead |
| tracing (OTel) | **lite shipped** | **C8**: `otelbaga` W3C traceparent + log fields; OTLP export still deferred |
| flags/CLI | **shipped** | **C7**: `flagbaga` — typed CLI flags (`--name`, `=`, bools, positionals) |

Flagship probe: a 12-factor demo service (config via env, `/metrics`,
JSON logs, graceful shutdown) in the existing Docker image — deployable
manifest included.

## 2. Track S — Storage / DB engine

Foundation-first; an engine without memory control and durable IO is a toy.

| Step | What | Why |
|------|------|-----|
| **S1** | MEM-1/2/3 from `2026-08-05-memory-management.md` | MEM-1/2 **shipped**; MEM-3 **lite** (arena handle seatbelt) shipped |
| **S2** | bytes mutators (httpdbaga gap G9): `bytes_set/push/builder` | **shipped** (`bytes_new`/`set`/`push`) |
| **S3** | `std/os`: `pread/pwrite/fsync/fdatasync` (+`fallocate`) | **shipped** (+ binary `fd_*_bytes`) |
| **S4** | `std/crypto/crc32c.baga` | **shipped** |
| **S5** | page cache package (clock/LRU) | buffer management — **shipped** in `app-product/lsmbaga/page.baga` (2026-08-05) |
| **S6** | **`lsmbaga`** — the engine probe: WAL → memtable → SSTable flush + compaction-lite, on top of kvbaga's RESP protocol so redis-cli keeps working | the flagship — **MVP shipped** 2026-08-05 |
| **S7** | **`raftbaga`** — leader election + log replication over channels; the M16 channel-invariant fragment proves what it can (append-only log matching, term monotonicity) and honestly skips the rest | the distributed exam — **MVP shipped** 2026-08-05 |
| **S8** | 2PC coordinator + MVCC notes | **MVP shipped** — `txnbaga` (2026-08-05) |

Performance analysis is part of the profile: **`bench/run_latency.sh`**
(`bench/latency.baga`) reports batch min/avg/max/p50/p99 via
`monotonic_ms`, alongside the existing cbmc oracle in `bench/`.

## 3. Sequencing (probe-per-feature, as the apps-roadmap worked)

1. **C1 signals** + **S2 bytes mutators** + **S3 file IO** + **S4 crc32c** — small, independent, unlock both tracks. **Done** (C1 with cloud batch).
2. **C2 metrics** + **C3 logging** + **C4 demo service** — the K8s story becomes demoable. **Done** (`metbaga`, `logbaga`, `cloudbaga`).
3. **MEM-1/2** — the foundation (L3 lands before it, per schedule). **Done**.
4. **S5+S6 lsmbaga MVP** — the storage credibility probe. **Done** (see `app-product/lsmbaga`, `tests/lsm_test.baga`).
5. **C5 protobuf/gRPC** — microservice interop. **Codec+frame done** (`pbbaga`); H2 unary glue optional follow-up.
6. **S7 raftbaga** — the consensus exam; then S8. **Done** (`app-product/raftbaga`, `tests/raft_test.baga`).
7. **S8 txnbaga** — 2PC + MVCC. **Done** (`app-product/txnbaga`, `tests/txn_test.baga`).

## 4. Honest boundaries (write them down now)

- **No async/await**: CSP over OS threads + poll loop. Trade: one stack per
  connection (document memory math), no function coloring, verifier-friendly.
- **io_uring**: Linux-only, later; the poll abstraction keeps the door open.
- **The verifier will not prove Raft safe in v1** — it proves local protocol
  fragments (term/append invariants) and says UNKNOWN elsewhere. That is
  still more than the ecosystems we compare against.
- **No datacenter claims**: everything targets containers on someone else's
  cloud; the Docker path is the delivery mechanism.
