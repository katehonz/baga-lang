# std/par — concurrency for cloud-native Baga

Go-inspired **tasks + channels**, Baga-shaped:

| Idea | Baga |
|------|------|
| goroutine | `go(fn, arg) -> handle !Par` — **OS thread** (`pthread`) |
| fire-and-forget | `go_bg(fn, arg) !Par` — detached; no join (accept loops) |
| Wait / result | `join(handle) -> i64 !Par` |
| detach later | `detach(handle) !Par` — race-safe with join |
| channel | `chan_*` over **`i64`** (ids, handles, small payloads) |
| mutex | `mutex_new` / `mutex_lock` / `mutex_unlock` |
| effect | `!Par` — concurrency is a type dimension |

These are **language builtins** (always available; no import). Design:
[`docs/superpowers/specs/2026-08-02-par-concurrency-design.md`](../../docs/superpowers/specs/2026-08-02-par-concurrency-design.md).

## API

```baga
fn worker(x: i64) -> i64 { return x * x }

fn main() -> i64 !Par {
    let h = go(worker, 7)
    return join(h)   // 49
}
```

| Builtin | Signature | Notes |
|---------|-----------|-------|
| `go` | `(fn, arg: i64) -> i64 !Par` | Function **name**; returns join handle. |
| `go_bg` | `(fn, arg: i64) -> i64 !Par` | Detached thread; returns 0. For accept loops. |
| `join` | `(h: i64) -> i64 !Par` | Blocks; returns worker result; frees handle. |
| `detach` | `(h: i64) -> i64 !Par` | Fire-and-forget a previously `go`'d handle. |
| `chan_new` | `(cap: i64) -> i64 !Par` | Buffered `i64` channel (cap &lt; 1 → 1). |
| `chan_send` | `(c: i64, v: i64) -> i64 !Par` | 0 ok, -1 closed. |
| `chan_recv` | `(c: i64) -> i64 !Par` | Blocks; closed+empty → 0 (ambiguous with payload 0). |
| `chan_recv2` | `(c: i64) -> i64 !Par` | Returns `cell2(ok, value)` — ok=1 got value, ok=0 EOF. |
| `chan_close` / `chan_len` | `!Par` | |
| `mutex_new` / `lock` / `unlock` | `!Par` | Opaque `i64` handle. |
| `cell2` / `cell2_0` / `cell2_1` | pure | Heap pair for worker context. |

Worker signature: `fn(i64) -> i64`. Effects on the worker **bubble** to the
`go`/`go_bg` site (so HTTP handlers can be `!IO !Net`).

## Cloud-native patterns

**Accept loop (httpdbaga):**

```baga
fn handle_conn(fd: i64) -> i64 !IO !Net {
    handle(fd)?
    tcp_close(fd)?
    return 0
}
// in main:
go_bg(handle_conn, fd)
```

**Fan-out + join:**

```baga
let h1 = go(part, a)
let h2 = go(part, b)
let r = join(h1) + join(h2)
```

**Fan-in with clean EOF:**

```baga
let p = chan_recv2(c)
if cell2_0(p) == 0 { /* closed */ }
let v = cell2_1(p)
```

## Honesty / limits

- **Not green threads** — each `go` is a real OS thread.
- **No closures** — pack state with `cell2` / ids.
- **No memory model** — races on shared `mut` are undefined; prefer channels.
- **LLVM** — C backend only; oracle SKIPs par examples.

## Examples / tests

- `examples/par.baga`, `examples/par_chan.baga`
- `tests/std/par_test.baga`
- `app-product/httpdbaga/server.baga` — concurrent by default
