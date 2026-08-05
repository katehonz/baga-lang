# Changelog

## [Unreleased]

### Advanced plan DoD met
- **Plan close-out:** `docs/superpowers/plans/2026-08-05-advanced-go-rust.md`
  status **plan DoD met**; evidence table in §7; residual horizon §9/§11.
- **Write-up:** `docs/superpowers/plans/2026-08-05-advanced-plan-dod.md`
  (criteria checklist, explicit non-claims, post-plan workstreams, smoke list).

### Phase 2 B1 — ormbaga + jsonrpc L3 results
- **ormbaga:** `OrmExec` / `OrmQuery` / `OrmCount` / `MigrateResult` are L3
  enums (`OrmEOk`/`OrmEErr`, `OrmQOk`/`OrmQErr`, …); helpers `orm_ok`,
  `orm_db_q`, `migrate_is_ok`, … Apps/api + registry + oauth use helpers
  (no `ok:i64` stand-in fields).
- **jsonrpcbaga:** `RpcResult` → `JrpcOk` / `JrpcErr` / `JrpcSkip`.
- Tests: `orm_test`, `api_test`, `jsonrpc_test` green (with migrated DB).

### RocksDB path R7 — byte-size targets + L3
- **lsmbaga:** `LsmDB.target_bytes` (default 0 = file-count only). When set,
  levels compact if file-count ≥ `compact_at` **or** total SST bytes ≥
  level target (L0=T, L1=4T, L2=16T, L3=64T). **L3** promote from L2.
  Helpers `lsm_level_bytes` / `lsm_level_target`. Serve env `LSM_TARGET_BYTES`.
  Tests: `r7_*` in `tests/lsm_test.baga`. Not renamed to rocksbaga yet.

### Phase 5 — io_uring poll backend sketch
- **`tools/iouring/`:** raw x86-64 Linux probe (no liburing):
  `io_uring_setup` detect, NOP CQE, `IORING_OP_POLL_ADD` on a self-pipe.
  Design note maps future `poll_wait_iouring` to today’s `PollResult`;
  production stays `std/net/poll.baga` (`SYS_poll`). Smoke:
  `./tools/iouring/test_sketch.sh`. Baga gap: 3-arg `syscall` + no ring mmap.

### Phase 5 — structural liveness + design notes
- **`examples/verify/liveness_struct.baga`:** `--verify` proves fixed-N
  unanimous 2PC ⇒ commit and matched fan-in ⇒ balanced (counting progress,
  not full temporal liveness). Wired into `scripts/run_verify.sh`.
- **Design notes:** C′ borrow-lite
  (`docs/superpowers/specs/2026-08-05-borrow-lite-design.md`); A4 LLVM L3
  status still C-only (`…/2026-08-05-llvm-l3-status.md`).

### Phase 5 — protoc_baga sketch
- **`tools/protoc_baga/`:** proto3 subset → baga `Msg_encode`/`Msg_decode`
  (string/int64/bytes/bool). Design note + `examples/hello.proto` /
  `registry.proto`. Smoke: `./tools/protoc_baga/test_sketch.sh` (hex goldens
  + baga compile of generated Hello helpers).

### Phase 3 exit — metrics + graceful shutdown
- **fmrbaga:** `fmr_run` uses `poll_wait` + `signal_watch(SIGTERM/SIGINT)`;
  stops accepting on signal; `fmr_shutting_down()` for readiness drain.
- **apps/api + registry:** `GET /metrics` (metbaga), `/ready`/`/readyz` → 503
  while shutting down.
- **Runbook:** `docs/runbooks/product-path.md` (API + registry gRPC + probes).

### Phase 4 B4.4 — latency bench gate
- Recorded `./bench/run_latency.sh` on Ryzen 5 3600 / baga 0.7.0:
  **p50 ≈ 7 ms**, **p99 ≈ 8 ms** per 8e6-iter batch (40 batches).
  Artifacts: `bench/results/latency-2026-08-05.md`, `bench/results/latency-latest.txt`.

### Phase 4 B4.3 — multi-key 2PC concurrent stress
- **txnbaga:** participants hold **multiple concurrent PREPARE**s when locks
  do not conflict; `tpc_txn_id` for disjoint tx ranges. Stress test
  `tests/txn_stress_test.baga` (3 go_bg workers, private multi-key + hot-key
  contention).

### Phase 4 B4.1 — lsm recovery + page-cache stress
- **`tests/lsm_recover_test.baga`:** WAL/SST/tomb reopen (crash-style close),
  double reopen, many-key compact recovery, page cache `cap=2` scan still
  correct. README recovery story (`sync_every=1` fdatasync).

### Phase 4 B4.2 — raft durable log lite
- **raftbaga:** `persist.baga` saves term/vote/commit/log to
  `/tmp/baga_raft_<id>.state`; nodes load + re-apply on start; flush on
  durable changes. `raft_persist_test` covers encode/load/apply and live
  disk recovery after PUT/stop.

### Phase 3 B3.4 — gRPC interop goldens
- **`tests/grpc_goldens_test.baga`:** fixed hex vectors matching protoc wire
  (HelloRequest/Reply, gRPC length-prefix frames, registry GetPackage/Package
  shapes) + google.rpc.Code / HTTP map for 0, 3, 5, 14, 16.

### Phase 3 B3.3 — registry dual protocol (HTTP + gRPC)
- **fmrbaga:** `fmr_is_grpc_request` + `fmr_grpc_handle` hook — same port as JSON
  when `Content-Type: application/grpc` and `/Service/Method` path.
- **apps/registry:** `regbaga.Registry` RPCs `GetPackage` / `ListPackages`
  (hand PB + statusbaga codes); test `tests/registry_grpc_test.baga`.

### Phase 3 B2.4 — OpenAPI from route table
- **fmrbaga:** `fmr_openapi_from_router` emits `paths` from the live `Router`
  (path params, bearer/public heuristics, body/response schema names,
  `x-baga-route-id`). `/openapi.json` uses the app's registered routes.
  `fmr_openapi_doc` keeps a catalog router for pure tests.

### Phase 3 B2.1 — fmr middleware (request-id + otel + log)
- **fmrbaga:** ordered pipeline in `fmr_handle`: request-id → W3C
  `traceparent` (otelbaga child span) → `fmr_before` → dispatch → logbaga
  JSON line (`FMR_LOG=1`) → response headers (`X-Request-Id`, `traceparent`,
  CORS). `FmrCtx` carries `req_id` / `trace_id` / `span_id`.
- **apps/api** + **registry** mains gain `!Time` for the log path.

### RocksDB path R6 — per-block CRC + L2
- **lsmbaga:** new SSTs write **`BAGASST5`**: per-restart-block **crc32c** after
  the restart index; partial get verifies the loaded block. **L2** level:
  L1 ≥ `compact_at` → merge to L2; collapse ≥2 L2 files. v1–v4 readable.
- Not renamed to rocksbaga yet (more polish still open; R7 added targets+L3).

### RocksDB path R5 — partial SST get + L0/L1
- **lsmbaga:** new SSTs write **`BAGASST4`** (core + bloom + fixed footer).
  Get: `fd_size` → footer → bloom pages → restart index → one data block via
  page cache (no full-file materialize on miss). **v1–v3** still readable.
- **L0/L1:** MANIFEST lines `gen level`; flush → L0; L0 ≥ `compact_at` →
  merge to L1; collapse multiple L1. Pure tombs kept on partial merges.
- **std/os:** `lseek` + `fd_size` for SST footer addressing.

### RocksDB path R4 — bloom filter + chain compact
- **lsmbaga:** new SSTs write **`BAGASST3`**: restart index + **bloom filter**
  (~10 bits/key, 4 double-hash probes). Get: CRC → bloom may-contain →
  restart bsearch → block scan. **BAGASST1/2** still readable.
- Compaction **chains** oldest-N merges while `gens >= compact_at`.

### RocksDB path R3 — binary values + better compaction
- **lsmbaga:** memtable `Map<str, bytes>`; WAL/SST values as bytes; `lsm_put_b`
  for NUL-safe puts; `lsm_get` returns `bytes`. Compaction merges the **oldest**
  `compact_at` gens (keeps younger SSTs) and **drops pure tombstones**.
- **kvbaga:** `resp_bulk_b` for binary-safe RESP bulk replies.

### RocksDB path R2 — SST restart index
- **lsmbaga:** new SSTs write **`BAGASST2`**: sorted records + restart index
  (every 16 keys) + crc. `sst_get` uses restart **bsearch** + one-block scan
  (no full row materialize). **`BAGASST1`** still readable. Compaction/KEYS
  still full-parse. Full file IO remains (page-sized blocks = later).

### Positioning + RocksDB path (R1)
- **README / BASE:** Baga is an **educational systems language**; packages are
  ecosystem blocks to **prove the language**, not demos. End goal: **RocksDB-class**
  embedded KV (`lsmbaga` road).
- **lsmbaga R1:** SST lookup uses **binary search** + first/last key filter
  (still full-file parse; block index landed as R2).

### Stabilize language + applications (focus)
- Advanced plan **Phase Stabilize**: pause further Result migrations
  (orm/jsonrpc L3 deferred); keep `main` green for ship.
- B1 landed for **pbbaga** + **pgbaga**; ormbaga keeps stand-in fields +
  accessor helpers (`orm_ok_q`, `orm_db_q`, …).
- Optional light borrow (C′) remains non-mandatory direction only.
- Full `scripts/run_tests.sh` green: registry `pg_conn_of` after L3;
  force `PORT=8090` for registry (no clash with `PORT=8080`);
  jsonrpc live HTTP drains until body; apps/api runbook expanded.

### Plan: optional light borrow checker (direction only)
- `docs/superpowers/plans/2026-08-05-memory-management.md` §7 — light
  **opt-in** borrow later; **not mandatory**, default sharing unchanged.
- Advanced plan Track **C′** mirrors this; does not block product B1–B3.

### B1 pgbaga — PgResult as L3 sum
- `PgResult` → `PgOk(PgRows) | PgErr(PgFail)`; rebind with `pg_conn_of(r)`.
- Accessors sum-aware; ormbaga `orm_*_from` updated. `pg_test` green.

### B1 pbbaga — L3 decode (no ok:i64 stand-ins)
- `GrpcMsg`: `GFrame` / `GBad`; stream: `StreamOk` / `StreamEnd`
- Hello: `HelloOk(HelloBody)` / `HelloBad`; reply: `ReplyOk(str)` / `ReplyBad`
- Unary/client/demo + `pb_test` / grpc_* tests updated.

### A2 Vec/Map of sum enums
- `Vec<Res>` / `Map<K, Res>` allowed (same box path as L4 structs):
  push/get/set/slice/concat and map_set/get; missing map key → zero tag.
- Checker + C codegen; test `tests/std/sum_vec_test.baga`; probe in
  `run_tests.sh`. Docs §11.1 updated.

### A1 qualified sum variants (`Enum::Ok`)
- Syntax: `PgRes::Ok(1)`, match `PgRes::Ok(v)` / bare `Ok(v)` when unique or
  scoped to match scrutinee. Token `::`, AST `NODE_PATH`.
- Cross-enum shared names allowed; bare ambiguous → `нееднозначен`.
- Tests: `tests/std/sum_qualify_test.baga`; probes in `run_tests.sh`.
- Docs: language §11.1 (en/bg); design marked implemented.

### Advanced plan (Go/Rust) + A5 FNS_MAX hard error
- Plan: `docs/superpowers/plans/2026-08-05-advanced-go-rust.md` — Track A
  language unlock, B product migrations, C verify/MEM differentiator.
- A1 design: `docs/superpowers/specs/2026-08-05-sum-variant-qualify-design.md`.
- **A5:** exceeding `FNS_MAX` (1024) for fns/structs/enums/variants is a
  **compile error** (no more silent truncation of symbols).

### L3 sum enums as struct fields + real gRPC client
- **Language (C backend):** structs and sum enums emit in **topological
  order**, so `struct Hold { r: Res }` and nested `enum Box { BoxHas(Wrap) }`
  compile. Docs §11.1 updated (en/bg).
- **pbbaga `grpc_client`:** unary client over HTTP/1.1 binary body —
  `grpc_call_unary` returns L3 `CallOk(GrpcCallOk) | CallErr(Status)`;
  `GrpcClient.last` stores the sum on a field; wires **statusbaga** +
  **mdtbaga** + `grpc-timeout`. Live test: `tests/grpc_client_test.baga`.
- **sumtype_test:** field + nested sum/struct cases.

### gRPC-shaped universal packages (status / metadata / context)
- **statusbaga**: gRPC codes 0–16 (`GRPC_OK`…`GRPC_UNAUTHENTICATED`),
  `Status`, HTTP mapping, trailer helpers, `grpc-timeout` encode/decode.
- **mdtbaga**: metadata multimap (lowercase keys, multi-value, reserved
  header filter) — Go `metadata.MD`.
- **ctxbaga**: deadline / cancel / values + `ctx_from_grpc_timeout`
  (depends on statusbaga).
- **pbbaga**: unary glue uses status codes/messages; `grpc_unary_with_status`,
  `grpc_response_with_md`.
- Tests: `status_test`, `mdt_test`, `ctx_test` + existing grpc unary/bidi.

### Universal foundation packages + file_exists
- **pathbaga**: POSIX-ish path helpers — `path_join`, `path_basename` /
  `dirname`, `path_ext` / `path_stem`, `path_is_abs` (pure).
- **globbaga**: shell-style `*` / `?` match + `glob_filter` for KEYS/routing.
- **uuidbaga**: UUID v4 (RFC 4122) + `uuid_ok` validator (`!Random`).
- **bufbaga**: `StrBuf` string builder (`buf_push` / `buf_str`) — closes
  md/template quadratic-concat gaps (M1).
- **querybaga**: URL query / form parse+encode with `+`→space decode
  (`query_parse` / `query_encode` / `query_from_path`) — G7-style gap.
- **std/os**: `access(2)` extern + `file_exists` / `file_readable` helpers.
- Tests: `path_test`, `glob_test`, `uuid_test`, `buf_test`, `query_test`,
  `os_fs_test` (exists/readable).

### gRPC bidi algebra + OTLP mock-collector integration
- **pbbaga**: `hello_bidi` / `hello_bidi_requests`, `grpc_hello_stream_response`
  (server-stream when unary n>1, else bidi). Live H1 test:
  `tests/grpc_bidi_test.baga`.
- **otelbaga**: end-to-end `otel_export_http` against in-process mock
  collector (`tests/otel_http_test.baga` → POST `/v1/traces`, 200).

### OTLP/JSON export, gRPC streaming frames, MEM-3 mut rebind
- **otelbaga**: `OtelSpan`, `otel_span_to_otlp_json` (base64 ids),
  `otel_export_file` / `otel_export_http` / `otel_export_span_file`.
- **pbbaga**: `grpc_stream_append` / `grpc_stream_next` / `grpc_stream_count`,
  `hello_stream_replies` (server-streaming message layer).
- **MEM-3**: `mut p = arena_alloc(...)` rebinds region; free of old arena
  no longer kills rebound `p` (probes in `run_tests.sh`).

### C8 lite otelbaga + MEM-3 region tags + cloudbaga gRPC
- **otelbaga**: W3C `traceparent` parse/format/new/child/from_header;
  `log_info_trace` / `log_emit_trace` for correlation. No OTLP export.
- **MEM-3 region**: `let p = arena_alloc(a, n)` tags `p` with arena `a`;
  `arena_free(a)` invalidates all such locals (compile error on use).
- **cloudbaga**: POST gRPC method paths → `grpc_hello_response`; logs
  include `trace_id`/`span_id`; echoes `traceparent` on `/hello`.

### HTTP: binary Response body + H2 trailers (gRPC-native)
- `Response` gains `body_bytes`, `trail_ks`/`trail_vs`; helpers
  `http_response_bytes`, `http_set_trailer`, `http_body_len` /
  `http_body_as_bytes`.
- `http_respond_keepalive` and `h2_respond` prefer `body_bytes` (NUL-safe);
  H2 emits trailer HEADERS with `END_STREAM` when trailers are set (gRPC
  `grpc-status`).
- `pbbaga/grpc_unary`: `grpc_hello_response` / `grpc_to_response` for H2.

### MEM-3 arena seatbelt + gRPC unary glue + latency bench
- **MEM-3 (lite):** checker tracks `arena_free` like `drop` on the handle —
  double `arena_free`, `arena_alloc`/`reset` after free, and use of a freed
  handle are compile errors (reuses drop_log join). Runtime null-guards on
  alloc/reset. Full region tagging of arena payloads not claimed. Probes in
  `scripts/run_tests.sh`.
- **gRPC unary glue** (`pbbaga/grpc_unary.baga`): `grpc_hello_handle` +
  `grpc_write_response` (headers as str, body via `tcp_write_bytes` —
  frames start with 0x00 so they cannot live in `Response.body` str).
  Live test: `tests/grpc_unary_test.baga`.
- **Latency bench:** `bench/latency.baga` + `bench/run_latency.sh` —
  batch min/avg/max/p50/p99 via `monotonic_ms`.

### Storage — S8 txnbaga: 2PC coordinator + MVCC
- New package `app-product/txnbaga`: in-process **two-phase commit**
  (2 participants, channels) + **MVCC** i64 store (snapshot reads by
  `read_ts`, versioned commits).
- PREPARE locks + write-set; all-YES → COMMIT publishes versions; any NO /
  timeout → ABORT. Return-updated `tpc_put_ex` / `tpc_txn_ex` (cluster is
  by-value).
- Pure rules under `--verify`: `tpc_decide`, `mvcc_visible`, `lock_conflict`,
  `next_ts` (`examples/verify/tpc_decide.baga`).
- Tests: `tests/txn_test.baga` (MVCC snaps + 2PC multi-version + two-key).
  Spec/notes: `docs/superpowers/specs/2026-08-05-txnbaga-design.md`.
- Closes Track S sequencing step S8 (distributed transactions probe).

### Cloud — C6 relbaga + C7 flagbaga
- **relbaga** (C6): exponential backoff (`rel_backoff_ms`), `rel_retry` over
  `fn(i64)->i64`, circuit breaker (`brk_*` closed/open/half-open), bulkhead
  via channel tokens (`bh_new`/`acquire`/`try`/`release`). Tests:
  `tests/rel_test.baga`.
- **flagbaga** (C7): `--name value` / `--name=value` / bare bool flags,
  positionals; `flags_str`/`i64`/`bool`/`has`; `flags_parse` (process) +
  `flags_parse_vec` (tests). Tests: `tests/flag_test.baga`.

### Cloud — C5 pbbaga: protobuf wire + gRPC framing
- New package `app-product/pbbaga`: Protocol Buffers wire codec (varint,
  fixed32/64, length-delimited strings/bytes), zigzag `sint64`, skip
  unknown fields, hand-built field helpers (no protoc).
- gRPC length-prefixed messages: `[0][BE32 len][payload]` via
  `grpc_encode` / `grpc_decode`; example `HelloRequest`/`HelloReply` +
  pure `hello_rpc`. Full H2 transport remains host glue on httpdbaga.
- Tests: `tests/pb_test.baga` (varint 300 golden, cyrillic, skip,
  fixed, zigzag, neg int64, frame, protoc string golden `testing`).
- Spec: `docs/superpowers/specs/2026-08-05-pbbaga-design.md`.

### Cloud — C1–C4: signals, metrics, logs, cloudbaga demo
- **C1 signals** (builtins): `signal_watch` / `signal_check` / `signal_clear`
  / `signal_wait` / `signal_raise` — process-global slot for graceful
  shutdown (SIGTERM/SIGINT). C backend + `libbaga_par.so` (LLVM). Tests:
  `tests/std/signal_test.baga`.
- **C2 metbaga**: Prometheus text exposition — counters, gauges,
  `met_render` for `GET /metrics`.
- **C3 logbaga**: JSON lines on stderr (`ts`, `level`, `msg`, optional
  `req_id`) via `std/json` escape.
- **C4 cloudbaga**: 12-factor demo service — `/healthz`, `/readyz`,
  `/metrics`, `/`; poll accept loop observes signals and flips readiness.
- Docs: language §19 builtins; cloud direction plan C1–C4 marked shipped.

### Consensus — raftbaga (S7): leader election + log replication
- New package `app-product/raftbaga`: **3-node in-process Raft** over CSP
  (`go` / channels / `chan_recv_timeout`). No shared mutable state between
  nodes; messages are nested `cell2` trees (M17 packing).
- Election (staggered timeouts), heartbeats, single-entry AppendEntries,
  majority commit, apply to per-node `Map<i64,i64>`.
- Client: `raft_start` / `raft_put` / `raft_get` / `raft_stop` — tries nodes
  one-by-one so only the leader appends.
- Pure decision rules in `rules.baga` with specs; `--verify` on
  `examples/verify/raft_term.baga` proves term adoption, log up-to-date,
  majority (and reports honest UNKNOWN on thinner fragments). **Full Raft
  safety is not claimed.**
- Tests: `tests/raft_test.baga` (rules + live cluster put/get/multi).
  Demo: `app-product/raftbaga/demo.baga`.
- Spec/plan: `docs/superpowers/specs/2026-08-05-raftbaga-design.md`,
  `docs/superpowers/plans/2026-08-05-raftbaga.md`.

### Storage — lsmbaga MVP (S5+S6): page cache + WAL → memtable → SSTable
- New package `app-product/lsmbaga`: durable LSM-style KV on **RESP2**
  (reuses `kvbaga/resp.baga` so redis-cli works for the supported set).
- **S5 page cache** (`page.baga`): fixed 4 KiB pages, clock eviction,
  dirty writeback, invalidate-on-unlink; composite key
  `file_id * 1e9 + page_no`.
- **S6 engine** (`engine.baga` / `wal.baga` / `sstable.baga`):
  crc32c WAL records → memtable; flush to sorted `BAGASST1` SSTables +
  MANIFEST; compaction-lite when `gens >= compact_at`; recovery =
  MANIFEST + SST gens + WAL replay. Path **prefix** layout
  (`<dir>.wal`, `<dir>.sst.<gen>`, `<dir>.manifest`).
- RESP: `PING SET GET DEL EXISTS INCR KEYS DBSIZE SAVE QUIT`; honest
  ERR for TTL/EXPIRE/SET EX. Env: `LSMPATH`, `LSM_FLUSH_AT`,
  `LSM_COMPACT_AT`.
- **std/os** helpers for the engine: `mkdir`/`unlink`/`link`,
  `fs_rename` (link+unlink — raw `rename` collides with `stdio.h`),
  binary-safe `fd_write_bytes` / `fd_pwrite_bytes` / `fd_pread_bytes`
  (tcp idiom: explicit length, embedded NUL ok).
- Tests: `tests/std/os_fs_test.baga` (incl. mid-buffer `0x00`),
  `tests/lsm_test.baga` (flush, tombstone, reopen recovery, compact,
  RESP in-process + TCP loopback durability). Demo:
  `app-product/lsmbaga/demo.baga`.
- Spec/plan: `docs/superpowers/specs/2026-08-05-lsmbaga-design.md`,
  `docs/superpowers/plans/2026-08-05-lsmbaga.md`.

### Language — drop + checker-enforced memory discipline (MEM-1/2)
- `drop(x)` frees a let-bound local's heap blocks **now**: deep free for
  `Vec` (element boxes for bytes/struct elems, data buffer, struct), `Map`
  (pv boxes for boxed struct values, entries, buckets, struct), `bytes`
  (data buffer), and `fn`
  (the malloc'd `(code, env)` cell handle — the closure env box stays in
  the arena, shared ownership, documented).
- Checker seatbelt (all compile errors): use after drop
  (`използване на 'x' след drop`), double drop, drop of a parameter,
  drop of a lambda-captured variable (inside or outside the lambda),
  drop inside a loop of a variable declared outside it, drop of
  `str`/scalars, drop of a non-local expression. Branch-join semantics are
  certainties only: a variable is definitely-dropped after an `if` only
  when dropped on ALL arms; maybe-dropped use after the join is allowed.
- Runtime: 16 B-granularity size-classed free list for blocks ≤ 1024 B in
  `baga_alloc` (padded allocations, serialized by the existing pthread
  mutex — `go`-safe). Reclaim proof: a 1M-iteration alloc+drop loop peaks
  at ~6.2 MB maxrss vs ~87.6 MB without `drop`.
- MEM-2 (`--verify`): `HK_DROP` ghost state keyed by source variable,
  registered at `let x = vec_new()/map_new()/bytes_new(...)`; use-after-drop
  or double drop on a live path is **ОБРОЧЕНО (REFUTED)** with a witness
  (`examples/verify/mem_drop.baga`); aliasing and fn-value drop are silent
  no-claim paths; fragment gating as M14.
- Honesty boundary: assignment revival stays an error (`drop(v); v =
  vec_new(); use(v)` — conservative v1); aliasing is the programmer's
  contract (`let y = x; drop(x); use(y)` NOT diagnosed — the checker
  tracks variables, not heap graphs); blocks > 1024 B not reclaimed;
  historical `vec_grow`/`map_rehash` garbage stays; scope-exit leaks are
  NOT diagnosed (no warning severity — MEM-3 territory); bytes/str inner
  buffers of freed boxes stay in the arena; LLVM backend honestly
  `unsupported`.
- Tests: `tests/std/drop_test.baga` (17 checks) + reclaim probe, 10
  probes (8 negative + 2 positive-join) in `scripts/run_tests.sh`,
  `examples/verify/mem_drop.baga`. Spec:
  `docs/superpowers/specs/2026-08-05-mem-drop-design.md`; docs §12.8.

### Storage foundation (S2–S4): bytes mutators, positioned IO, crc32c
- **S2 — `bytes` mutators** (httpdbaga **G9**): `bytes_new(n)` returns a
  fresh zeroed buffer (`n < 0` clamps to 0); `bytes_set(b, i, v)` is a
  bounds-checked write (`baga: bytes_set: индекс N извън границите [0, L)`,
  `v` masked to a byte) that **mutates the shared buffer** — aliases see
  the write (Vec/Map semantics); `bytes_push(b, v)` returns a **new**
  `bytes` of length `len+1` (the source is untouched, O(n) copy per push —
  fine for frame building). C backend only; the LLVM backend honestly
  reports `unsupported`.
- **S3 — positioned IO** (`std/os`): new externs `pread`/`pwrite`/
  `fsync`/`fdatasync`/`fallocate` (!IO, i64/str params) plus wrappers
  `fd_pwrite(fd, data, off) -> 0/-1` (partial-write loop) and
  `fd_pread(fd, n, off) -> str` (heap buffer, `""` at EOF/error) — neither
  moves the fd's file position. `std/net/tcp.baga` now externs
  `pread64`/`pwrite64` (glibc weak aliases of the same symbols): two
  same-named body-less externs can't coexist — both prototypes are emitted
  unmangled and gcc fails with "conflicting types".
- **S4 — CRC-32C** (`std/crypto/crc32c.baga`): Castagnoli (reflected poly
  0x82F63B78) over native `bytes`, masked-i64 u32 — `crc32c_update` /
  `crc32c_final` / `crc32c_b`, incremental chaining; validated against the
  published iSCSI vector set.
- Tests: `tests/std/bytes_mut_test.baga` (8 checks), `tests/std/os_io_test.baga`
  (11 checks), `tests/std/crc32c_test.baga` (10 checks) + an S2 OOB negative
  probe (`bytes_set` out of bounds). Spec: `docs/superpowers/specs/2026-08-05-storage-foundation-design.md`;
  parent plan `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md`
  (Track S step 1).

### Language — sum types (L3): payload enums + full match
- Enums can carry a payload per variant — real sum types:
  `enum Res { Ok(i64), Err(str) }`, constructed as `Ok(42)`; payload-less
  variants stay bare (`None`). The type is nominal (`TYPE_ENUM`, not
  `i64`) — a `Res` no longer passes where an `i64` is expected.
- Checker: variant names of sum enums are globally unique across the
  program and may not collide with function names (`повторена дефиниция на
  вариант`); constructor arity and payload type are checked; `match` on a
  sum enum takes `Variant(binding)` / `Variant` / `_` patterns, must be
  **exhaustive** (the error names the missing variant), and all arms must
  agree in type. Non-enum matches keep first-arm-wins with no
  exhaustiveness.
- C backend: tagged struct + payload `union` + a `static inline`
  constructor per variant. LLVM backend: honest `unsupported` pointing at
  docs §11.
- Honest v1 limits: no `Vec<sum enum>` / `Map<K, sum enum>` (existing
  `неподдържан елементен тип` error), no generics (write a concrete enum
  per use site), exactly one payload type per variant (use a struct for
  more), sum enums can't be struct fields yet (C typedef order), and an
  enum payload must be declared before the enum that uses it.
- Bug fixed alongside: bare-expression match arms in `-> void` functions
  were wrongly checked against the enclosing fn's return type.
- Unblocked gaps: jsonrpcbaga **R1**, tplbaga **P2**, bagadecimal **D4**,
  oauthbaga **O4** (migrations of the stand-in structs optional).
- Tests: `tests/std/sumtype_test.baga` (15 checks) + 8 negative probes in
  `scripts/run_tests.sh`. Spec:
  `docs/superpowers/specs/2026-08-05-l3-sum-types-design.md`.

### Language — function values & closures (L5)
- Functions are first-class values, typed `fn(T, ...) -> R` (effects
  allowed: `fn(i64) -> i64 !IO`), usable in locals, parameters, return
  types, and as `Vec`/`Map` elements — method tables work
  (`Map<str, fn(str)->str>`).
- **Named references** (`let f = add`) and **lambdas with explicit
  by-value captures** (`fn [a, b] (x: i64) -> i64 { ... }`; later
  mutation of the source doesn't propagate, `Vec`/`Map` captures share
  the reference). Closures can be returned from functions (env box lives
  in the arena).
- C backend: a fn value is an i64 `(code, env)` handle over the par
  runtime's `cell2`; every user fn gets a `__clo` wrapper; lambdas
  compile to a synthetic env struct + wrapper, emitted before the fn
  bodies via memstreams (no AST pre-pass). Calls go through a statically
  typed function-pointer cast.
- Checker: structural `type_eq` for fn types; effect contract at wrap
  time (the value's effects must fit the annotation); **fn-typed locals
  may not shadow global functions** (keeps `--verify` sound — calls
  through values are opaque to it, honest skip); clear errors for
  calling non-functions and for ambiguous fn references (L6 rule:
  caller's own module wins).
- LLVM backend: honest `unsupported` for fn values.
- Unblocked gaps: jsonrpcbaga **R2**, tplbaga **P1**, testbaga **T3**
  (migrations optional; the probe tests live in
  `tests/std/fnval_test.baga`, 18 checks + 4 negative probes).
- Docs: §12.6 in both languages; spec in
  `docs/superpowers/specs/2026-08-05-l5-closures-design.md`.

### TLS 1.3 — TLS_AES_256_GCM_SHA384 (0x1302) negotiated end to end
- New `std/crypto/sha512.baga`: SHA-384/SHA-512 (FIPS 180-4) with 64-bit
  words as hi/lo 32-bit halves — no intermediate exceeds 2^33, the same
  signed-i64 discipline as the rest of std/crypto. FIPS vectors incl.
  the 56-byte and 1,000,000-'a' cases.
- `hmac_sha384_b` (block 128, 48-byte MAC) and `hkdf384_extract/expand`
  (RFC 5869; empty-salt → 48 zeros rule). Vectors from python
  hashlib/hmac computed offline (`tests/std/sha512_test.baga`, 13
  checks).
- `std/net/tls.baga` is suite-parameterized: `TlsSchedule.cipher`
  selects the transcript hash, HKDF/HMAC flavor, HashLen (32/48) and key
  length (16/32) everywhere — schedule, flight decrypt, CertificateVerify
  input, Finished HMACs, application secrets. The gate accepts both
  suites; the ClientHello already offered both.
- Live proof: `scripts/run_tests.sh` runs the openssl peer a **third**
  time with `-ciphersuites TLS_AES_256_GCM_SHA384`; the handshake test
  asserts the negotiated suite via `TLSCIPHER` env.
- **L6 correction found by this work**: sha512.baga's `u32` collided
  with sha256.baga's and every *internal* call turned ambiguous. The
  unqualified resolution now prefers the **caller's own module** (the
  main file is just the common case) instead of the main file only.

### wsbaga — fragmented message reassembly (W2 closed)
- New `ws_read_message`: reassembles continuation frames into one message
  (original opcode, full payload, 64 MiB cap kept). Control frames
  (ping/pong/close) are delivered immediately even mid-message (RFC 6455
  §5.4); a lone continuation or a new data frame mid-message is a
  protocol violation (`bad=1`). `WsConn` gains the accumulator state
  (`frag_op`/`frag`); `ws_read_frame` stays the frame-level API.
- `ws_handle_conn` (echo server) now serves fragmented messages instead
  of closing on opcode 0.
- `tests/ws_test.baga`: fragmented echo (UTF-8 split across frames),
  ping interleaved mid-message, lone-continuation close on a fresh
  connection (the accept loop is serial).

### Language — namespaces (L6): module-qualified calls
- Every imported file is a **module** named by its basename
  (`std/net/http_client.baga` → `http_client`); its functions are
  callable qualified: `http_client.http_get(url)`. Same-named functions
  in different modules are now legal — each decl gets a unique internal
  symbol `module.name`, so the gcc "redefinition" wall is gone.
- Unqualified resolution: the **current file's own** definition wins,
  then the single module defining the name, otherwise a compile error
  (`нееднозначно извикване на 'who' — има я в модулите 'alfa' и 'beta';
  уточни с alfa.who или beta.who`). A local variable named like a module
  shadows it (field access unaffected).
- Same-module duplicates are now a **checker** error (`повторна
  дефиниция на функция`, was: gcc redefinition); forward declaration +
  one implementation stays legal (self/compiler.baga relies on it).
- Clear errors for `unknown.module(...)` and `module.missing_fn(...)`.
- Mechanism: `SrcPos.file` — token/node origin survives the textual
  import expansion (this also unlocks G13 error attribution later).
  Fixed a **latent use-after-scope**: imported files were lexed with a
  filename pointing at the `resolved` stack buffer in `collect_tokens`.
- Scope honestly documented: structs/enums and `go` worker references
  stay global; no `import ... as` alias yet.
- Tests: `tests/ns_mods/{alfa,beta}.baga` + `tests/std/ns_test.baga`
  (qualified calls, main precedence, shadowing) + 4 negative probes in
  `scripts/run_tests.sh`. Docs: §18.1 in both languages.

### Language — `Map<K, struct>` (the last piece of L4)
- Map values may now be struct types — in annotations (`Map<str, Sess>`)
  and in fix-on-first-use inference; struct value types compare **by
  name** (`vec_elem_eq` reused; `type_eq`'s Map branch too).
- C backend: one box per entry (`void *pv` on `baga_MapEntry`, stable
  across rehashes); `baga_map_{set,get}_{str,i64}_box` runtime helpers.
  `map_set` copies in, `map_get` copies out — same semantics as
  `Vec<struct>`.
- **Missing key → field-wise zero struct** (new `emit_zero_struct` in
  codegen): `str` fields come out as `""` — not NULL — so printing a
  missing entry is safe; nested structs recurse; `bytes` zeroed.
- Runtime hardening (needed by the zero struct): `vec_len`/`map_len`
  tolerate NULL and return 0.
- Tests: `tests/std/map_struct_test.baga` (25 checks: copy in/out,
  shared reference fields, missing-key zero struct, del, i64 keys,
  inference, 500-entry rehash) + a negative probe (`Map<str,A>` set `B`)
  in `scripts/run_tests.sh`.
- Gaps closed: oauthbaga **O3**, httpdbaga **G14** — the whole
  "no struct values in containers" class is now gone from the language.
- Docs: `docs/language-{en,bg}.md` §12.5 (struct values + zero-struct
  semantics).

### Language — `Vec<struct>` (L4 closed for struct elements)
- `Vec<T>` element kinds now include **struct types** — in annotations
  (`Vec<Line>`, `[Line]`) and in fix-on-first-use inference. Element
  equality compares struct types **by name**: pushing a different struct
  (or a scalar) into a typed vector is a compile-time error
  (`vec_elem_eq`; `type_eq`'s Vec branch fixed to match).
- C backend: struct elements are **boxed copies** — generic
  `baga_vec_{push,get,set,slice,concat}_box` runtime helpers take the
  element size from the call site (`sizeof(b_T)`); push/set wrap the
  rvalue in a statement expression to get an lvalue. `vec_get` copies
  out by value: mutating the result does not touch the vector, while
  reference-typed fields (`Vec`/`Map`) stay shared — the same semantics
  as plain struct assignment.
- LLVM backend: honest `unsupported` diagnostic (same stance as
  `Vec<bytes>`/`Map`). The `--verify` fragment already skips struct
  constructions honestly — no change.
- Real-world proof: `dec_sum_vec(Vec<Decimal>)` in bagadecimal closes
  gap **D7** (invoice-line sums). Gaps updated: tplbaga **P3** closed,
  oauthbaga **O3** half-closed (`Map<str,struct>` remains), httpdbaga
  **G14** narrowed to `Map` struct values, xmlbaga **X2** unblocked.
- Tests: `tests/std/vec_struct_test.baga` (20 checks: push/get/set,
  copy-in/copy-out semantics, shared reference fields, slice/concat,
  annotated + inferred parameters, growth) + two negative probes
  (`Vec<A>` vs `B`, struct vs scalar) in `scripts/run_tests.sh`.
- Docs: `docs/language-{en,bg}.md` §12.4 (element types + box/copy
  semantics).

### App products — xmlbaga (universal XML, apps-roadmap №11)
- New base package `app-product/xmlbaga`: **pull XML parser + writer**,
  quick-xml style — no DOM (L4: no struct elements in vectors; cursor
  state lives in reference-typed fields so `XmlParser` stays copyable).
- Events: OPEN / CLOSE / TEXT / EOF / ERROR. Self-closing tags emit
  OPEN+CLOSE; comments, PIs and the declaration are skipped; CDATA is raw
  TEXT; DOCTYPE skipped leniently. Full well-formedness errors:
  mismatched/unclosed tags, multiple roots, text outside root, duplicate
  attributes, `<` in attribute values, unknown entities, invalid/control
  char refs, unterminated markup (with byte offsets).
- Entities: builtin five + numeric char refs decoded as UTF-8. Raw names,
  no namespace resolution (X1, documented).
- Writer: text/attr escaping, **sorted (deterministic) attribute order**,
  `xml_elem` convenience, declaration helper; writer → parser round-trip
  in tests.
- CLI `demo.baga`: event dump + `ROUNDTRIP=1` re-emit mode.
- `tests/xml_test.baga` — 35 checks (events, all error paths, entities,
  CDATA, writer goldens, round-trip); sandak discovery builds it.
- Format-specific importers (bank camt.053, invoices) are deliberately
  out of scope — they belong to apps on top of this package.
- Honest gaps in gaps.md: X1 namespaces, X2 no DOM (L4), X3 DOCTYPE
  entities, X4 byte-lenient names, X5 per-char concat (G1 lineage).

### bagaDecimal 0.3.0 — rounding modes + scientific parse (P1 shipped)
- **Rounding modes:** one `dec_round_impl` behind five public entries —
  `dec_round_dp` (half-away, unchanged default), `dec_round_bankers_dp`
  (half-even), `dec_trunc_dp`, `dec_floor_dp`, `dec_ceil_dp`. Dropped
  digits tracked as most-significant-dropped + sticky; zero normalizes
  to +0.
- **Scientific notation:** `dec_parse` accepts `[eE][+-]?digits`
  (`1.23e4`, `1E-2`). Exact when representable; scale > 28 rounds
  half-away, mantissa overflow errors; exponents saturate at ±1000.
- **D6 closed with data:** worst-case `dec_div` (bit-by-bit multi-limb
  path) measured at ≈7 ms — fine for money workloads; Knuth divmod only
  if a bulk workload appears.
- `pg_cell_decimal_or_zero` now carries an explicit "display only, never
  post sums computed through it" warning (strict `pg_cell_decimal` is
  the posting path).
- Tests: `tests/decimal_test.baga` 59 → 86 checks (five modes × signs,
  bankers sticky/odd, exponent round-trips incl. exact 96-bit max,
  exponent error paths).
- Package version 0.2.0 → 0.3.0; PLAN/gaps/design-notes updated.

### bagaDecimal — signed-overflow fixes + mul rescue (P0.6)
- **BUG (crash).** `dec_limb_mul` accumulated 32-bit limb products +
  carry up to ~2^64 in signed `i64`; the sum wrapped negative, the
  arithmetic-shift carry followed, and the carry drain walked out of
  bounds. `dec_mul(3100000000, 3100000000)` aborted at runtime — the
  whole upper half of the 96-bit mantissa range was broken.
- **BUG (silent wrong digits).** `dec_limb_div_small` stepped in base
  2^32 (`rem * 2^32 + limb`) and overflowed for single-limb divisors
  > 2^31: `dec_div_scale(1, 3000000000, 28)` returned confidently wrong
  digits with `ok = 1`.
- **Fix:** the wide paths now run on 16-bit half-limbs — every
  intermediate stays below 2^48 (`dec_limb_mul`, `dec_limb_mul_small`,
  `dec_limb_div_small`). Documented invariant in `docs/design-notes.md`:
  no intermediate may reach 2^63. gaps.md D1 verdict revised.
- **`dec_mul` scale rescue (rust-decimal parity):** a product that still
  exceeds 96 bits after the scale-28 rescale now drops fractional digits
  (half-away on the most significant dropped) until it fits; an error is
  returned only when the integer part alone overflows. Was: loud error.
- **BUG (accounting).** `dec_percent_of` pre-rounded the rate to scale 8
  before multiplying — 33.3333333% of 999999999.99 posted 333333330.00
  instead of 333333333.00. Now exact product → one division by 100 → one
  rounding at the posting. `dec_with_percent` taxes the same rounded
  base it posts.
- Tests: `tests/decimal_test.baga` 40 → 59 checks — big-limb mul,
  192-bit rescale (scale 48 → rescued 19), true overflow stays loud,
  big-divisor div, precise percent, parse/round/-0/to_i64 edges.

### Compiler — non-ASCII string literals before a hex digit (C backend)
- **Bugfix.** `emit_c_string` emitted non-ASCII bytes as `\xHH`, and C hex
  escapes are greedy: a Cyrillic (UTF-8) literal directly followed by an
  ASCII hex digit (`"ел0"`, `"елa"`, `"елf9"`) fused into one invalid
  escape (`\xbb0`) — gcc warned "hex escape sequence out of range" and the
  string came out wrong. Bytes now emit as 3-digit octal (`\ooo`), which
  the standard bounds to exactly three digits.
- Regression probe in `scripts/run_tests.sh` (Cyrillic + `0`/`a`/`f9`).
- `app-product/httpdbaga/gaps.md` G14 brought up to date: `Map<K,V>` has
  shipped, the remaining parallel-Vec shape is the struct-element half
  (L4 lineage, tplbaga P3 / oauthbaga O3).

### Language — `Vec<bytes>` (L4 closed for the bytes element kind)
- `Vec<T>` element kinds now include `bytes` alongside `i64`/`str`/`f64` —
  in annotations (`Vec<bytes>`, `[bytes]`) and in fix-on-first-use
  inference (`vec_push`/`vec_set`). Mixing bytes with another element type
  is a compile-time error, same as the other kinds.
- C backend: elements are boxed `baga_bytes` behind a pointer per slot
  (the `Map` bytes-values precedent); `baga_vec_{push,get,set,slice,concat}_bytes`
  emitted in the runtime. Binary-safe: NUL/0xFF round-trips.
- LLVM backend: honest `unsupported` diagnostic for `Vec<bytes>` (same
  stance as `Map`) instead of a silent fall-back to the i64 helpers.
- `Vec<struct>`/`Vec<Decimal>` etc. remain outside the element whitelist —
  that part of L4 stays open (tplbaga P3, bagadecimal D7).
- Tests: `tests/std/vec_test.baga` (27 checks: round-trip, set, slice,
  concat, annotated parameter, unannotated inference, growth) via baga-test
  discovery; a negative `Vec<bytes>` + str push probe in
  `scripts/run_tests.sh` next to the Map mismatch probes.
- Docs: `docs/language-{en,bg}.md` §12.4 + error/builtin tables; the
  `std/net/tls.baga` flight reader keeps its flat u24 shape (comment
  updated — the format predates `Vec<bytes>` and tracks GCM sequence
  numbers by record order anyway).

## [0.8.0] — 2026-08-04

**Language + ecosystem + pure-Baga cryptography.** README opens with the
language story, the in-tree crypto stack (no OpenSSL at runtime), and the
`app-product` application ecosystem.

### jwtbaga — RS256/ES256 verify + hardening (full crypto stack)
- JWT verify now uses TLS crypto: **RS256** (`rsa_pkcs1_sha256_verify`) and
  **ES256** (`ecdsa_p256_verify_sha256_raw`, JWS R‖S format).
- Hardening: `jwt_alg`, reject `alg:none`, `jwt_verify_hs256` requires
  header `alg == HS256`; `jwt_time_ok` / `jwt_accept_hs256` for exp/nbf/iss/aud.
- `ecdsa_p256_verify_sha256_raw` added in `std/crypto/p256.baga` for JWT.
- HS256 sign/encode unchanged (`jwt_encode`). Asymmetric *sign* still out
  (no private-key API) — verify is enough for OIDC resource servers.
- `tests/jwt_test.baga`: golden HS256, none-attack, exp, RS256/ES256 vectors.

### bagaDecimal — decimal + Postgres NUMERIC (accounting)
- New `app-product/bagadecimal` (**bagaDecimal**), inspired by
  [paupino/rust-decimal](https://github.com/paupino/rust-decimal): 96-bit
  mantissa + scale under `src/` (`ops/`, `parse/`, `format/`, `round/`,
  `convert/`, `money/`, `pg/`, `math/`).
- Core: `dec_parse`, `dec_to_string`, `dec_add/sub/mul/div`, `dec_round_dp`,
  cmp/abs/neg, i64 convert → `DecResult`.
- **Accounting:** `dec_money`, `dec_as_money`, `dec_normalize`,
  `dec_percent_of`, `dec_with_percent`, `dec_sum2/3`.
- **Postgres NUMERIC** (text protocol, like rust-decimal db-postgres):
  `dec_to_pg` / `dec_from_pg`, `pg_param_decimal`, `pg_cell_decimal`,
  `pg_col_is_numeric` (OID 1700). Dependency: `pgbaga`.
- Tests: `tests/decimal_test.baga`, live `tests/decimal_pg_test.baga`
  (INSERT/SELECT/SUM numeric), `examples/money.baga` (27.26).

### TLS 1.3 client, T8 — `https://` + openssl mock (no real OAuth account)
- Application traffic secrets and `TlsConn` (`tls_connect`, seal/open,
  read/write) on pure Baga TLS 1.3.
- **Bugfix:** client Finished HMAC must cover the transcript through
  the server Finished. The T5 probe only checked outer record type ≠ 21
  and accepted encrypted `decrypt_error` alerts (outer type 23).
- `std/net/http_client`: `https://` URLs (port 443); same `http_get` /
  `http_post` / `http_request` API. Self-signed peers accepted (empty
  trust anchor). `!Random` on the client API for ephemeral key share.
- `tests/std/https_test.baga` — live GET against `openssl s_server
  -tls1_3 -www` with a fresh self-signed cert (mock; no third-party
  account). Wired into `scripts/run_tests.sh`.
- Closes oauthbaga gap O1 for the **client** half of G6.

### TLS 1.3 client, T7 — ECDSA-P256 CertificateVerify
- `std/crypto/p256.baga`: NIST P-256 field/point arithmetic on bn limbs;
  `ecdsa_p256_verify_sha256(qx, qy, msg, sig_der)` (SEC1, DER signatures).
- `std/crypto/x509.baga`: EC SPKI (prime256v1 uncompressed) and
  ecdsa-with-SHA256 cert signatures alongside RSA.
- `tls_verify_server`: `ecdsa_secp256r1_sha256` (0x0403) in addition to
  RSA-PSS (0x0804).
- `tests/std/p256_test.baga` — python cryptography vectors (~9 s).
- Live openssl peer runs **twice** in `scripts/run_tests.sh`: RSA-2048
  self-signed and ECDSA-P256 self-signed — both full handshakes green.
- AES_256_GCM_SHA384 still deferred (needs SHA-384 HKDF); servers keep
  picking AES_128_GCM_SHA256 from the ClientHello order.

### TLS 1.3 client, T6 — X.509 + RSA-PSS CertificateVerify
- `std/crypto/der.baga`: minimal DER walker (SEQUENCE / INTEGER / BIT
  STRING / OID, definite lengths).
- `std/crypto/rsa.baga`: `rsa_public` (s^e mod n via bn), PKCS#1 v1.5
  SHA-256 verify (X.509 cert signatures) and EMSA-PSS SHA-256 verify
  (sLen=32, MGF1-SHA256 — TLS `rsa_pss_rsae_sha256`).
- `std/crypto/x509.baga`: parse Certificate → TBS + RSA SPKI (n, e) +
  signature; self-signed and trust-anchor checks.
- `std/net/tls.baga`: `TlsFlight` exposes `cv_algo` / `cv_hash`;
  `tls_verify_server(flight, anchor_der)` trusts the leaf and verifies
  CertificateVerify. Empty anchor = accept self-signed (dev peer).
- `tests/std/rsa_pss_test.baga` — python `cryptography` golden vectors
  (PSS accept/reject + self-signed cert). Live openssl path in
  `tls_handshake_test` now asserts cert + CV.
- Honest gaps: no name constraints / time / revocation; ECDSA is T7.

### Toolchain / packaging — Makefile is C-only; tests via sandak + baga-test
- The root `Makefile` no longer embeds the regression suite (~670 lines of
  hand-listed package tests). It builds the C toolchain only (`baga`,
  `sandak`, optional `baga-llvm`, `libbaga_par.so`) and thin targets
  (`test`, `self`, `test-llvm`).
- `make test` → `scripts/run_tests.sh`:
  - **sandak** discovery — every `app-product/*/sandak.toml` and
    `apps/*/sandak.toml` is built (no hand-maintained package list);
  - **baga-test** discovery — every `tests/**/*_test.baga` (specials
    with env/peers: registry, oauth PG, TLS vs openssl);
  - **run_verify.sh** — the static `--verify` oracle (M0–M18).
- Closes the GitHub-linguist skew where Makefile looked like a large
  share of the repo; package work stays in the package system.

### TLS 1.3 client, T4+T5 — record layer, ClientHello, encrypted handshake
- `std/net/tls.baga`: TLS 1.3 client core — record layer
  (`tls_read_record`), ClientHello builder (x25519 key share,
  supported_versions, signature_algorithms), ServerHello parser, the RFC
  8446 §7.1 key schedule (`tls_schedule`), flight decryption
  (`tls_open_handshake`: multi-record, GCM sequence numbers, inner
  content-type stripping, transcript walk, server-Finished HMAC verify)
  and the client Finished builder (`tls_finished_record`).
- `tests/tls_handshake_test.baga` — a full live handshake against
  `openssl s_server -tls1_3` with a fresh self-signed cert, wired into
  make test: ClientHello → ServerHello → decrypt
  EncryptedExtensions/Certificate/CertificateVerify/Finished → verify the
  server Finished → send the client Finished and assert no alert.
- Scars, documented:
  - the "derived" key-schedule step takes the transcript hash of the
    empty message (SHA-256 of "") as context — not an empty context;
    validated against RFC 8448 known answers before the live server
    would decrypt anything;
  - the ServerHello key_share is a single KeyShareEntry (the list-length
    wrapper exists only in the ClientHello);
  - openssl splits the flight into separate records (EE | Cert | CV+Fin)
    and sends a middlebox-compat ChangeCipherSpec; the flight reader
    handles both;
  - `openssl s_server` without `-quiet` resets connections (stdin loop);
    the harness runs it with `-quiet < /dev/null`;
  - `Vec<bytes>` is not a supported element kind yet (L4) — the flight
    reader takes a u24-length-prefixed `bytes` instead.

### TLS 1.3 client, T3 — HKDF + AES-GCM (std/crypto)
- `hkdf.baga`: RFC 5869 HKDF-SHA256 (extract with the empty-salt →
  HashLen-zeros rule, expand); Appendix A cases 1–3 pass.
- `aes.baga`: AES-128/256 forward cipher (FIPS-197 C.1/C.3). The S-box
  is computed at expand time from GF(2^8) inversion + the affine map —
  no 256-byte literal to mistype. Decryption is deliberately absent:
  GCM needs only the forward direction.
- `gcm.baga`: AES-GCM AEAD — GHASH in GF(2^128) (right-shift form,
  R = e1‖0^120), CTR from J0 = nonce‖00000001 (12-byte nonces, the
  TLS 1.3 shape), 16-byte tags verified with ct_eq_b.
- `tests/std/hkdf_test.baga` + `tests/std/aes_gcm_test.baga`: vectors
  from RFC 5869, FIPS-197, and python `cryptography` generated offline
  (AES-256-GCM, 60-byte non-block PT + AAD, AAD-only, tamper/nonce/AAD
  rejection). Both in the make test std loop; ~0.5 s each.
- Scars, documented: the S-box affine rotations were first written as
  rotl 5/6/7 instead of 3/2/1, and ShiftRows filled its buffer in
  push order (a transpose) — both caught by the intermediate-state
  probes against the FIPS walkthrough, not by the end-to-end vector.

### TLS 1.3 client, T2 — std/crypto/x25519.baga (RFC 7748 ECDH)
- X25519 on top of bn.baga: clamped scalars, Montgomery ladder (bits
  254..0) with constant-time conditional swaps, field arithmetic mod
  2^255-19 with the 2^255 ≡ 19 fold, final inversion z^(p-2) over fmul.
  Little-endian encoding per the RFC. A full scalar multiply ≈ 0.1 s.
- `tests/std/x25519_test.baga` — RFC 7748 §5.2 (first iteration, k=u=9)
  and §6.1 (both public keys + shared secret, both directions); in the
  make test std loop. The 1,000-iteration vector is documented but kept
  out of CI.
- Scar, documented: the first fold dropped a carry that escaped limb 9
  (≥ 2^260 must fold by ×608 again) — (p-1)² came out p-607 instead of
  1, exactly one lost 608. Caught by the modexp-free field probes.

### TLS 1.3 client, T1 — std/crypto/bn.baga (fixed-width bignum)
- The milestone plan is `docs/superpowers/plans/2026-08-04-tls-client.md`
  (T1–T8; closes №10 P2 / gap G6 — the last production blocker).
- New `std/crypto/bn.baga`: unsigned bignum on 26-bit limbs in
  `Vec<i64>` — add/sub/cmp, schoolbook mul, **in-place** shift-subtract
  mod, modmul, left-to-right modexp, big-endian byte codec. Signed-i64
  discipline: widest RSA-2048 column stays below 2^59.
- `tests/std/bn_test.baga` — 19 golden-vector checks, oracle = python
  bigints computed offline: byte round-trips, 256-bit mul/mod/exp
  (NIST P-256 prime), and RSA-2048 modexp over the RFC 3526 group prime
  (e=65537 fast path + 512-bit exponent slow path, ~1.5 s measured).
- Design scar, documented: the first `bn_mod` rebuilt the shifted modulus
  per bit — thousands of arena allocations per mod, and the bump arena
  never reclaims, so the slow-path modexp was OOM-killed. In-place
  reduction fixed it (43 s OOM → 2.1 s green).

### std/net — URL percent-encoding (oauthbaga gap O2)
- New `std/net/url.baga`: `url_encode` / `url_decode` (RFC 3986 §2.1) —
  unreserved set passes through, everything else `%XX` per UTF-8 byte;
  decode is byte-exact (rebuilt through `bytes`, since `chr()` would
  UTF-8-encode values ≥ 0x80 and double-encode the stream) and lenient
  (malformed `%XX` and `%00` copy through literally — baga strings are
  C strings).
- `tests/std/url_test.baga` — 20 checks incl. Cyrillic/emoji round-trips;
  in the `make test` std loop.
- First user: oauthbaga percent-encodes `redirect_uri` in the authorize
  redirect and the provider decodes it.

### App products — oauthbaga worker model (O5 closed)
- PG mode is now fully concurrent: every HTTP connection runs on its own
  `go_bg` worker with its own DB connection (fmr legacy idiom); the CSRF
  authorize states moved to an `oauth_states` table (migration
  20260804102), so nothing is shared between threads. Dev (in-memory)
  mode stays serial.
- `oauth_pg_test` proves the cross-thread flow: the `/login` state row is
  written by one connection and consumed by another; the suite is now on
  the default ports (the workers rebuild config from env).
- `demo.baga` migrates once before booting the two nodes (two concurrent
  `migrate_up` would race on the version insert).
- Honest scar, recorded in gaps.md: the first bg handlers responded
  without `tcp_close(fd)` — with Connection: close the client reads to
  EOF, so an unclosed fd hangs clients until their read timeout.

### Compiler — call-site Vec/Map element inference (tplbaga P5 closed)
- **Soundness fix.** An unannotated `vec_new()` / `map_new()` passed to a
  typed parameter (`Vec<str>`, `Map<str,str>`, …) now gets its element
  kind fixed from the callee parameter at the call site — the same
  fix-on-first-use mutation `vec_push` already did. Before, a later
  `vec_get` fell into the historical i64 default and the codegen read
  `str` memory as `i64`: garbage output, no diagnostic, no compiler
  complaint. Found by tplbaga (№7) the way the roadmap intended.
- The reverse order (`vec_get` before the fixing call) is now a compile
  error instead of a silent wrong type.
- Two regressions in `make test` (vec + map call-site inference).
- `type_str`: rotating buffers — two `Vec`/`Map` types in one diagnostic
  no longer overwrite each other's text.

### Compiler — LLVM `ord` decodes UTF-8 (oracle parity)
- The LLVM backend's `baga_ord` returned the first *byte* (208 for "А");
  the C runtime decodes the code point (1040). Now both backends decode
  1–4 byte UTF-8 sequences; `examples/strings.baga` matches byte-for-byte
  and `make test-llvm` is fully green.

### App products — oauthbaga P1 (Postgres persistence, №10 "всичко заедно")
- The pgbaga/ormbaga leg of №10: `oauth_codes` / `oauth_refresh` /
  `oauth_sessions` tables (goose-style migrations, version 20260804101),
  `$N` parameterized queries only; `OAUTH_PG=1` + `PG*` switches the
  backend, in-memory maps remain the dev mode (both live-tested).
- Store ops meet at typed rows (`OaCode`/`OaTok`/`PxSess`); the JSON
  record codec is now contained to the in-memory backend (O3 shrunk).
- One DB connection per HTTP connection (fmr legacy idiom) — no struct
  rebinding through the handler chain, concurrency-safe, and the natural
  path to a worker pool (O5 mostly closed; the CSRF `states` map stays
  per-node for now, O7 notes the per-request SCRAM cost).
- `tests/oauth_pg_test.baga` — live full cycle on ports 18692/18693 plus
  DB-level proofs: the code row is consumed by the exchange, refresh
  rotation keeps exactly one live token, the session row appears on
  login and dies on logout. Wired into `make test` like registry_test
  (`OAUTH_PG=1 PGDATABASE=baga_oauth`).

### App products — oauthbaga (OAuth proxy, apps-roadmap №10 complete)
- New product `app-product/oauthbaga`: the integration exam — std HTTP
  client (№2) + jwtbaga + cookie sessions, pages rendered by tplbaga
  (first cross-product dependency in the series; №7 feeds №10).
- Provider node: `/oauth/authorize` (auto-approve dev profile, one-time
  codes), `/oauth/token` (authorization_code + refresh_token grants with
  rotation), `/api/me` (Bearer JWT guard); RFC 6749-shaped JSON errors.
- Proxy node: `/login` (CSRF state) → `/callback` (real server-to-server
  code exchange over the std client) → `sid` cookie session with
  transparent refresh → `/logout`.
- Two serial nodes (`cell2` ports, kvbaga idiom): the provider never
  calls itself, so no self-accept deadlock; state is `Map<str,str>` of
  JSON records (L4 stand-in); `TokenReply`/`PxTokens` continue the L3
  Result stand-in convention.
- `demo.baga` boots both nodes (`OAUTH_PORT`/`OAUTH_PROVIDER_PORT`/
  `OAUTH_SECRET`); a browser completes the flow on loopback.
- `tests/oauth_test.baga` — live full cycle: authorize → exchange →
  bearer → protected → refresh/rotation → browser flow → logout
  (47 checks); runs via `scripts/baga-test`.
- Probes: O2 no URL percent-coding in std (third hand-rolled query
  parse); O5 go_bg carries i64 only → serial nodes until state moves to
  Postgres (P1); TLS/G6 still the production blocker (O1).

### httpdbaga — 302 reason phrase
- `reason_phrase(302)` now says "Found" (was the generic "Status") —
  the OAuth redirects were the first 3xx on the wire.

### App products — tplbaga (HTML templates, apps-roadmap №7)
- New product `app-product/tplbaga`: mustache-ish subset — `{{ expr }}`
  escaped interpolation, `{{{ expr }}}` raw, `{% if %}` / `{% else %}` /
  `{% endif %}` (nestable, `!` negation), `{# comments #}`, filter chains
  `{{ v | trim | upper }}` (upper/lower/trim/len/default:arg, ASCII case).
- Tokens are prefix-encoded `Vec<str>` + a `Map<i64,i64>` jump table for
  block pairing — one iterative walk, no recursion (L4 stand-in); `TplOut`
  ok/err struct as the L3 stand-in; filters dispatch by name — the
  designated L5 (closures) probe.
- `demo.baga` CLI: template file + `key=value` data file (exit 0/1/2).
- `tests/tpl_test.baga` — 46 checks (escape, jump table, filters, error
  paths, realistic page); runs via `scripts/baga-test`.
- Probes: unannotated `vec_new()` passes the checker but codegen emitted
  i64 element access until annotated (P5, compiler bug candidate); `spec`
  keyword cannot be an identifier and the diagnostic doesn't say so (P6).

### App products — jsonrpcbaga (JSON-RPC 2.0, apps-roadmap №6)
- New product `app-product/jsonrpcbaga`: JSON-RPC 2.0 subset over HTTP —
  single/batch, notifications, standard error codes, methods
  `ping`/`add`/`echo`/`fail` via name switch.
- `RpcResult` struct as L3 Result stand-in; `rpc_handle_body` pure +
  `rpc_serve` accept loop; `tests/jsonrpc_test.baga` (pure + live HTTP).
- Gaps: no sum types (R1/L3), no function-value method table (R2/L5).

### App products — queuebaga (task queue, apps-roadmap №5)
- New product `app-product/queuebaga`: disk-backed jobs, `chan` of job ids,
  `go_bg` workers, reverse-payload demo work, `fail:` retry until max
  attempts, `q_wait` with timeout.
- Flat paths `<prefix>.<id>.{job,status,result,attempts}` (no mkdir).
- `tests/queue_test.baga` + demo. Gaps: i64-only chan/go (Q1), no setenv
  (Q2), write_file truncate races (Q3), no L5 handlers (Q5).

### App products — grebaga (grep-like CLI, apps-roadmap №9)
- New product `app-product/grebaga`: literal + mini-pattern (`.`/`*`/`\`),
  ASCII `-i`, streaming line scan (chunked read; empty line ≠ EOF), CLI
  `demo.baga` (`-n`/`-i`, files or stdin, exit 0/1/2).
- `tests/grep_test.baga` — match unit tests + live file stream.
- Probe: std `read_line` empty/EOF collapse → custom scanner (G1).

### App products — testbaga (test asserts + runner, apps-roadmap №8)
- New product `app-product/testbaga`: fail-fast `assert_true` /
  `assert_eq_i64` / `assert_eq_str` / `assert_ne_str`, plus `Suite`
  (continue-on-fail, `suite_finish` → exit code).
- `scripts/baga-test` — discovers `*_test.baga` and runs each via baga
  (shell driver: no readdir/process spawn in language yet).
- Dogfood: `tests/testbaga_test.baga`; `tests/std/sort_test` migrated off
  local `check`.
- Gaps: T1 process spawn, T2 list_dir, T3 function values (L5).

### App products — mdbaga (Markdown → HTML, apps-roadmap №4)
- New product `app-product/mdbaga`: CommonMark-ish subset — ATX headings,
  paragraphs, emphasis, inline/fenced code, ul/ol, blockquotes, hr, links,
  HTML escape; `md_to_html` / `md_to_document`.
- CLI `demo.baga` reads `arg(0)` via `read_file`, prints HTML (`MDDOC=1` for
  full document shell). Package: `sandak build`.
- `tests/md_test.baga` — escape, blocks, inline, XSS-ish `<` in text/code.
- Probe gaps: nested concat still dominates builders (M1 / G1); no
  file-exists vs empty distinction on `read_file` (M2).

### App products — chatbaga (WebSocket chat, apps-roadmap №3 complete)
- New product `app-product/chatbaga`: multi-room chat on a single-threaded
  `poll(2)` event loop — JSON join/msg over wsbaga text frames, room
  broadcast, leave notifications, error replies.
- Closes **W1 / K1** (serial accept): one poll set watches the listener +
  every client fd; connection state lives in `Map`s keyed by fd.
- Forced **`Map` bytes values** into the language (`Map<i64, bytes>` residual
  buffers) — also the path to close kvbaga K2 for binary store values.
- `demo.baga` standalone server (`CHATPORT`, default 16460); interop with
  `wscat` and raw RFC 6455 clients (UTF-8 text, multi-client broadcast).
- `tests/chat_test.baga` — 18 live checks (two clients, errors, room
  isolation, close/`left`); package via `sandak build` (app-product list).

### std/net — poll(2) event loop primitive
- `std/net/poll.baga`: `poll_wait(fds, timeout_ms)` / `poll_has` over
  SYS_poll (POLLIN|POLLERR|POLLHUP). Same memfd staging pattern as tcp.
- `tests/std/poll_test.baga` in the `make test` std loop.

### Language — Map bytes values (kvbaga K2 path)
- `Map<K,V>` values may be `bytes` (in addition to i64/str/f64). Checker +
  C runtime (`baga_map_*_bytes`); missing key → empty bytes.
- `tests/std/map_test.baga` covers NUL/0xFF round-trip through map values.

### App products — wsbaga (WebSocket, apps-roadmap №3)
- New product `app-product/wsbaga`: RFC 6455 — server handshake
  (`Sec-WebSocket-Accept`), frame codec (FIN/opcode, 7/16/64-bit lengths,
  client masking), text/binary/ping/pong/close handling, buffered
  `ws_read_frame`, echo server `ws_serve(port)`, and a masked client
  (`ws_client_connect` verifies the accept key).
- **Interop-verified**: `wscat` (Node.js) echoes UTF-8 text and 900-byte
  payloads against the Baga server; loopback `tests/ws_test.baga` covers
  all length boundaries (125/126/65535/65536), binary with NUL/0xFF,
  ping→pong, close→EOF (14 checks).
- Honest limits in gaps.md: serial accept closed by chatbaga (W1 = K1 →
  poll); no fragmented-message reassembly (W2) still open.

### std/crypto — SHA-1 (probed into existence by wsbaga)
- `std/crypto/sha1.baga`: RFC 3174, same shape as sha256 (Vec core +
  `bytes` wrappers: sha1/sha1_hex/sha1_b/sha1_b_hex).
- `tests/std/sha1_test.baga`: RFC vectors incl. million-'a' and the
  RFC 6455 accept-key vector; in the `make test` std loop.
- SHA-1 only for protocol mandates (RFC 6455); sha256 stays the default.

### apps/registry — пакетен registry за sandak (apps-roadmap №2, втора половина)
- New app `apps/registry`: JSON/HTTP package index on the fmrbaga/ormbaga/
  pgbaga stack — `GET /v1/packages[?q=]`, `GET /v1/packages/{name}`,
  `POST /v1/packages` (publish = upsert package + unique version; 409/422
  error shapes). Migrations create `reg_packages` / `reg_versions`.
- `sandak search [term]` / `sandak publish --git URL [--rev R] [--subdir S]
  | --path P` — the client is a Baga program (`src/sandak_registry.baga`)
  executed by sandak through the compiler, talking HTTP via the new std
  client. Registry URL from `SANDAK_REGISTRY` (default http://127.0.0.1:8090).
- `baga` CLI gained **program arguments**: `baga prog.baga arg1 arg2…` (and
  an explicit `--` separator) — everything after the input file reaches
  `arg()`/`arg_count()` of the compiled program. Before this, `arg()` had
  no way to receive values through compile-and-run.
- fmrbaga `jbody_parse_str` now rejects malformed bodies with
  `json_strict_valid` before the lenient parse (G13 in a real request path).
- `tests/registry_test.baga` — first full-stack live HTTP test: boots the
  server in a go_bg worker, drives it through std/net/http_client (18
  checks: publish/dup-409/show/index/search/404/400/422). In `make test`.

### std/net — HTTP/1.1 client (apps-roadmap №2, първа половина)
- `std/net/http_client.baga`: `http_request(method, url, headers, body,
  timeout)` + `http_get` / `http_post`. URL parse (http:// only — https
  waits for TLS), DNS hostnames through `tcp_connect_to`, `Map<str,str>`
  request/response headers (lowercased, case-insensitive lookup via
  `http_resp_header`), Content-Length + chunked bodies, read-to-close.
- First product of the map type in std itself: headers are `Map<str,str>`.
- `tests/std/http_client_test.baga` — 17 live loopback checks against an
  httpdbaga worker (GET/POST/UTF-8 bodies, chunked, 418, refused, bad URL);
  wired into `make test`.
- Gap found (L6): no namespaces — the client's `http_header` collided with
  httpdbaga's; renamed to `http_resp_header`. Prefix convention holds until
  module scope exists.

### Language — `main -> i64` exit code (kvbaga K3 closed)
- The C wrapper emitted `b_main(); return 0;`, swallowing the exit code of
  `fn main() -> i64`. Now `return (int)b_main();` for i64/i32 mains; void
  mains unchanged. The baga CLI already propagated `WEXITSTATUS`.
- Regression check in `make test`; kvbaga gaps.md K3 closed.

### App products — kvbaga (Redis-compatible KV server)
- New product `app-product/kvbaga`: a RESP2 KV server built deliberately on
  the new map type — the first "app as language probe" on `Map<K,V>`.
- `resp.baga` (pure RESP2 codec: buffered parse, reply builders, client
  round-trip), `store.baga` (`Map<str,str>` + `Map<str,i64>` deadlines,
  lazy TTL expiry), `server.baga` (serial accept loop for `go_bg`,
  idle `SO_RCVTIMEO` guard).
- Commands: PING, SET [EX s], GET, DEL, EXISTS, INCR, KEYS, EXPIRE, TTL,
  DBSIZE, QUIT — Redis-shaped errors (`-ERR`, nil bulks, arity checks).
- Honest limits logged in gaps.md (K1–K5): serial connections (`go()`
  carries only i64 — the store can't cross threads), text-only values,
  and the swallowed `main` exit code (K3 — repo idiom is `exit(1)`).
- Tests: `tests/kv_test.baga` — 27 live loopback checks; demo boots a
  worker and drives it. Both wired into `make test`.

### Language — `Map<K, V>` (first-class hash table)
- New type `Map<K, V>`: keys `i64`/`str`, values `i64`/`str`/`f64`/`bytes` —
  the same fix-on-first-use rules and annotations as `Vec<T>`; mixing key or
  value types is a compile-time error. (`bytes` values added with chatbaga.)
- Builtins: `map_new`, `map_set`, `map_get` (zero-value when absent),
  `map_has`, `map_del`, `map_len`, `map_keys` (→ `Vec<str>`/`Vec<i64>`).
- Maps are pointers: passing one to a function shares it (mutate-through,
  unlike by-value structs) — the natural store for servers and caches.
- C backend: chained hash table (`baga_Map`, FNV-1a / Murmur-mix hashing,
  grows at load factor 3/4). LLVM backend: honest "unsupported" diagnostic.
- Self-hosting parity unchanged (`make self` fixed point holds); the self
  compiler does not parse `Map` yet (documented limitation).
- Docs: `docs/language-{en,bg}.md` §12.5 + type/builtin tables.
- Tests: `tests/std/map_test.baga` (bytes + rehash growth) + two negative
  type-error checks wired into `make test`.

### std/net — production connects
- **DNS resolution:** `tcp_resolve_ipv4` — hostnames via `getaddrinfo`
  (AF_INET, `mem_read` pointer-walk through the `addrinfo` list); dotted
  IPv4 still short-circuits the resolver.
- **Timeouts:** `tcp_set_timeouts` (SO_RCVTIMEO + SO_SNDTIMEO) — a blocked
  read/write/connect fails instead of hanging forever.
- **Client tuning:** `tcp_set_nodelay` (TCP_NODELAY), `tcp_set_keepalive`
  (SO_KEEPALIVE); `tcp_connect_to(host, port, timeout_s)` wires all of it.
  `tcp_connect` keeps its classic behavior.
- New primitive `mem_read(addr, n)` — copy arbitrary process memory into a
  Baga `str` via memfd (with the offset reset; SYS_write advances it).

### App products — pgbaga (Postgres adapter)
- **Production connect:** `pg_connect_to(host, port, ..., timeout_s)` —
  hostname or IPv4, bounded connect/read/write; `pg_set_timeout` retunes a
  live connection; **`pg_cancel`** sends CancelRequest on a fresh connection
  using the BackendKeyData captured at startup.
- **JSON/JSONB tables end to end:** `pg_param_json` binds (`$N::json[b]`),
  column OID detection (`pg_col_is_json` / `pg_col_is_jsonb`), JSON cell
  accessors (`pg_cell_json` / `pg_cell_json_ok`), and validated literals in
  ormbaga (`sql_json` / `sql_jsonb`).
- `std/json`: new `json_strict_valid` — a strict RFC 8259 validator
  (the existing `json_parse` stays lenient for recovery).
- Typed getters: `pg_cell_bool`, `pg_cell_f64`; transaction wrappers
  `pg_begin` / `pg_commit` / `pg_rollback`; structured error accessors
  `pg_sqlstate` / `pg_err_message`.
- `PgReader` now lives inside `PgConn` — buffered socket state survives
  across queries (gap G9 closed; ground for LISTEN/NOTIFY later).
- Hardening: `pg_read_msg` rejects message lengths outside `[4, 2^30-1]`.
- `tests/pg_test.baga`: live JSON table round-trips + strict harness
  (a FAIL now exits 1 instead of printing "all passed"); 70 checks.

### Packages — sandak (пакетна система)
- New tool `sandak`: `sandak.toml` manifests, path + git dependencies
  (with `subdir` for monorepos), `sandak.lock` with `--locked`, and
  `fetch`/`build`/`run` commands. Zero dependencies (libc + git + gcc).
- Compiler: repeatable `-I <dir>` import search path flag.
- The whole monorepo is packaged: `std`, `app-product/*`, `apps/api` have
  manifests; imports are package-named (`import "fmrbaga/app.baga"`).
- Docker: multi-stage `Dockerfile` + `docker-compose.yml` — point `APP_REPO`
  at a git URL and the container clones toolchain + app + deps and builds.

## [0.7.0] — 2026-08-02

Second tagged release: M14–M18 static verification, soundness fixes, evaluation
and research docs. CLI: `baga --version` / `-V` prints `baga 0.7.0`.

### Static verification — M18: `!Overflow` as an effect (effect system ≡ verifier)
- Arithmetic safety (M15) is now a **type-level effect**. `!Overflow` is a
  permission (like `!IO`), not a claim: the M15 kind-4 obligations are the
  *effect inference* for `!Overflow`, and the one-way effect check is the
  *discharge*. The effect system and the verifier become one judgement.
- A function **without** `!Overflow` claims overflow-safety; `--verify`
  proves it (`ефект !Overflow: безопасна — типът е точен`), refutes it with a
  concrete witness when it overflows (undeclared overflow ⇒ nonzero exit), or
  honestly reports НЕ МОГА ДА РЕША.
- A function **with** `!Overflow` is discharged: the overflow is still printed
  as evidence, but it is no longer a contract violation and does not fail
  verification (`ensures` verdicts are idealized-ℤ-only). Over-declaring
  `!Overflow` on a provably-safe function is allowed (noted as redundant).
- `!Overflow` propagates through calls via the generic effect merge — a caller
  must declare or catch it ("необработен ефект !Overflow"); no checker change
  was needed.
- The fragment gate now admits `{Par, Overflow}` (`ret_has_unverifiable_effects`);
  functions with other effects still skip honestly and make no overflow claim.
- The M15 exit-flag rule is gated: a REFUTED arithmetic obligation fails
  verification only when the function does not declare `!Overflow`. No
  existing example declares `!Overflow`, so all prior exit codes are unchanged.
- `--verify --json` adds an `overflow_effect` field
  (`{analyzed, declared, safe, result, witness}`); `--proofs` emits a
  `theorem <fn>_overflow_safe`.
- Examples: `examples/verify/ovf_eff_{safe,refuted,declared,unknown,redundant,skip,propagate,propagate_ok}.baga`.
- Notes: `docs/thesis-m18-overflow-effect.md` (the culmination),
  `docs/thesis-open-problems.md` (liveness / full BV / rich polynomials),
  `docs/thesis.md` (binding research monograph).
- Doc seriousness pass: research monograph/notes without degree theatre;
  proof sketches vs LA certificates; CLI/`--verify` recursion claim;
  self-host LOC (~2660); STLC SN not claimed for full Baga; theory placement
  among tools instead of curriculum comparisons.

### Static verification — M17: pair abstraction (`cell2` + channel pair APIs)
- `cell2(a,b)` / `cell2_0(p)` / `cell2_1(p)` are exact rewrites in the
  verifier (`cell2_0(cell2(a,b)) = a`) — allowed anywhere, including inside
  conditions (`if cell2_0(r) == 1`).
- The pair-returning channel APIs are now in the fragment with ranges for
  the status component and M16 content axioms for the value component:
  - `chan_recv2` (ok ∈ [0,1]), `chan_try_recv` / `chan_recv_timeout`
    (status ∈ [0,2]), `chan_select2*` (which ∈ [0,3]; value gets only the
    axioms BOTH channels share).
  - `select2_wait`'s which ∈ {0,1,3} is modeled as the interval [0,3]
    (over-approx; the abstract status keeps refutations honest).
- `go(worker, cell2(a, b))`: packed arguments work; a worker's
  `requires cell2_1(p) >= 1` is discharged at spawn where the pair's
  components are visible. Inside the worker, packed params stay honestly
  opaque.
- Examples: `examples/verify/pair_{recv2,select,go}.baga`.
- Note: `docs/thesis-m17-pairs.md`.

### Static verification — M16: channel content invariants (rely–guarantee)
- New statement-level annotation `invariant <expr>` (contextual keyword):
  - `invariant c[*] >= 1` — "every payload sent on channel `c` satisfies the
    predicate", anchored on the channel's resolved symbolic var (aliases work).
  - scalar form (no `[*]`) acts as `assume` — the path gains the constraint.
  - `chan_send` discharges the predicate (else the axiom is dropped, M3
    rule); `chan_recv` instantiates it on the result.
- Cross-thread: a worker's `requires c[*] ...` is discharged against the
  caller's axioms at `go` spawn (kind-2 obligation, provable); a worker
  without matching requires drops them at spawn — honest, never unsound.
  The same discharge/drop rules apply at plain M5 calls.
- `go` workers may now declare `Par` effects (channel-using workers were
  previously outside the fragment; non-`Par` effects still skip).
- Examples: `examples/verify/chan_inv{,_bad,_par,_escape}.baga`.
- Note: `docs/thesis-m16-channel-invariants.md`.

### Static verification — M15: arithmetic safety (the ℤ-vs-i64 bridge)
- New kind-4 obligations: every `+ - * -x / % <<` in verified code gets a
  verdict — ДОКАЗАНО (cannot overflow on this path), ОБРОЧЕНО with a concrete
  large-magnitude witness (e.g. `abs(INT64_MIN)`, `n + 1` at `n = INT64_MAX`,
  `n / m` at `m = 0`), or honestly НЕ МОГА ДА РЕША.
- Exact bound search over the FM core (binary search on feasibility);
  products use tightest provable |factor| bounds, compared in `__int128`.
- When all arith obligations of a function are proven, the idealized-ℤ model
  and the i64 runtime coincide — the output says so; otherwise it marks the
  ensures verdicts as idealized-model-only. JSON: `"arith": [...]`.
- The extreme window (2^62, 2^63) reports UNKNOWN, never a false proof.

### Soundness fixes (found by M15)
- **M1 loop havoc**: variables assigned/let-bound in a `while` body are now
  havoced before the invariant is assumed (head + post-loop states). Before,
  the post-loop state kept stale pre-loop values, making invariants vacuous —
  a loop returning `-n` was falsely ДОКАЗАНО for `output >= 0`. Now honestly
  UNKNOWN unless the invariant really covers the variable
  (`examples/verify/loop_havoc.baga`).
- **Rational core**: `rat_add/rat_mul/rat_mk/v_gcd/rat_neg` are now
  INT64_MIN-safe (`__int128` intermediates); `fm_sat` bails out conservatively
  (SAT = "cannot decide") on overflowed constraints.

### Static verification — M14: `!Par` enters `--verify`
- Functions whose only effect is `Par` are now verifiable (other effects
  still skip honestly).
- **Fork–join determinism:** for a pure verifiable worker `f`,
  `join(go(f, x)) ≡ f(x)` — the worker spec applies via M5 assume–guarantee
  (requires discharged at spawn, ensures assumed for the join result).
- **Handle protocols:** ghost state per symbolic handle —
  `spawn → join | detach`; join/detach after consume is REFUTED with a
  counterexample (join-after-detach is fatal at runtime). Channels track
  open/closed; `send` on a known-closed channel is provably `-1`.
- New JSON field `"protocol"` for kind-3 obligations.
- Boundary (honest skips): pair-returning builtins (`chan_recv2`,
  `chan_try_recv`, `chan_select2*`), mutexes, `pool_map`, effectful workers.
- Examples: `examples/verify/par_{join,join_bad,detach_bad,chan}.baga`.
- Note: `docs/thesis-m14-par-fragment.md`.

### Proof extraction
- `--proofs` now prints the verifier's established facts, not just heuristics:
  - `_terminates` uses the real verdict — recursion with a proven `decreases`
    measure is reported as full correctness; otherwise honestly partial.
  - while-loop invariants appear as `lemma <fn>_invariant_<k>` with their
    Hoare status (init + preservation proven, or honestly unproven → UNKNOWN).

## [0.2.0] — 2026-08-02

First tagged release after the static-verification arc and theory write-up.

### Static verification (`--verify`)
- **M0–M7** — linear i64 paths, while invariants, bounds, element axioms,
  assume–guarantee recursion, `decreases` termination, integer tightening
- **M8–M12** — product symbols, sign table, const/var div–mod, floor mul,
  complete square, AM-GM identity, conclusiveness gate (no false alarms)
- **M13** — products inside `if`/`while` guards; sound bitwise envelope
  (`| & ^` neutrals, `n&1∈{0,1}`, `<<`/`>>` special cases)

### Concurrency & backends
- `!Par`: `go` / `join` / channels / select wait–timeout
- LLVM `!Par` parity via `libbaga_par.so`

### Docs
- `docs/theory-{en,bg}.md` — Fourier–Motzkin, Farkas, ℤ-tightening, M0–M13
- `docs/thesis-m13-nonlinear-fragment.md` — research note

### CLI
- `baga --version` / `-V` prints `baga 0.2.0`

## [0.1.0] — unreleased baseline

Bootstrap compiler, self-hosting, effects, specs runtime, std library, playground.
