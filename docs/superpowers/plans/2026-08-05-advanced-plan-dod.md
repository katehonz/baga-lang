# Advanced plan — Definition of Done (close-out)

**Date:** 2026-08-05  
**Plan:** [2026-08-05-advanced-go-rust.md](2026-08-05-advanced-go-rust.md)  
**Verdict:** **DoD met** — criteria below are green; residuals are post-plan horizon only.

## Criteria checklist

| # | Criterion | Status | Evidence (paths) |
|---|-----------|--------|------------------|
| 1 | Qualified sum variants + `Vec` of sum enums | ✅ | A1/A2; `tests/std/sum_qualify_test.baga`, `sum_vec_test.baga`; `docs/language-en.md` |
| 2 | pg / orm / jsonrpc / pbbaga L3 results (no dual `ok:i64` world on those APIs) | ✅ | `pgbaga` `PgResult`; `ormbaga` `OrmExec`/`OrmQuery`/`MigrateResult`; `jsonrpcbaga` `JrpcOk`/`JrpcErr`/`JrpcSkip`; `pbbaga` `GrpcCall` |
| 3 | apps/api production layout (tracing/logs/metrics/timeouts surface) | ✅ | fmr middleware B2.1; OpenAPI B2.4; `/metrics` + `/ready` + graceful SIGTERM; [product-path.md](../../runbooks/product-path.md) |
| 4 | gRPC unary client + server product path + Go-aligned codes | ✅ | client + registry dual protocol; `tests/grpc_goldens_test.baga`, `registry_grpc_test.baga`; statusbaga |
| 5 | Engine recovery beyond happy-path MVP | ✅ | `tests/lsm_recover_test.baga`; `tests/raft_persist_test.baga`; LSM R0–R7 |
| 6 | Honest limits still listed | ✅ | Plan non-goals + §11; package `gaps.md`; LLVM / borrow / io_uring design notes |

## What shipped under the plan (summary)

- **Track A:** A1 qualified variants, A2 Vec/Map of sums, A5 FNS_MAX, MEM lite  
- **Track B:** B1 L3 stack (pb/pg/orm/jsonrpc); B2 middleware + OpenAPI; B3 dual gRPC + goldens; B4 recover/stress/latency  
- **Track R:** lsmbaga R0–R7 (SST v5, L0–L3, byte targets)  
- **Phase 5 stretch (sketches/notes):** protoc_baga, io_uring probe, structural liveness, borrow-lite design, LLVM L3 status  

## Explicitly not claimed

- Full generics, full borrow checker, production io_uring, full protoc plugin  
- Full Raft safety proof, full OTel SDK, RocksDB feature parity / `rocksbaga` rename  
- LLVM as production backend for L3  

## Post-plan horizon (next workstreams)

Ordered for north-star fit (storage + product + language honesty):

1. **Storage polish** — ~~finer compact pick (R8)~~; bloom filter file; optional rename to rocksbaga  
2. **Product depth** — L5 fmr routes; gRPC H2 client; more status/ctx defaults  
3. **Language** — LLVM L3 or keep C-only; optional C′ borrow-lite impl  
4. **Sketches only if product needs them** — syscall6 + mmap for real io_uring; full protoc  

## Smoke commands (regression narrative)

```bash
./baga -I . -I app-product tests/orm_test.baga
./baga -I . -I app-product tests/jsonrpc_test.baga
./baga -I . -I app-product tests/lsm_test.baga
./baga -I . -I app-product tests/lsm_recover_test.baga
./baga -I . -I app-product tests/grpc_goldens_test.baga
./tools/protoc_baga/test_sketch.sh
./tools/iouring/test_sketch.sh
# product path: docs/runbooks/product-path.md
```

## Changelog

Recorded under `CHANGELOG.md` **[Unreleased]** as *Advanced plan DoD met*.
