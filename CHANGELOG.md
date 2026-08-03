# Changelog

## [Unreleased]

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
- New type `Map<K, V>`: keys `i64`/`str`, values `i64`/`str`/`f64` — the same
  fix-on-first-use rules and annotations as `Vec<T>`; mixing key or value
  types is a compile-time error.
- Builtins: `map_new`, `map_set`, `map_get` (zero-value when absent),
  `map_has`, `map_del`, `map_len`, `map_keys` (→ `Vec<str>`/`Vec<i64>`).
- Maps are pointers: passing one to a function shares it (mutate-through,
  unlike by-value structs) — the natural store for servers and caches.
- C backend: chained hash table (`baga_Map`, FNV-1a / Murmur-mix hashing,
  grows at load factor 3/4). LLVM backend: honest "unsupported" diagnostic.
- Self-hosting parity unchanged (`make self` fixed point holds); the self
  compiler does not parse `Map` yet (documented limitation).
- Docs: `docs/language-{en,bg}.md` §12.5 + type/builtin tables.
- Tests: `tests/std/map_test.baga` (31 checks, incl. rehash growth) +
  two negative type-error checks wired into `make test`.

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
