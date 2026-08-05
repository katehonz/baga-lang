# iouring — Phase 5 poll backend sketch

Experiment for a future **io_uring** path behind the same shape as
`std/net/poll.baga` (`poll_wait` → ready fds).

**Not** production. Product still uses `SYS_poll`.

## Why Python / no liburing

Baga’s net layer only exposes `syscall(nr, a1, a2, a3)`. `io_uring_enter`
needs six arguments and ring **mmap**, so a full backend needs runtime work
first. This probe uses raw syscalls via Python `ctypes` on **x86-64 Linux**
only, with no `liburing` package.

## Usage

```bash
# feature detect only
python3 tools/iouring/iouring_probe.py detect

# setup + NOP CQE
python3 tools/iouring/iouring_probe.py nop

# POLL_ADD on a self-pipe (POLLIN after write)
python3 tools/iouring/iouring_probe.py poll

# all of the above
python3 tools/iouring/iouring_probe.py all

# smoke (skip soft if kernel lacks uring)
./tools/iouring/test_sketch.sh
```

Exit codes: `0` ok, `1` unexpected failure, `2` unsupported arch/kernel
(test script treats `2` on detect as skip).

## Design

See `docs/superpowers/specs/2026-08-05-io-uring-poll-sketch-design.md`.

## Baga gap (honest)

| Need | Status |
|------|--------|
| `io_uring_setup` | probe only |
| ring mmap + SQE/CQE | probe only |
| `poll_wait_iouring` in baga | not started |
| fmr / product switch | not started; default remains poll |
