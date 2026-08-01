# std/time — wall and monotonic clocks

`clock_gettime(2)` wrappers. `struct timespec` on LP64 Linux is
`{ i64 tv_sec; i64 tv_nsec; }` — 16 bytes, decoded with `mem_i64` (std/os).

- `extern clock_gettime(clk: i64, ts: str) -> i64 !Time` — libc `clock_gettime`; `ts` is a 16-byte heap buffer.
- `clock_ms(clk: i64) -> i64 !Time` — clock `clk` (0 = CLOCK_REALTIME, 1 = CLOCK_MONOTONIC) in milliseconds; -1 on error.
- `time_now_ms() -> i64 !Time` — wall clock (CLOCK_REALTIME) in milliseconds since the epoch.
- `monotonic_ms() -> i64 !Time` — monotonic clock (CLOCK_MONOTONIC) in milliseconds.

Effects: !Time. Memory: buffers are heap strings (leak-tolerant); no arena needed.
