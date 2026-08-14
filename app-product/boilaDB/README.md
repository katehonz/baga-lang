# boilaDB

Multimodal database in **baga**: a modular monolith over sharded
rocksbaga (LSM), **PostgreSQL-compatible SQL dialect** (BoilaSQL),
high load as the goal. **Encoding: UTF-8 only.** Geo/GPS is explicitly
out of scope.

- **Architecture:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **Plan:** [PLAN.md](PLAN.md) (P0–P11, each phase with a benchmark gate)
- **Limitations:** [gaps.md](gaps.md)

## Status

**All phases P0–P11 landed.** Core SQL + multi-database server, PG wire
protocol v3 (real `psql`/libpq compatible, port 6575), FTS (BM25 +
phrase), vector (HNSW kNN), time-series (TTL + `time_bucket`), graph
(`WITH RECURSIVE` BFS/DFS/Dijkstra), hardening (query budget, full DDL,
users/ACL, `EXPLAIN [ANALYZE]`). Concurrent HTTP + PG server: bounded
`BOILA_WORKERS` pool (default 4; `=0` → `go_bg` per conn), per-shard
hop-less stores, shared per-db plan cache; DDL is serial per db
(gaps W1/P11-1). No SCRAM/TLS (gaps W6). Measured @10k ladder: point
156k ops/s, insert 524 ops/s, mix 2.6k ops/s
(`bench/boila/results/harness-2026-08-09.md`).

**ormbaga live (2026-08-13):** `tests/orm_boila_test.baga` **36/36**
срещу `serve_pg` (клиент и сървър с и без `--rc`). Вж.
`bench/boila/results/orm-boila-2026-08-13.md`.

## Running

```bash
# HTTP: worker pool + per-shard hop-less + multi-DB
#   BOILA_MAX_CONN=64  BOILA_WORKERS=4 (0 = go_bg per conn)
./baga -I . -I app-product app-product/boilaDB/tools/serve.baga
# PG wire protocol v3 (BOILA_PGPORT default 6575)
./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga
curl localhost:6570/health   # mode=mt-pool, workers, live_conn; /ready, /metrics
```

## Tests

```bash
./scripts/baga-test                                # discovers tests/**/*_test.baga
bash app-product/boilaDB/scripts/filesize.sh       # ≤ 400 lines per source file
```

## Modality benches (chunked seed — Q2 arena)

```bash
bash bench/boila/run_modality_benches.sh all       # ts / graph / vec
# results: bench/boila/results/modality-2026-08-10.md
```
