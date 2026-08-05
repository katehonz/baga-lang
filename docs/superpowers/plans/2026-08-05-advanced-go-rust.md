# Advanced plan: Baga vs Go / Rust — language + real apps (not demos)

**Date:** 2026-08-05  
**Status:** **plan DoD met** (2026-08-05) — educational language + ecosystem
to prove Baga; long goal **RocksDB-class DB** continues as post-plan horizon  
**Version baseline:** 0.8.0 · A1/A2/MEM + B1–B4 + Phase R0–R8 + Phase 5 sketches  
**North star:**  
1. **Language lab** — Baga is an *educational systems language*; packages are
   real building blocks, not throwaway demos.  
2. **Ecosystem** — HTTP/TLS/PG/gRPC/consensus/apps exist to *exercise and
   showcase* the language (effects, MEM, verify).  
3. **Endgame storage** — a **RocksDB-like embedded KV** in pure Baga
   (`rocksbaga` → LSM quality; not RocksDB feature parity).  
4. Differentiator vs Go/Rust: effects + `--verify` + honest systems stack —
   not full Rust borrow (optional light borrow only; never mandatory).

---

## 0. Positioning (honest)

| Competitor | They win today | Baga’s counter |
|------------|----------------|----------------|
| **Go** | stdlib + gRPC ecosystem, `context`, deploy simplicity, GC | CSP `!Par` already Go-class; status/mdt/ctx/client landed; **no GC footguns claimed** — MEM seatbelt + effects |
| **Rust** | `Result`/`Option`, ownership, crates.io depth | L3 sum types + drop/MEM-1/2/3 lite; **effects as types** stronger than ignored `Result`; verifier M0–M18 |
| **Neither** | — | Spec-first + static fragment proofs on concurrent protocols (M14/M16) |

**Non-goals of this plan**

- **Full** Rust borrow checker / affine types / lifetime generics  
- Generics / traits / protoc codegen (later tracks)  
- Async/await (CSP + poll stays; document as feature)  
- Full OTel SDK, full Raft safety proof, **production** io_uring poll backend
  (Phase 5 sketch only: `tools/iouring/`)  

**Optional (never a gate):** a **light, opt-in borrow checker** — see Track C′
and `docs/superpowers/plans/2026-08-05-memory-management.md` §7. Default Baga
sharing stays; no package must adopt it.

**Rule:** every milestone ends with a **regression-green product surface** (tests + one real app path), not a new orphan package.

---

## 1. Current foundation (already shipped — do not re-demo)

**Language:** L3 sum types + full match; L3 **struct fields** (topo emit); L5 closures; L4 containers-of-structs; MEM-1/2 drop; MEM-3 arena seatbelt lite; effects; `--verify` M0–M18 fragment.

**Product:** fmrbaga + apps/api (Lucky stack); httpdbaga H1/H2; pg/orm; jwt; pbbaga + **unary client** (`CallOk`/`CallErr`); status/mdt/ctx; lsm/raft/txn; cloud metrics/logs/signals; otel lite.

**Residuals at plan open (historical) → status at DoD**

1. ~~Global unique sum-variant names~~ — **A1 shipped** (`Enum::Ok`)  
2. ~~No `Vec<Res>` / `Map<K,Res>`~~ — **A2 shipped**  
3. ~~Stand-in `ok:i64` / `err:str`~~ — **pbbaga + pg + orm + jsonrpc L3** ✅  
4. fmr routes still id-dispatch (L5 not adopted in router) — **post-plan**  
5. gRPC client is H1-only; H2 trailers still approximate — **post-plan**  
6. ~~apps/api middleware bare~~ — **B2.1 shipped** (request-id + otel
   traceparent + structured log); full status/ctx/mdt as *handler default*
   still optional polish  
7. LLVM rejects L3 (honest); production path is C backend — **post-plan**
   (status note `specs/2026-08-05-llvm-l3-status.md`)

---

## 2. Architecture of the plan — three tracks

```
        ┌─────────────────────────────────────────┐
        │  Track A — Language (unlock real APIs)  │
        │  A1 variant hygiene → A2 Vec<sum> →     │
        │  A3 std Result convention → A4 LLVM L3  │
        └──────────────────┬──────────────────────┘
                           │ enables
        ┌──────────────────▼──────────────────────┐
        │  Track B — Product (kill demos)         │
        │  B1 migrate core stack to L3 Result     │
        │  B2 fmr + apps/api production surface   │
        │  B3 gRPC service product (not Hello)    │
        │  B4 lsm/raft engine hardening           │
        └──────────────────┬──────────────────────┘
                           │ proves
        ┌──────────────────▼──────────────────────┐
        │  Track C — Differentiator               │
        │  C1 MEM-3 deeper regions                │
        │  C2 verify on product fragments         │
        │  C3 bench gates in CI narrative         │
        └─────────────────────────────────────────┘
```

Sequencing principle: **A unlocks B; B consumes C.** Do not land B-scale migrations before A1 (variant collisions will thrash).

---

## 3. Track A — Advanced language

### A1 — Namespaced / qualified sum variants  **(highest leverage)**

**Problem:** `Ok`/`Err` are globally unique → every package invents `CallOk`, `DecOk`, … — unusable for a shared Result culture (Rust/Go ergonomics).

**Design options (pick one in implementation plan):**

| Option | Shape | Pros | Cons |
|--------|-------|------|------|
| **A1-a** (recommended) | Construction `Res::Ok(x)` + match `Res::Ok(v)` *or* short `Ok` if unambiguous | Rust-like; keeps bare `Ok` when unique | Parser + checker resolve path |
| **A1-b** | Keep global uniqueness; ship `std/result` with **one** `Ok`/`Err` only | Smallest change | Libraries cannot define own Results |
| **A1-c** | Mangle variant names as `EnumName_Variant` at codegen only; surface bare if unique | Backward compatible | Surprising resolve rules |

**Recommended A1-a acceptance:**

- `enum PgRes { Ok(PgRows), Err(str) }` and `enum RpcRes { Ok(str), Err(RpcErr) }` coexist  
- Bare `Ok(x)` still works when exactly one `Ok` in scope (compat)  
- Qualified form always works  
- Tests: multi-package import collision probe + sumtype_test  
- Docs §11.1 rewrite; CHANGELOG  

**Estimate:** 1–2 solid lang PR days (parser, checker resolve, codegen, LLVM skip path unchanged).

### A2 — `Vec<Res>` / `Map<K, Res>` (box path like L4 structs)

**Why:** batch RPC results, validation error lists, job queues of outcomes.

**Approach:** reuse L4 struct-element box path in C runtime (`elem_kind` sum-enum); checker allow `TYPE_ENUM` as vec/map value; map keys stay i64/str.

**Acceptance:**

- `vec_push`/`vec_get` round-trip `Res`  
- `Map<str, Res>` get/set  
- Negative: still no generic `Result<T,E>`  
- sumtype_test + map/vec tests  

**Estimate:** 1–2 days after A1 (or parallel if variants stay unique — harder to test multi-package).

### A3 — Standard Result / Option conventions (stdlib, not demos)

After A1:

```baga
// std/result/result.baga  (names TBD; avoid stealing all Ok)
// Document convention: package-local enums with qualified variants
// Helpers: unwrap_or, is_ok, map_err (fn values / L5 where useful)
```

**Do not** force one global `enum Result` if A1-a lands — prefer **documented pattern** + 2–3 helpers.

**Acceptance:** language-en §11.2 “Result convention”; used by B1 migrations.

### A4 — LLVM L3 parity (optional, production-optional)

C backend remains ship path. LLVM `unsupported` is honest today.

**Scope if pursued:** tagged union lower + match; same topo typedef order.  
**Defer** until A1–A2 green and product pressure exists (sandbox/JIT).

### A5 — Language hygiene (small, do early)

| Item | Why |
|------|-----|
| Hard error at `FNS_MAX` (not silent) | fmr+orm real apps hit this class of bug before |
| Match arm expression value consistency for `if` blocks | already bit us in grpc_client_test |
| Document sum-in-field + qualified variants in language-bg/en together | agent/human single source |

---

## 4. Track B — Product: advanced apps, not probes

### B1 — Migrate core stack off `ok:i64` stand-ins  **(credibility)**

Order by call-graph fan-in (migrate leaves first or bottom-up drivers):

| Phase | Package | Target shape | Tests gate |
|-------|---------|--------------|------------|
| B1.1 | **statusbaga** (already codes) | keep struct Status; optional sum for parse outcomes | status_test |
| B1.2 | **pbbaga** decode | `GFrame`/`GFail`, Hello parse as sum (unique or A1) | pb/grpc tests |
| B1.3 | **pgbaga** | `PgQueryOk`/`PgQueryErr` (or qualified) for query | pg_test |
| B1.4 | **ormbaga** | migrate `OrmQuery`/`OrmCount`/`MigrateResult` | orm_test |
| B1.5 | **jsonrpcbaga** | kill `RpcResult` stand-in | jsonrpc_test |
| B1.6 | **tplbaga / bagadecimal / oauthbaga** | as touched | existing tests |

**Rules**

- One package per commit when possible  
- No dual APIs longer than one release — delete `ok:i64` fields  
- Prefer struct payload for multi-field success (`struct Rows { … }`) + sum wrapper  

### B2 — fmrbaga + apps/api as the flagship product

**Goal:** `apps/api` is the answer to “show me a real Baga service,” not `cloudbaga` Hello.

| Step | Work |
|------|------|
| B2.1 | Middleware stack: request-id, **otel traceparent**, **logbaga** JSON, optional CORS (ordered hooks, not only `fmr_before`) | ✅ |
| B2.2 | Wire **ctxbaga** deadline from `grpc-timeout` / `X-Request-Timeout` on long routes |
| B2.3 | Errors: map domain failures → **statusbaga** codes where gRPC; HTTP detail envelope stays |
| B2.4 | OpenAPI emit from route table (closes fmr G5) — real client contract | ✅ |
| B2.5 | L5 route table experiment: `Map<str, fn(FmrCtx)->FmrOut>` *or* keep ids but codegen from table (pick one; ids OK if documented) |
| B2.6 | Integration script: migrate → serve → curl auth CRUD → metrics/readyz (CI-friendly) |

**Acceptance:** README of apps/api reads as ops runbook; no “probe” language; `FMR_WORKERS=N` path tested.

### B3 — gRPC product surface (fight Go)

| Step | Work |
|------|------|
| B3.1 | H2 unary client path (or document H1 as intentional edge proxy mode) | partial (H1 unary product path) |
| B3.2 | Server interceptor chain: auth MD → ctx → metrics → handler (compose with mdt/status) | lite (request-id + log on gRPC via fmr) |
| B3.3 | **registry dual-protocol** HTTP JSON + gRPC GetPackage/ListPackages same port | ✅ |
| B3.4 | Fixed golden vectors (Hello, gRPC frame, registry PB, status 0/3/5/14/16) | ✅ |
| B3.5 | `google.rpc.Status` details-bin optional (statusbaga S1) | later |

**Acceptance:** live test calls Baga server from Baga client; status codes match Go semantics for 0/3/5/14/16.

### B4 — Storage / consensus: engine quality bar

MVPs shipped; advanced means **ops-grade behavior**:

| Step | Work |
|------|------|
| B4.1 | lsmbaga recovery + page-cache stress (`lsm_recover_test`; partial get already R5) | ✅ |
| B4.2 | raftbaga: persistence of log + restart recovery test (not only in-memory election) | ✅ |
| B4.3 | txnbaga: multi-key 2PC under concurrent clients (channel stress) | ✅ |
| B4.4 | Bench gate: `bench/run_latency.sh` numbers recorded in CHANGELOG or `bench/results/` for regressions | ✅ |

**Acceptance:** kill -9 / restart recovery story for lsm; raft node restart follows leader.

---

## 5. Track C — Differentiator (why not just use Go)

### C1 — MEM-3 regions beyond lite

- Tag more pointer arithmetic / slices where safe  
- Document alias contract (already honest)  
- Use in lsm page buffers (drop + region on free)

### C2 — Verify product fragments

- Extract pure decision functions from raft rules / tpc_decide (already partial)  
- Add `--verify` examples under `examples/verify/` for **new** B1 pure helpers  
- Never claim full Raft safety (thesis-open-problems stays)

### C3 — Effects narrative in apps

- Public APIs of drivers show `!IO !Net !Par` honestly  
- Handlers in apps/api stay effect-correct (no silent catch)

### C′ — Light optional borrow checker (**optional — not mandatory**)

Parent design space: `docs/superpowers/plans/2026-08-05-memory-management.md`
(Option A revised §7). **This track never blocks A/B phases.**

| Rule | Meaning |
|------|---------|
| **Opt-in only** | Default typecheck unchanged; no pragma = no new errors |
| **Not full Rust** | No lifetime params, no move-by-default, no rewrite of Vec/Map sharing |
| **Lite shapes** | Spot exclusive borrow of a local; and/or drop-adjacent alias ban; and/or `--borrow-lite` WARN pass |
| **Honesty** | Aliases through containers still unchecked (same as MEM-1) |

**When to design:** only if/when product pain shows (e.g. use-after-drop
through a second name the checker could have seen). Prefer finishing MEM-3
payload regions first if both compete for attention.

**Exit criteria (if ever started):** a design note + one opt-in probe file;
existing monorepo builds without flags still green.

---

## 6. Phased roadmap (execution order)

### Phase 0 — Lock the story (0.5 day)

- [ ] This plan → `docs/superpowers/plans/2026-08-05-advanced-go-rust.md` (+ short spec if A1 needs design approval)  
- [ ] README “vs Go/Rust” one-pager pointer (optional, no marketing fluff)  
- [ ] Milestone labels: A1… / B1… / C1… in CHANGELOG Unreleased sections as they land  

### Phase 1 — Language unlock (A1 + A5, then A2) — **done**

1. **A5** FNS_MAX hard error ✅  
2. **A1** qualified variants ✅  
3. **A2** Vec/Map of sum enums ✅  

### Phase 2 — Stack migration (B1) — **done**

1. pbbaga decode sums ✅  
2. pgbaga `PgResult` → `PgOk`/`PgErr` ✅  
3. ormbaga `OrmQuery`/`OrmExec`/`OrmCount`/`MigrateResult` L3 ✅  
4. jsonrpc `RpcResult` L3 ✅ (`JrpcOk`/`JrpcErr`/`JrpcSkip`)  
5. Optional later: polish call sites to always use helpers (done for apps/api/registry/oauth)

**Note:** access via `orm_ok` / `orm_db_q` / `migrate_*` helpers — sum enums
have no `.ok` fields. C′ borrow remains optional-only.

### Phase Stabilize — language + apps (**landed baseline**)

Green full suite + apps/api runbook. Residual B1 orm/jsonrpc explicit backlog.

### Phase R — RocksDB path (storage endgame; parallel with product)

**Horizon:** embedded engine comparable *in ambition* to RocksDB — not a
feature clone, but a real LSM store that forces the language.

| Step | Work | Status |
|------|------|--------|
| R0 | `lsmbaga` MVP: WAL + memtable + SST + page cache + RESP | ✅ |
| R1 | **Binary search** on sorted SST + min/max key filter | ✅ |
| R2 | **BAGASST2** restart index (bsearch restarts + block scan; v1 read OK) | ✅ |
| R3 | Binary values (`Map<str,bytes>`), oldest-N compact + drop pure tombs | ✅ |
| R4 | **BAGASST3** bloom + restart; chain oldest-N compact | ✅ |
| R5 | **BAGASST4** footer partial get + **L0/L1** levels | ✅ |
| R6 | **BAGASST5** per-block CRC + **L2** file-count tiers | ✅ |
| R7 | Byte-size targets + **L3**; optional rocksbaga name when quality warrants | ✅ targets+L3; rename later |
| R8 | Oldest-N merge pick (`merge_pick`) | ✅ |
| R9 | Standalone bloom sidecar (`BAGABLM1`) | ✅ |
| R10 | Package rename **`rocksbaga`** (`lsmbaga` shim kept) | ✅ |

**Rule:** every R step keeps `tests/lsm_test.baga` green and documents honesty
in `lsmbaga/gaps.md`.

### Phase 3 — Flagship apps (B2 + B3)

1. fmr middleware + apps/api otel/log — **B2.1 done**  
2. OpenAPI emit from route table — **B2.4 done**  
3. gRPC dual path on registry — **B3.3 done**  
4. Interop goldens — **B3.4 done** (`tests/grpc_goldens_test.baga`)

**Exit criteria:** single README path runs product API + gRPC call with metrics and graceful shutdown.  
**Done:** [docs/runbooks/product-path.md](../../runbooks/product-path.md) — API `/metrics`+`/ready`, registry dual gRPC, `fmr_run` SIGTERM/SIGINT + poll accept.

### Phase 4 — Engines + verify (B4 + C)

1. lsm recovery + page-cache stress — **B4.1 done** (`lsm_recover_test`)  
2. raft persistence lite — **B4.2 done** (`persist.baga` + `raft_persist_test`)  
3. txn multi-key concurrent 2PC — **B4.3 done** (`txn_stress_test`)  
4. Bench numbers — **B4.4 done** (`bench/results/latency-2026-08-05.md`)

**Exit criteria:** recovery + latency note; verify examples still green.

### Phase 5 — Stretch (only if Phases 1–3 hold)

- A4 LLVM L3 — **status note** (`specs/2026-08-05-llvm-l3-status.md`); still C-only  
- protoc → baga sketch (P1 pbbaga) — **done** (`tools/protoc_baga/`)  
- io_uring poll backend experiment — **sketch done** (`tools/iouring/` +
  `specs/2026-08-05-io-uring-poll-sketch-design.md`); product still `SYS_poll`  
- Structural liveness lemmas — **done** (`examples/verify/liveness_struct.baga`)  
- **C′ light optional borrow** — **design note** (`specs/2026-08-05-borrow-lite-design.md`); not implemented

### Phase note — MEM vs product

Default execution order remains **B (product) first**. Track C / C′ are
differentiators. **Optional borrow does not reorder or delay B1–B3.**

---

## 7. Definition of Done (plan success) — **MET 2026-08-05**

The advanced plan is **done** when all of:

| # | Criterion | Evidence |
|---|-----------|----------|
| 1 | **Language:** qualified sum variants + `Vec` of sum enums | A1/A2; `tests/std/sum_qualify_test.baga`, `sum_vec_test.baga`; language docs |
| 2 | **Stack:** pg/orm/jsonrpc/pbbaga L3 results end-to-end | `PgResult`, `Orm*`, `RpcResult`/`Jrpc*`, `GrpcCall`/`CallOk`; no dual `ok:i64` world on those surfaces |
| 3 | **App:** apps/api production layout | middleware + OpenAPI + `/metrics`/`/ready`; [product-path runbook](../../runbooks/product-path.md) |
| 4 | **gRPC:** unary client+server product path | `pbbaga` client + registry dual protocol; goldens `tests/grpc_goldens_test.baga`; statusbaga codes |
| 5 | **Engine:** recovery beyond happy-path MVP | `tests/lsm_recover_test.baga`, `tests/raft_persist_test.baga`; R0–R7 on lsmbaga |
| 6 | **Honest limits** still listed | This § non-goals + package `gaps.md` + §11 residual horizon |

**Verdict:** criteria 1–6 are satisfied. Stretch Phase 5 items that shipped as
**sketches/notes** (protoc_baga, io_uring probe, structural liveness, borrow-lite
design, LLVM L3 status) count as honesty, not full product backends.

Full write-up: [2026-08-05-advanced-plan-dod.md](2026-08-05-advanced-plan-dod.md).

---

## 8. Risk register

| Risk | Mitigation |
|------|------------|
| A1 breaks existing bare `Ok` in sumtype_test | Compat rule: bare OK if unique |
| Migration churn across monorepo | Package-at-a-time; no big-bang |
| Global variant uniqueness already in wild (`CallOk`) | Keep; A1 makes new code nicer |
| Scope creep into protoc/generics | Explicit Phase 5 only |
| “Just more packages” habit | Phase gates require app/engine acceptance |

---

## 9. Immediate next implementation step

**Plan DoD met.** Historical Phase 0+1 start steps are complete (A1/A5/A2 → B1–B4 → R → Phase 5 sketches).

**Post-plan horizon** (not gates for this plan’s DoD) — pick one:

1. **Storage:** ~~rocksbaga rename (R10) + layered dirs~~; further quality  
2. **Product:** L5 route table; gRPC H2 client; optional status/ctx defaults in more handlers  
3. **Language:** LLVM L3 (or keep C-only honesty); optional C′ borrow-lite impl  
4. **Sketches → product only if needed:** multi-arg syscall / ring mmap for io_uring; full protoc plugin  

See §11 and [advanced-plan-dod.md](2026-08-05-advanced-plan-dod.md).

---

## 10. File / surface map (touch list)

| Area | Paths |
|------|--------|
| Lang A1/A2/A5 | `src/parser.c`, `src/checker.c`, `src/codegen_c.c`, `tests/std/sumtype_test.baga`, `docs/language-*.md` |
| Result convention | `std/result/` (new) or docs-only until A1 |
| B1 migrations | `app-product/{pbbaga,pgbaga,ormbaga,jsonrpcbaga}/`, matching `tests/*` |
| B2 | `app-product/fmrbaga/`, `apps/api/` |
| B3 | `app-product/pbbaga/grpc_client.baga`, H2 client glue, product service |
| B4 | `app-product/{rocksbaga,raftbaga,txnbaga}/`, `bench/` |
| C | `src/verify.c` examples, MEM region in checker |

---

## 11. Out of scope this plan + post-plan residual horizon

### Still out of scope (do not claim)

- New universal packages (path/glob/uuid wave is closed enough)  
- Rewriting cloudbaga as the flagship (apps/api is)  
- Full generic `Result<T,E>`  
- **Full** Rust borrow checker / move-by-default (light optional is C′, not this)  
- Kubernetes operator / multi-cluster  
- Self-hosting compiler on L3 APIs  
- Production io_uring poll backend (sketch only: `tools/iouring/`)  
- Full protoc plugin / full Raft safety proof / full OTel SDK  

### Honest limits (pointers)

| Area | Where named |
|------|-------------|
| Generics / traits | language docs; this plan non-goals |
| H2 gaps | `app-product/httpdbaga/gaps.md` |
| Raft incompleteness | `app-product/raftbaga/gaps.md` |
| LSM vs RocksDB | `app-product/rocksbaga/gaps.md` (R0–R10; not full RocksDB) |
| LLVM L3 | `docs/superpowers/specs/2026-08-05-llvm-l3-status.md` |
| Borrow-lite (C′) | design only: `…/2026-08-05-borrow-lite-design.md` |

### Post-plan residual (ordered by north-star fit)

1. ~~lsmbaga → **rocksbaga** (R10)~~  
2. L5 fmr route table; gRPC over H2  
3. LLVM L3 or permanent C-backend honesty  
4. Optional C′ borrow-lite implementation  

---

*Plan living doc. Phase 1 done; Phase 2 B1 in progress. C′ borrow lite =
optional only — see memory-management.md §7.*
