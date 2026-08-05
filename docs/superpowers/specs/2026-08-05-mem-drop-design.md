# MEM-1/2 — `drop` + checker-enforced memory discipline: design

Date: 2026-08-05. Status: approved by user (chat — "ok следващото"), implementation in progress.
Parents: `docs/superpowers/plans/2026-08-05-memory-management.md` (design
reference, Option B core + MEM-2), `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md`
(sequencing step 3 — L3 landed before this, as required).

## Goal

`drop(x)` — explicit free of heap values with a checker seatbelt:
use-after-drop and double-drop are **compile errors**, and the runtime
actually reclaims (size-classed free list in the bump arena). This is the
foundation for the DB buffer pool / WAL work (S1 in Track S).

## What drop accepts (v1)

`drop(x)` where `x` is a **let-bound local** (never a parameter) of type:
- `Vec<T>` — deep: element boxes (bytes/struct elements), data buffer, vec struct;
- `Map<K,V>` — deep: pv boxes (struct values), entries, bucket array, map struct;
- `bytes` — the data buffer;
- `fn(...)` closure handle — frees the malloc'd `cell2` pair (the env box
  stays in the arena — closures are few; documented).

`str` and scalars are rejected (`drop: неподдържан тип str — drop е за Vec/Map/bytes/fn`).
Strings stay arena-shared per the plan's §6.

## Checker rules (the seatbelt)

Per-variable `live → dropped` tracking, built on the existing
single-forward-pass + env infrastructure (no new path engine):

- **Straight line**: after `drop(x)`, any use of `x` →
  `използване на 'x' след drop`; a second `drop(x)` → `повторен drop на 'x'`.
- **Branch join (if/else)**: a variable is "definitely dropped" after the
  `if` only if dropped on **all** arms (dropped-set intersection at the
  block join, same scope level). Use of a maybe-dropped variable after the
  join is allowed (honest: the checker reports only certainties).
- **Loops**: `drop` of a variable declared **outside** the loop, anywhere
  in the loop body → error (`drop на външна за цикъла променлива 'x' —
  втората итерация би била use-after-drop`). Dropping loop-locals is fine
  (fresh per iteration; the body is checked once).
- **Parameters**: `drop(param)` → error (params share the caller's buffer
  for Vec/Map — freeing would dangle the caller; v1 draws the line here).
- **Captured by a live closure**: capturing `x` in a lambda marks it;
  `drop(x)` after that → error (`'x' е заснет от ламбда — drop би оставил
  висящ указател`). (Captures of Vec/Map share the reference per L5.)
- **Scope-exit leaks are NOT diagnosed in v1** — the compiler has no
  warning severity (grep: zero) and adding one is out of scope. Leak
  detection is the verifier's job (MEM-2, below) and MEM-3 regions later.
  Documented, not hidden.
- **Aliasing boundary (documented contract)**: the checker tracks
  variables, not heap graphs. A value stashed inside another container
  (`vec_push(v2, x)`-style sharing of str/bytes elements, aliases
  `let y = x` NOT followed by drop of `y`... ) — dropping `x` while an
  alias lives is the programmer's contract, exactly like C. The one alias
  class we DO catch: `let y = x; drop(x); use(y)` is **not** diagnosed
  (documented honestly).

Resolution order: `drop(...)` special-cases in `infer_call` ONLY when no
local/user-fn named `drop` exists (decimal_pg_test.baga's `let drop = ...`
and bagadecimal's `drop: i64` param keep working untouched).

## Runtime (C backend)

- **Free list in `baga_alloc`**: per-size-class singly-linked free lists
  for blocks ≤ 1024 B (16 B granularity, 64 classes), first word of a
  freed block stores the next pointer; `baga_alloc` pops before bumping;
  larger blocks are not reclaimed in v1 (documented). The existing
  pthread mutex already serializes this for `go` threads.
- `baga_free(p, n)` + deep walkers: `baga_drop_vec(v, elem_kind,
  elem_size)`, `baga_drop_map(m, val_kind, val_size)`, `baga_drop_bytes(b)`,
  `baga_drop_fn(h)` — emitted next to the other helpers; `drop(x)` maps in
  codegen by the static element/value kind from the checker.
- Historical garbage (old buffers from `vec_grow`/`map_rehash`) stays
  unreclaimed — drop frees the CURRENT blocks only; documented.
- LLVM backend: honest `llvm_unsupported("drop")` (oracle SKIP-clean).
- Self-hosting: `self/compiler.baga` uses no `drop`; its emitted runtime
  is untouched; `make self` must simply stay green.

## MEM-2 — verifier obligations (alloc → drop)

Reuse the M14 machinery (`HandleList` in `src/verify.c`, kind-3 protocol
violations → REFUTED with witness path):

- New handle kind `HK_DROP` (states 0=live, 1=dropped), keyed by **source
  variable name** (the M14 handles key by symbolic i64 var; Vec/Map/bytes
  aren't symbolic values, so the key is the var string — new small keying
  path, explicitly NOT the Lin-term machinery).
- `vec_new`/`map_new`/`bytes_new`/bytes-producing builtins in a supported
  fragment register the var as live; `drop(var)` transitions; use after
  drop or double drop on a live path → kind-3 violation: **REFUTED**
  ("използване след drop по ..."), with the M14 witness printer.
- Scope/fn exit with a still-live handle → **no verdict** (honest; leaks
  stay UNKNOWN-class by absence, matching the checker v1).
- Outside the supported fragment (unsupported constructs present): honest
  skip, same as M14's `has_unsupported_rec` gating.

## Honestly out of v1

Scope-exit leak errors/warnings; drop on parameters; closure env-box
reclaim; blocks > 1024 B reclaim; region/arena checking (MEM-3); borrow
checking of any kind (plan Option A — rejected); aliasing-through-
containers tracking; `free` of arbitrary `&T`.

## Testing

- `tests/std/drop_test.baga`: drop Vec<i64>/Vec<str>/Vec<bytes>/
  Vec<struct>/Map variants/bytes/closure; reuse-after-drop is NOT in this
  file (it's a compile error — probes); a server-loop-shaped probe
  (alloc → use → drop per iteration ×1e5) that would OOM under the old
  bump-only arena — the reclaim proof.
- Negative probes in `scripts/run_tests.sh`: use-after-drop, double-drop,
  drop-in-loop of outer var, drop of param, drop of captured var, drop of
  str, drop of non-local.
- MEM-2: `examples/verify/mem_drop.baga` — REFUTED use-after-drop with
  witness; a clean alloc→drop fn PROVEN-class (no violation).
- Gates: `make test`, `make self`, `make test-llvm`.
- Docs: new § in docs/language-{en,bg}.md (drop + the honesty boundary),
  CHANGELOG, memory-management plan marked "MEM-1/2 landed".
