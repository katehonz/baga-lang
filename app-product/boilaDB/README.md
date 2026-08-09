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
users/ACL, `EXPLAIN [ANALYZE]`). Concurrent HTTP + PG server: `go_bg`
per connection, per-shard hop-less stores, shared per-db plan cache;
DDL is serial per db and there is no true 10k-client pool yet
(gaps W1/P11-1). No SCRAM/TLS (gaps W6). Measured @10k ladder: point
156k ops/s, insert 524 ops/s, mix 2.6k ops/s
(`bench/boila/results/harness-2026-08-09.md`).

## Running

```bash
# HTTP: go_bg + per-shard hop-less + multi-DB (BOILA_MAX_CONN default 64)
./baga -I . -I app-product app-product/boilaDB/tools/serve.baga
# PG wire protocol v3 (BOILA_PGPORT default 6575)
./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga
curl localhost:6570/health   # mode=mt-shard, live_conn; /ready, /metrics
```

## Tests

```bash
./scripts/baga-test                                # discovers tests/**/*_test.baga (boila: 29 files)
bash app-product/boilaDB/scripts/filesize.sh       # ≤ 400 lines per source file
```
