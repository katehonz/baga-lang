# Storage foundation (S2+S3+S4): bytes mutators, positioned file IO, crc32c — design

Date: 2026-08-05. Status: approved by user (chat — "отива на следващото:
разпределени системи storage"), implementation in progress.
Parent: `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md`
(Track S, sequencing step 1: S2+S3+S4 — small, independent, unlock both
tracks; C1 signals stays with the cloud batch).

## S2 — bytes mutators (httpdbaga G9)

Three new builtins, C backend + honest LLVM unsupported:

```baga
let buf = bytes_new(16)        // zeroed 16-byte buffer
bytes_set(buf, 0, 255)         // buf[0] = 255 — bounds-checked, mutates
let frame = bytes_push(bytes_new(0), 137)   // append → NEW bytes (len+1)
```

- `bytes_new(n: i64) -> bytes` — zeroed buffer, `n ≥ 0`.
- `bytes_set(b: bytes, i: i64, v: i64)` — `b.data[i] = v & 0xFF`;
  `baga_bounds_fail` on OOB (same as `bytes_at`). **Mutates the shared
  buffer — every alias sees the write**, exactly like `Vec`/`Map`
  reference semantics everywhere else in Baga. This is the point (page
  buffers); documented, not hidden.
- `bytes_push(b: bytes, v: i64) -> bytes` — returns a NEW `bytes`
  (fresh arena alloc, len+1, old buffer untouched — aliases keep the
  old one). O(n) copy per push: fine for protocol-frame building (the
  G9 sites are a handful of bytes); a cap-field builder is the later
  optimization, noted not built.
- Runtime: three `baga_bytes_*` helpers emitted by codegen_c next to the
  existing ones (`src/codegen_c.c:1812-1843`); checker rows in the flat
  builtin table (`src/checker.c:1066-1073`); name mapping
  (`src/codegen_c.c:615-620`). LLVM: unmapped → honest
  `llvm_unsupported`, oracle-SKIP-clean, per policy.

## S3 — std/os positioned IO + durability

Pure `std/os/os.baga` additions over the existing extern FFI (params
i64/str only; buffer contract: read-style externs write into a heap
`str_repeat` buffer — the established os.baga idiom):

```baga
extern fn pread(fd: i64, buf: str, count: i64, offset: i64) -> i64 !IO
extern fn pwrite(fd: i64, buf: str, count: i64, offset: i64) -> i64 !IO
extern fn fsync(fd: i64) -> i64 !IO
extern fn fdatasync(fd: i64) -> i64 !IO
extern fn fallocate(fd: i64, mode: i64, offset: i64, len: i64) -> i64 !IO
```

Plus pure-Baga wrappers mirroring `fd_write`/`fd_read`:
`fd_pwrite(fd, s, off) -> i64` (partial-write loop advancing the offset)
and `fd_pread(fd, n, off) -> str` (read up to n at offset). No new
compiler machinery — extern FFI already works in both backends.

## S4 — std/crypto/crc32c.baga (WAL/page checksums)

CRC-32C (Castagnoli, reflected poly `0x82F63B78`), pure Baga, masked-i64
u32 discipline (std/crypto README), table built per call as `Vec<i64>`
(sha256_k idiom; 256 iterations — fine at WAL-record granularity, perf
note documented):

```baga
fn crc32c_update(crc: i64, data: bytes) -> i64   // running state, init 0xFFFFFFFF
fn crc32c_final(crc: i64) -> i64                 // ^ 0xFFFFFFFF
fn crc32c_b(data: bytes) -> i64                  // one-shot = final(update(init, data))
```

Incremental chaining is the WAL shape: `st = crc32c_update(st, chunk)`.

## Honestly out

- cap-field bytes builder (amortized push); io_uring; `extern fn` bytes
  params (G10 — separate); C1 signals (cloud batch); MEM-1+ (next
  milestone per sequencing); perf bench runner (with the MEM milestone).

## Testing

- `tests/std/bytes_mut_test.baga`: new/set/at round-trip, push builds a
  known frame (hex compare), alias semantics of set, push leaves the old
  buffer intact, empty push chain. Runtime-OOB probe for `bytes_set` in
  `scripts/run_tests.sh` (expects the `baga_bounds_fail` message).
- `tests/std/os_io_test.baga`: open O_CREAT|O_RDWR in /tmp → pwrite at
  offsets 0 and 8 → fdatasync + fsync → pread back → content equality;
  fallocate returns 0; wrapper partial-write loop with a >1 chunk write.
- `tests/std/crc32c_test.baga`: published vectors — `"123456789"` →
  `0xE3069283`, empty → 0, iSCSI set (32×0x00 → `0x8A9136AA`, 32×0xFF →
  `0x62A8AB43`, `00..1F` → `0x46DD794E`, `1F..00` → `0x113FDB5C`),
  incremental == one-shot. Oracle: throwaway bitwise python crc32c
  (validated first against the `"123456789"` check value) generates the
  decimals embedded in the test — no crcmod/google-crc32c on the machine.
- Gates: `make test`, `make self`, `make test-llvm` (SKIP-clean).
- Docs: std/os + std/crypto READMEs, bytes section of
  docs/language-{en,bg}.md if builtins are listed there, CHANGELOG,
  httpdbaga gaps G9 → "Unblocked 2026-08-05".
