# std/os — libc syscall wrappers

Thin wrappers over libc, declared with `extern fn`.

- `extern open(path: str, flags: i64, mode: i64) -> i64 !IO` — libc `open(2)`; returns the fd, or -1.
- `extern close(fd: i64) -> i64 !IO` — libc `close(2)`.
- `extern read(fd: i64, buf: str, count: i64) -> i64 !IO` — libc `read(2)` into `buf`.
- `extern write(fd: i64, buf: str, count: i64) -> i64 !IO` — libc `write(2)` from `buf`.
- `extern pread(fd: i64, buf: str, count: i64, offset: i64) -> i64 !IO` — libc `pread(2)`; positioned read, does not move the fd's file position.
- `extern pwrite(fd: i64, buf: str, count: i64, offset: i64) -> i64 !IO` — libc `pwrite(2)`; positioned write, does not move the fd's file position.
- `extern fsync(fd: i64) -> i64 !IO` — libc `fsync(2)`; flush data + metadata to stable storage.
- `extern fdatasync(fd: i64) -> i64 !IO` — libc `fdatasync(2)`; flush data only.
- `extern fallocate(fd: i64, mode: i64, offset: i64, len: i64) -> i64 !IO` — libc `fallocate(2)`; preallocate/zero file regions.
- `extern getenv(name: str) -> str !IO` — libc `getenv(3)`; NULL coerces to `""`.
- `env(name: str) -> str !IO` — environment variable, or `""` when unset.
- `fd_write(fd: i64, data: str) -> i64 !IO` — write all of `data`, looping on partial writes; 0 on success, -1 on error.
- `fd_read(fd: i64, n: i64) -> str !IO` — read up to `n` bytes; `""` at EOF or on error.
- `fd_pwrite(fd: i64, data: str, off: i64) -> i64 !IO` — write all of `data` at absolute offset `off`, looping on partial writes; 0 on success, -1 on error. Does not move the fd's file position.
- `fd_pread(fd: i64, n: i64, off: i64) -> str !IO` — read up to `n` bytes at absolute offset `off`; `""` at EOF or on error. Does not move the fd's file position.
- `read_file(path: str) -> str !IO` — read an entire file; `""` on error or for an empty file.
- `write_file(path: str, data: str) -> i64 !IO` — write `data` to a file (O_WRONLY|O_CREAT|O_TRUNC, mode 0644); 0 on success, -1 on error.
- `mem_i64(s: str, off: i64) -> i64` — little-endian u64 load from 8 bytes of `s` at `off`; decodes structs filled by libc (e.g. `struct timespec`). Pure.

Buffer contract: `read`-style externs write into the `str` buffer you pass —
the buffer must be heap-allocated (built by `str_repeat`), never a string
literal.

NUL caveat: `fd_read`/`fd_pread` are strlen-based — the returned `str`
truncates at the first NUL byte, so they are only safe for NUL-free data.
For binary reads that may contain NULs, call raw `pread` into a
`str_repeat` buffer and inspect it with `byte_at` (the hole check in
`tests/std/os_io_test.baga` demonstrates this).

Note: `std/net/tcp.baga` externs `pread64`/`pwrite64` instead of
`pread`/`pwrite`. Two same-named body-less externs cannot coexist — both
prototypes are emitted unmangled into the C output and gcc fails with
"conflicting types". On glibc x86-64, `pread64`/`pwrite64` are weak
aliases of the same symbols, so the rename is free.

Effects: !IO. Memory: buffers are heap strings (leak-tolerant); no arena needed.
