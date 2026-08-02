# Channel Content Invariants: Rely–Guarantee Without a Memory Model

**Working title (PhD-scale research note, slice 3)**
*Quantified payload predicates over CSP channels, discharged at spawn*

**Artifact:** the Baga compiler (`--verify`), milestone M16
**Prior art in the same pipeline:** M0–M13 (nonlinear fragment),
M14 (fork–join determinism + handle protocols), M15 (arithmetic safety)
**Status:** implemented and regression-tested (`examples/verify/chan_inv*.baga`)

---

## Abstract

M14 showed that banning shared mutable state collapses fork–join
verification onto sequential machinery. This note closes the concurrency
story's remaining gap: **what flows through a channel**. We add a single
annotation — `invariant c[*] >= 1`, "every payload sent on `c` satisfies the
predicate" — and give it rely–guarantee semantics: every send site must
*discharge* the predicate against the local path (or the axiom is dropped),
every receive site *instantiates* it, and at a `go` spawn the caller must
prove its channel invariant covers the worker's declared `requires c[*] ...`
— otherwise nothing is assumed. The mechanism reuses the M3 element-axiom
machinery wholesale: channels are treated as write-once-read-many "vectors
in time", and the M8 conclusiveness discipline keeps refutations honest.
No new decision procedures, no SMT, no shared-state reasoning.

---

## 1. The remaining gap after M14

M14 proves facts about join results and handle protocols, but a `chan_recv`
payload was "unconstrained i64": value flow across threads was invisible.
The realistic pattern — producer sends work items, consumers rely on their
shape — was out of reach. What is needed is a *quantified* statement over a
channel's contents, and a discipline for who may rely on it.

## 2. The annotation and its semantics

```baga
let c = chan_new(4)
invariant c[*] >= 1        // every payload ever sent on c is >= 1
```

`invariant` is a contextual keyword (like its `while` namesake); the scalar
form `invariant e` doubles as `assume` (the path gains the constraint).

**Anchoring.** The axiom is anchored on the channel's *resolved symbolic
value* (its ghost handle), not its source name — so `let c2 = c` aliases
share one invariant and cannot smuggle an undischarged send around it.

**Rely (receive).** `chan_recv(c)` yields a fresh symbolic value constrained
by every axiom anchored on `c` — the same instantiation rule as M3's
`vec_get`.

**Guarantee (send).** `chan_send(c, v)` must *prove* `v` satisfies every
axiom on `c` against the current path (M3's `vec_push` rule). If it cannot,
the axiom is **dropped** — downstream receives honestly get nothing. A
dropped axiom is the sound response to "this channel might carry anything".

## 3. Crossing the thread boundary

The interesting part is the boundary discipline. A worker declares its
assumption in its own spec:

```baga
spec worker {
    input: c: i64
    output: i64
    requires: c[*] >= 1     // I rely on every payload being >= 1
    ensures:  output >= 1
}
fn worker(c: i64) -> i64 !Par {
    return chan_recv(c)
}
```

At `go(worker, c)` the verifier discharges this against the **caller's**
axioms on the actual channel: same predicate direction, and the caller's
bound provably at least as strong (FM implication). The discharge appears as
an ordinary call-site obligation (provable ДОКАЗАНО in the report).

**Drop-on-escape.** If the callee's spec carries *no* content requires on
the channel parameter, the caller's axioms on it are dropped at the call:
the callee might send anything. The same rule applies at plain M5 calls —
an axiom survives a boundary only when the other side *declares* it. This
is rely–guarantee in the small: assumptions are explicit contracts, checked
at the exact point of thread creation.

Composing with M14's determinism lemma, the whole chain is then provable:

```baga
fn boss(n: i64) -> i64 !Par {         // requires n >= 1, ensures output >= 1
    let c = chan_new(4)
    invariant c[*] >= 1
    chan_send(c, n)                   // discharged: n >= 1
    let h = go(worker, c)             // discharged: caller axiom covers requires
    return join(h)                    // worker ensures output >= 1 assumed (M14)
}
```

— proven statically, end to end (`examples/verify/chan_inv_par.baga`).

## 4. Soundness sketch

- **Receive instantiation** is sound: the axiom is a fact about every sent
  payload; any received value is either some sent payload or `0` on
  closed+empty. (The closed+empty case returns 0 — see the honest
  limitation below: predicates are intended to be established such that
  `0` satisfies them, or the channel is known-N fan-in.)
- **Send discharge** maintains the axiom's truth inductively over the
  program's send sites; dropping is the safe default.
- **Spawn discharge** ensures the worker's assumption holds at thread
  creation; combined with M14's `join(go(f,x)) ≡ f(x)`, the worker's
  ensures (proven under that assumption) may be assumed by the joiner.
- **Drop-on-escape** bounds the analysis to code that opted in; nothing
  unsound can flow through an undeclared boundary.

## 5. Evaluation

| Example | Claim |
|---|---|
| `chan_inv.baga` | local invariant: send discharges, recv instantiates, ensures ДОКАЗАНО |
| `chan_inv_bad.baga` | undischargable payload drops the axiom; recv claim honestly UNKNOWN |
| `chan_inv_par.baga` | cross-thread: spawn discharge ДОКАЗАНО, join result ≥ 1 ДОКАЗАНО |
| `chan_inv_escape.baga` | worker without requires ⇒ drop at spawn ⇒ UNKNOWN |

All machine-checked by `make test`.

## 6. Limitations

1. **Closed+empty reads return 0** (runtime API): a strict content predicate
   (e.g. `c[*] >= 1`) is then violated by the *read itself*, not by any send.
   The discipline today covers known-N fan-in (exactly N sends, N recvs,
   then close); integrating `chan_recv2`'s `ok` flag is future work (it
   needs the pair abstraction deferred from M14).
2. **Single-channel worker arguments.** Channels packed via `cell2` remain
   outside the fragment (same deferred pair abstraction).
3. **Predicate shape** is the M3 fragment: `c[*] <cmp> <linear>` plus
   `sorted`-style markers; no quantifier alternation, no per-index
   predicates.
4. **Liveness** (deadlock freedom) remains out of scope, as in M14.

## 7. Positioning

Session types give channel protocols by construction, at the price of a
specialized type discipline; separation-logic verifiers handle channels via
permissions and ghost state, at the price of SMT and annotation weight.
Baga's answer is one annotation with M3's semantics and M14's boundary
checks — a rely–guarantee fragment where the "rely" and the "guarantee"
are the same predicate, checked by the same Fourier–Motzkin core that
already audits arithmetic, loops, and overflow.

---

## Appendix A — How to reproduce

```bash
make
./baga --verify examples/verify/chan_inv_par.baga     # cross-thread proof
./baga --verify examples/verify/chan_inv_bad.baga     # honest UNKNOWN
./baga --verify examples/verify/chan_inv_escape.baga  # drop-on-escape
make test
```

## Appendix B — Mapping to source

| Concept | Location |
|---------|----------|
| `invariant` statement (syntax) | `parser.c` parse_stmt; `NODE_INVARIANT` |
| Axiom anchoring/extraction | `extract_axiom_resolved`, `resolve_key` |
| Send discharge / recv instantiate | `eval_par_call` (`axiom_holds_for_value`, `axiom_instantiate`) |
| Spawn/call discharge | `eval_user_call` (`axiom_entailed`) |
| Drop-on-escape | `eval_user_call` + spec-less `go` branch (`ax_drop_key`) |
| Worker effect gate | `worker_sig_supported` (Par-only effects) |

---

*Galactic University draft — Baga project. The concurrency chapter closes
where it started: the absence of shared state made it short.*
