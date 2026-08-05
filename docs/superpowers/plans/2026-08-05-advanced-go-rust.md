# Advanced plan: Baga vs Go / Rust — language + real apps (not demos)

**Date:** 2026-08-05  
**Version baseline:** 0.8.0 · HEAD includes L3 fields + gRPC client (`991f782`)  
**North star:** Ship systems/product code that a team would choose over Go for *cloud services* and over “just another language” for *storage/consensus* — with effects + `--verify` as the differentiator, not feature-count parity.

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
- Full OTel SDK, full Raft safety proof, io_uring  

**Optional (never a gate):** a **light, opt-in borrow checker** — see Track C′
and `docs/superpowers/plans/2026-08-05-memory-management.md` §7. Default Baga
sharing stays; no package must adopt it.

**Rule:** every milestone ends with a **regression-green product surface** (tests + one real app path), not a new orphan package.

---

## 1. Current foundation (already shipped — do not re-demo)

**Language:** L3 sum types + full match; L3 **struct fields** (topo emit); L5 closures; L4 containers-of-structs; MEM-1/2 drop; MEM-3 arena seatbelt lite; effects; `--verify` M0–M18 fragment.

**Product:** fmrbaga + apps/api (Lucky stack); httpdbaga H1/H2; pg/orm; jwt; pbbaga + **unary client** (`CallOk`/`CallErr`); status/mdt/ctx; lsm/raft/txn; cloud metrics/logs/signals; otel lite.

**Residual that blocks “advanced language in apps”**

1. Global unique sum-variant names → cannot share `Ok`/`Err` across packages  
2. No `Vec<Res>` / `Map<K,Res>`  
3. Stand-in `ok:i64` / `err:str` still dominate orm/pg/jsonrpc/oauth/tpl/decimal  
4. fmr routes still id-dispatch (L5 not adopted in router)  
5. gRPC client is H1-only; H2 trailers still approximate  
6. apps/api does not yet use status/ctx/mdt/otel as default middleware  
7. LLVM rejects L3 (honest); production path is C backend  

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
| B2.1 | Middleware stack: request-id, **otel traceparent**, **logbaga** JSON, optional CORS (ordered hooks, not only `fmr_before`) |
| B2.2 | Wire **ctxbaga** deadline from `grpc-timeout` / `X-Request-Timeout` on long routes |
| B2.3 | Errors: map domain failures → **statusbaga** codes where gRPC; HTTP detail envelope stays |
| B2.4 | OpenAPI emit from route table (closes fmr G5) — real client contract |
| B2.5 | L5 route table experiment: `Map<str, fn(FmrCtx)->FmrOut>` *or* keep ids but codegen from table (pick one; ids OK if documented) |
| B2.6 | Integration script: migrate → serve → curl auth CRUD → metrics/readyz (CI-friendly) |

**Acceptance:** README of apps/api reads as ops runbook; no “probe” language; `FMR_WORKERS=N` path tested.

### B3 — gRPC product surface (fight Go)

| Step | Work |
|------|------|
| B3.1 | H2 unary client path (or document H1 as intentional edge proxy mode) |
| B3.2 | Server interceptor chain: auth MD → ctx → metrics → handler (compose with mdt/status) |
| B3.3 | One **real** service: e.g. `apps/registry` or new `apps/greeter` is **not** enough — prefer **registry** or **api** dual-protocol (HTTP JSON + gRPC) |
| B3.4 | Interop check: generate golden frames vs `protoc`/Go client **or** fixed golden vectors (no full protoc yet) |
| B3.5 | `google.rpc.Status` details-bin optional (statusbaga S1) |

**Acceptance:** live test calls Baga server from Baga client; status codes match Go semantics for 0/3/5/14/16.

### B4 — Storage / consensus: engine quality bar

MVPs shipped; advanced means **ops-grade behavior**:

| Step | Work |
|------|------|
| B4.1 | lsmbaga: reduce full-file SST load (gaps L3); bounded page cache under stress test |
| B4.2 | raftbaga: persistence of log + restart recovery test (not only in-memory election) |
| B4.3 | txnbaga: multi-key 2PC under concurrent clients (channel stress) |
| B4.4 | Bench gate: `bench/run_latency.sh` numbers recorded in CHANGELOG or `bench/results/` for regressions |

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

---

## 6. Phased roadmap (execution order)

### Phase 0 — Lock the story (0.5 day)

- [ ] This plan → `docs/superpowers/plans/2026-08-05-advanced-go-rust.md` (+ short spec if A1 needs design approval)  
- [ ] README “vs Go/Rust” one-pager pointer (optional, no marketing fluff)  
- [ ] Milestone labels: A1… / B1… / C1… in CHANGELOG Unreleased sections as they land  

### Phase 1 — Language unlock (A1 + A5, then A2)

1. **A5** FNS_MAX hard error  
2. **A1** qualified variants (design note + implement + tests)  
3. **A2** Vec/Map of sum enums  
4. Docs language-en/bg + sumtype_test expansion  

**Exit criteria:** two packages in one program each define `Ok`/`Err` under different enums; `Vec` of that type works.

### Phase 2 — Stack migration (B1)

1. pbbaga decode sums (pattern exemplar)  
2. pgbaga → ormbaga  
3. jsonrpc + one more (tpl or decimal)  
4. Delete stand-in fields; update gaps.md “migrated” not “unblocked”  

**Exit criteria:** `grep -R "ok: i64" app-product/{pg,orm,jsonrpc,pbbaga}` shows only intentional wire flags, not Result stand-ins.

### Phase 3 — Flagship apps (B2 + B3)

1. fmr middleware + apps/api otel/log/status  
2. OpenAPI emit  
3. gRPC dual path on one product service  
4. Interop goldens  

**Exit criteria:** single README path runs product API + gRPC call with metrics and graceful shutdown.

### Phase 4 — Engines + verify (B4 + C)

1. lsm SST/page hardening + recovery test  
2. raft persistence lite  
3. MEM-3 where it pays  
4. Bench numbers checked in  

**Exit criteria:** recovery + latency note; verify examples still green.

### Phase 5 — Stretch (only if Phases 1–3 hold)

- A4 LLVM L3  
- protoc → baga sketch (P1 pbbaga)  
- io_uring poll backend experiment  
- Thesis open problems: one structural liveness lemma (not full)  

---

## 7. Definition of Done (plan success)

The advanced plan is **done** when all of:

1. **Language:** qualified sum variants + `Vec` of sum enums shipped and documented  
2. **Stack:** pg/orm/jsonrpc/pbbaga use L3 results end-to-end (no ok/err dual world)  
3. **App:** apps/api is the canonical production layout with tracing/logs/metrics/timeouts  
4. **gRPC:** unary client+server product path with status/metadata/context semantics aligned to Go codes  
5. **Engine:** at least one recovery test beyond happy-path MVP for lsm or raft  
6. **Honest limits** still listed (no generics, no full Raft proof, H2 gaps named)  

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

**Start Phase 0+1:**

1. Write design note for **A1 qualified variants** under `docs/superpowers/specs/2026-08-05-sum-variant-qualify-design.md`  
2. Implement A5 FNS_MAX hard error (tiny, unblocks real apps)  
3. Implement A1 against sumtype_test multi-enum `Ok` collision  
4. Only then open B1.2 pbbaga decode migration as the first product consumer  

---

## 10. File / surface map (touch list)

| Area | Paths |
|------|--------|
| Lang A1/A2/A5 | `src/parser.c`, `src/checker.c`, `src/codegen_c.c`, `tests/std/sumtype_test.baga`, `docs/language-*.md` |
| Result convention | `std/result/` (new) or docs-only until A1 |
| B1 migrations | `app-product/{pbbaga,pgbaga,ormbaga,jsonrpcbaga}/`, matching `tests/*` |
| B2 | `app-product/fmrbaga/`, `apps/api/` |
| B3 | `app-product/pbbaga/grpc_client.baga`, H2 client glue, product service |
| B4 | `app-product/{lsmbaga,raftbaga,txnbaga}/`, `bench/` |
| C | `src/verify.c` examples, MEM region in checker |

---

## 11. Out of scope this plan (record, don’t “accidentally” do)

- New universal packages (path/glob/uuid wave is closed enough)  
- Rewriting cloudbaga as the flagship (apps/api is)  
- Full generic `Result<T,E>`  
- Kubernetes operator / multi-cluster  
- Self-hosting compiler on L3 APIs  

---

*End of advanced plan. Approve Phase 1 (A1 + A5) to begin implementation; Phase 2+ wait on A1 landing.*
