# Design: io_uring poll backend sketch (Phase 5)

Date: 2026-08-05  
Status: **experiment / sketch** — not production  
Goal: map a path from today’s `SYS_poll` event loop to an optional
Linux **io_uring** backend without claiming drop-in replacement of
`std/net/poll.baga` or fmr accept loops.

## Problem

Product servers (`fmr_run`, chat, registry) use **`poll_wait`**
(`std/net/poll.baga` → `SYS_poll` = 7): one thread, many fds, POLLIN /
POLLERR / POLLHUP. That closes kvbaga K1 / wsbaga W1 and is correct.

Linux 5.1+ offers **io_uring** as a higher-throughput submission /
completion model (batch SQEs, shared rings, `IORING_OP_POLL_ADD`).
Stretch goal from the advanced plan: experiment whether a baga-shaped
API can sit on io_uring later.

## Non-goals (explicit)

- Replacing `poll_wait` in fmr / product paths
- Full liburing-compatible API or SQPOLL / fixed buffers / registered files
- Multi-arch beyond x86-64 Linux (sketch assumes x86-64 syscall numbers)
- Extending baga’s `extern syscall(nr, a1, a2, a3)` arity in this stretch
- Performance claims vs poll on production benches

## Current baga constraints

| Constraint | Impact |
|------------|--------|
| `extern fn syscall(nr, a1, a2, a3) -> i64 !Net` (3 args) | `io_uring_setup` needs 2; **`io_uring_enter` needs 6**; `io_uring_register` needs 5 — cannot call enter from baga today |
| No mmap of ring offsets from baga std | SQ/CQ are shared-memory rings; poll path stages structs via memfd+arena, not `mmap` |
| No liburing on host / no C dep for baga runtime | Experiment stays in `tools/` (Python + raw `syscall`) |
| Default product: poll | Keep green path; opt-in experiment only |

## Goals (sketch)

1. **Feature detect** kernel support: `SYS_io_uring_setup` (425 on x86-64)
   succeeds for small `entries`.
2. **Minimal ring exercise** without liburing:
   - map SQ + CQ + SQEs
   - submit `IORING_OP_NOP`
   - `io_uring_enter` + harvest CQE
3. **Poll-shaped story:** document how `IORING_OP_POLL_ADD` would map to
   `PollResult { ok, ready: Vec<i64> }` (fd via `user_data`).
4. Tool under `tools/iouring/` (Python 3, stdlib only). Smoke:
   `./tools/iouring/test_sketch.sh`.
5. Design honesty: production remains `poll_wait` until baga gains
   multi-arg syscall (or dedicated `extern io_uring_*`) + ring mmap.

## Proposed future baga surface (not implemented)

```baga
// Future — same shape as poll_wait; backend selectable
struct PollResult {
    ok: i64,
    ready: Vec<i64>
}

// Default: SYS_poll (today)
fn poll_wait(fds: Vec<i64>, timeout_ms: i64) -> PollResult !Net !IO

// Opt-in when BAGA_POLL=iouring and kernel+runtime support
fn poll_wait_iouring(fds: Vec<i64>, timeout_ms: i64) -> PollResult !Net !IO
```

### Mapping

| poll.baga | io_uring |
|-----------|----------|
| build `pollfd[]`, `SYS_poll` | `IORING_OP_POLL_ADD` per fd (or multi-shot where available) |
| timeout_ms | linked `IORING_OP_TIMEOUT` or enter timeout |
| revents & (POLLIN\|POLLERR\|POLLHUP) | CQE `res` ≥ 0 with poll mask / POLL_ADD completion |
| ready list of fds | CQE `user_data` = fd (or index into original vec) |

## Runtime prerequisites (before product switch)

1. **Syscall arity:** either `syscall6` extern or thin C helpers:
   `iouring_setup`, `iouring_enter`, `iouring_register`.
2. **Ring mmap:** map SQ ring, CQ ring, SQE array using offsets from
   `io_uring_params` (`IORING_OFF_SQ_RING`, `…_CQ_RING`, `…_SQES`).
3. **Lifecycle:** one ring per accept loop (or process); drain CQ on
   shutdown; never leave POLL_ADD dangling across fd close without remove.
4. **Fallback:** if setup fails → `poll_wait` (feature flag + env).
5. **Verify / effects:** still `!Net !IO`; no change to pure paths.

## Experiment artifact

| Path | Role |
|------|------|
| `tools/iouring/iouring_probe.py` | setup → NOP → optional POLL_ADD on pipe |
| `tools/iouring/test_sketch.sh` | smoke; skip/soft-fail only if kernel lacks uring |
| `tools/iouring/README.md` | how to run; honesty |

## Honesty

This is a **Phase 5 stretch sketch**: prove kernel + layout understanding and
document the baga gap. It does **not** make fmr faster and does not
replace `std/net/poll.baga`. Full backend is a separate design after
syscall/mmap plumbing.

## References

- Linux `include/uapi/linux/io_uring.h` (kernel headers)
- Baga poll: `std/net/poll.baga` (`SYS_poll`, memfd staging)
- Advanced plan Phase 5: `docs/superpowers/plans/2026-08-05-advanced-go-rust.md`
