# Design: `std/par` — cloud-native concurrency (Go-inspired)

Date: 2026-08-02
Status: Accepted for M1 implementation

## 1. Goal

Give Baga a **practical concurrency surface** for cloud-native services:
fan-out request handlers, parallel CPU work, pipeline stages — without
pretending to be a green-thread runtime yet.

Inspiration: Go goroutines + channels (CSP). **Not** a 1:1 clone.

## 2. Why not “just goroutines”

| Go | Baga M1 |
|----|---------|
| Unbounded lightweight threads | OS threads via `pthread` (honest cost) |
| Closures capture environment | No HOF/closures yet → **named** `fn(i64)->i64` workers |
| Channels of any type | Channels of **`i64`** (opaque handles / small payloads) |
| `select`, context cancel | M2 |
| Runtime scheduler (M:N) | M3+ |

Cloud-native preference: **structured patterns** (spawn + join / fan-in via
channels) over fire-and-forget. Unbounded `go` without join is allowed but
documented as a foot-gun.

## 3. Effect: `!Par`

Concurrency is a type dimension, same as `!IO` / `!Net`:

```baga
fn worker(x: i64) -> i64 { return x * x }

fn main() -> i64 !Par {
    let h = go(worker, 7)     // !Par
    return join(h)            // 49
}
```

- Pure functions stay pure.
- A handler that fans out must declare `!Par` (and usually `!IO`/`!Net`).
- Effects remain compile-time only (no runtime effect objects).

## 4. Surface (M1 builtins — always available, no import)

| Builtin | Signature | Notes |
|---------|-----------|-------|
| `go` | `(fn, arg: i64) -> i64 !Par` | First arg is a **function identifier** with signature `(i64) -> i64`. Returns opaque join handle (as `i64`). |
| `join` | `(h: i64) -> i64 !Par` | Blocks until the task finishes; returns the worker’s `i64` result. |
| `chan_new` | `(cap: i64) -> i64 !Par` | Buffered channel of `i64`. `cap >= 0`; `0` = unbuffered rendezvous. |
| `chan_send` | `(c: i64, v: i64) -> i64 !Par` | Blocks if full. Returns 0 on success, -1 if channel closed. |
| `chan_recv` | `(c: i64) -> i64 !Par` | Blocks if empty. Returns the value; after close + drain returns `0` and… (see below). |
| `chan_try_recv` | `(c: i64) -> i64 !Par` | Non-blocking: returns value, or a sentinel. |
| `chan_close` | `(c: i64) -> i64 !Par` | Idempotent close. |

**Recv after close.** M1 uses a pair protocol for clean shutdown in practice:

```baga
// Prefer: producer sends N values then closes; consumers use
// chan_recv_ok which returns 1 and writes via side channel — too rich for M1.
// M1 ship: chan_recv returns the i64; after close+empty returns 0 and
// chan_closed(c) is 1. Document: do not send 0 as a payload if you need
// EOF distinction, OR use a length-prefixed protocol on a second channel.
```

Simpler M1 contract (chosen):

- `chan_recv(c) -> i64 !Par` — if closed and empty, returns **0** and sets a
  per-channel flag; `chan_ok(c) -> i64 !Par` returns 0 if the last recv hit
  EOF, else 1. (Thread-local last status is wrong for multi-consumer.)
- **Better M1:** `chan_recv` returns the value; closed+empty **blocks forever**
  is wrong; closed+empty returns `0` and `chan_is_closed(c)` is separate —
  ambiguous for payload 0.
- **Chosen M1:** use **two-slot result** is not available. Use:
  - `chan_send` / `chan_recv` for data
  - **Convention for examples:** send a count first, or use only non-zero
    payloads in demos; production code can send pointers-as-i64 to heap
    boxes later.

Even better practical choice for M1:

```
chan_recv(c) -> i64 !Par   // blocks; on closed+empty returns 0
chan_open(c) -> i64 !Par   // 1 if still open OR buffer non-empty, 0 if closed and empty
```

Workers loop: `while chan_open(c) != 0 { let v = chan_recv(c); ... }` — still
racy. Go’s `v, ok := <-c` is the right model.

**M1 `ok` model without tuples:** return codes on a dedicated API:

| Builtin | Behavior |
|---------|----------|
| `chan_recv` | Blocks. On success returns value. On closed+empty returns `0`. |
| `chan_recv_ok` | Returns `1` if last successful recv on **this call** got a value, `0` if EOF. Implemented as: `chan_recv` packs nothing; instead **`chan_recv` returns value, and we add `chan_try_recv` returning special**. |

Simplest shippable API (no ambiguity):

```
// Returns 1 and stores value at *out_ptr if ok; 0 if closed+empty.
// But no pointers-as-out in baga easily for stack...

// Use arena/global slot: NO.

// FINAL M1:
chan_send(c, v) -> i64     // 0 ok, -1 closed
chan_recv(c) -> i64        // value; ONLY call when you know data remains
                           // OR use length protocol
chan_close(c) -> i64
```

Examples will use **known N** fan-out/fan-in (cloud-native batching), not
open-ended stream EOF. Document stream-EOF as M2 (`select` / tuple ok).

## 5. Worker signature

```baga
fn my_task(arg: i64) -> i64 { ... }
let h = go(my_task, arg)
let r = join(h)
```

Checker rules for `go`:

1. Exactly 2 args.
2. Arg0 is `NODE_IDENT` naming a **user function** (not builtin/extern).
3. That function has exactly one parameter of type `i64` and return type `i64`
   (effects on the worker: **must be pure** in M1 — no `!IO` inside worker —
   keeps the runtime trampoline simple). Soften later.
4. Arg1 type is `i64`.
5. Result type `i64` with effect `Par`.

**M1 worker purity:** if the worker declares effects, checker error:
`go: worker must be pure (no effects) in M1`. Cloud handlers that need IO
should do IO on the parent and only parallelize pure compute, **or** wait for
M2 (effectful workers + thread-safe runtime).

Actually HTTP fan-out needs `!IO` in workers. Relax M1:

- Workers **may** have effects; the `go` call’s effect set is `Par ∪ worker_effects`.
- Parent must declare them.
- Document: no memory model; data races on shared `mut` are undefined (same as C).

## 6. Runtime (C backend)

```c
typedef int64_t (*baga_par_fn)(int64_t);

typedef struct {
    baga_par_fn fn;
    int64_t arg;
    int64_t result;
    pthread_t th;
    int joined;
} baga_JoinHandle;

typedef struct {
    int64_t *buf;
    int64_t cap, len, head;
    int closed;
    pthread_mutex_t mu;
    pthread_cond_t not_empty, not_full;
} baga_Chan;

// go → malloc handle, pthread_create, return (int64_t)(uintptr_t)handle
// join → pthread_join, return result, free handle
// channels: classic monitor
```

Link flag: **`-pthread`** always (or detect use — always is simpler; tiny cost).

## 7. LLVM / self-compiler

- **LLVM M1:** unsupported → clear error if `go`/`chan_*` used, or skip in oracle.
- **Self-compiler:** no need to host `go` for bootstrap; examples stay on C bootstrap.

## 8. Cloud-native examples (M1)

1. **Parallel map:** split `Vec<i64>`, `go` per chunk, `join`, merge.
2. **Fan-in:** N workers `chan_send` results; parent `chan_recv` N times.
3. Future M2: per-connection `go` in `httpdbaga` (effectful workers).

## 9. Non-goals (M1)

- Green threads / work-stealing scheduler
- `select`
- Cancellation / context deadlines
- Generic channels (`Chan<T>`)
- Closures
- Shared-memory atomics API (mutex can be M1.1 if needed)

## 10. Naming

- Builtins: `go`, `join`, `chan_new`, `chan_send`, `chan_recv`, `chan_close`
- Effect: `Par` (short; reads as “parallel / concurrent”)
- Module docs: `std/par/` with README + thin wrappers later if needed
- Not trademarking “goroutine” in API names

## 11. Testing

- `examples/par.baga` — `go`/`join` square
- `examples/par_chan.baga` — N producers → one channel → sum
- `tests/std/par_test.baga` in `make test`
- Stress: 32 tasks join correctly

## 12. Key Decisions

1. **OS threads, not green threads** — honest, zero extra runtime, good enough for I/O-bound cloud services at moderate concurrency; CPU-bound still benefits from ~NCPU tasks.
2. **`!Par` effect** — concurrency is visible in the type, fits Baga’s pillars.
3. **Function identifier, not closure** — works today without HOF; upgrade path when closures land.
4. **`i64` channels** — opaque handles and small integers; JSON/strings stay on the parent or as pointers-as-i64 later.
5. **Known-N fan-in for M1 EOF** — avoid Go’s `v, ok` without tuples; M2 adds `recv_ok` or tuples.
6. **Always `-pthread`** — simpler than usage analysis.

## 13. PR Plan

| PR | Scope |
|----|-------|
| PR1 | Design (this doc) + runtime helpers + checker + `-pthread` + `examples/par.baga` |
| PR2 | Channels + fan-in example + `tests/std/par_test.baga` + std/par README |
| PR3 (later) | Effectful workers hardening, mutex, HTTP accept-loop demo |
| PR4 (later) | `select`, cancel, LLVM parity |

M1 lands PR1+PR2 in one push if small enough.
