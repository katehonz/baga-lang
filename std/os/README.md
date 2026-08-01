# std/os — libc syscall wrappers

Thin wrappers over libc, declared with `extern fn`.

- `extern open(path: str, flags: i64, mode: i64) -> i64 !IO` — libc `open(2)`; returns the fd, or -1.
- `extern close(fd: i64) -> i64 !IO` — libc `close(2)`.
- `extern read(fd: i64, buf: str, count: i64) -> i64 !IO` — libc `read(2)` into `buf`.
- `extern write(fd: i64, buf: str, count: i64) -> i64 !IO` — libc `write(2)` from `buf`.
- `extern getenv(name: str) -> str !IO` — libc `getenv(3)`; NULL coerces to `""`.
- `env(name: str) -> str !IO` — environment variable, or `""` when unset.
- `fd_write(fd: i64, data: str) -> i64 !IO` — write all of `data`, looping on partial writes; 0 on success, -1 on error.
- `fd_read(fd: i64, n: i64) -> str !IO` — read up to `n` bytes; `""` at EOF or on error.
- `read_file(path: str) -> str !IO` — read an entire file; `""` on error or for an empty file.
- `write_file(path: str, data: str) -> i64 !IO` — write `data` to a file (O_WRONLY|O_CREAT|O_TRUNC, mode 0644); 0 on success, -1 on error.
- `mem_i64(s: str, off: i64) -> i64` — little-endian u64 load from 8 bytes of `s` at `off`; decodes structs filled by libc (e.g. `struct timespec`). Pure.

Buffer contract: `read`-style externs write into the `str` buffer you pass —
the buffer must be heap-allocated (built by `str_repeat`), never a string
literal.

Effects: !IO. Memory: buffers are heap strings (leak-tolerant); no arena needed.
