# Storage Foundation (S2+S3+S4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the storage-track foundation: bytes mutators (`bytes_new`/`bytes_set`/`bytes_push`), positioned/durable file IO in std/os (`pread`/`pwrite`/`fsync`/`fdatasync`/`fallocate`), and `std/crypto/crc32c.baga`.

**Architecture:** S2 = three runtime helpers + checker/codegen builtin rows (C backend, LLVM honest unsupported). S3 = pure std/os extern FFI + Baga wrappers (no compiler changes). S4 = pure-Baga CRC-32C in std/crypto (no compiler changes).

**Tech Stack:** Baga compiler (`src/checker.c`, `src/codegen_c.c`), std (.baga), tests via `scripts/baga-test` + grep probes in `scripts/run_tests.sh`.

**Spec:** `docs/superpowers/specs/2026-08-05-storage-foundation-design.md`

## Global Constraints

- No git commits by implementers — the controller commits after user confirmation. Skip every commit step.
- Every task ends green: `make -j4 && make test`; `make self` + `make test-llvm` at the end.
- `bytes_set` mutates the shared buffer (aliases see writes — Vec/Map semantics); `bytes_push` returns a NEW bytes (old buffer untouched). Document, don't hide.
- LLVM: new builtins stay unmapped → honest `llvm_unsupported` (oracle SKIP-clean). No silent zeros anywhere.
- std/crypto discipline: masked-i64 u32 (`& 4294967295`), no u32 type; bitwise ops parenthesized against comparisons (G12).
- Test idiom: local `check(name, ok)` printing `ok <name>` / FAIL + exit(1), final `<name>: all passed`.

---

### Task 1 (S2): bytes mutators — `bytes_new` / `bytes_set` / `bytes_push`

**Files:**
- Modify: `src/checker.c` (flat builtin table ~:1066-1073)
- Modify: `src/codegen_c.c` (name→runtime mapping ~:615-620; emitted helpers near :1812-1843)
- Create: `tests/std/bytes_mut_test.baga`
- Modify: `scripts/run_tests.sh` (one OOB probe in the probes section)

**Interfaces:**
- Produces: builtins `bytes_new(i64) -> bytes`, `bytes_set(bytes, i64, i64) -> void`, `bytes_push(bytes, i64) -> bytes` — pure (no !IO), like the other bytes builtins.

- [ ] **Step 1: checker rows**

In the flat builtin table (`src/checker.c:1066-1073`), mirror the existing `bytes_at`/`bytes_concat` rows and add: `bytes_new` (ret bytes, 1 param), `bytes_set` (ret void, 3 params), `bytes_push` (ret bytes, 2 params). Read how the table encodes param types for the bytes builtins (there may be a param-kind column or a separate check site — grep for `bytes_at` in checker.c to find every place that special-cases it) and replicate for the new names. If arg types are checked positionally somewhere (e.g. "bytes arg must be bytes"), add the new names there too.

- [ ] **Step 2: runtime helpers (`src/codegen_c.c`, next to `baga_bytes_concat` ~:1822-1824)**

```c
static baga_bytes baga_bytes_new(int64_t n) {
    if (n < 0) n = 0;
    baga_bytes b; b.len = n; b.data = baga_alloc(n > 0 ? n : 1);
    memset(b.data, 0, (size_t)(n > 0 ? n : 1));
    return b;
}
static void baga_bytes_set(baga_bytes b, int64_t i, int64_t v) {
    if (i < 0 || i >= b.len) baga_bounds_fail(i, b.len);
    b.data[i] = (unsigned char)(v & 0xff);
}
static baga_bytes baga_bytes_push(baga_bytes b, int64_t v) {
    baga_bytes r; r.len = b.len + 1; r.data = baga_alloc(r.len);
    memcpy(r.data, b.data, (size_t)b.len);
    r.data[b.len] = (unsigned char)(v & 0xff);
    return r;
}
```
(Check the exact `baga_bounds_fail` signature/order at the existing `baga_bytes_at` helper and match it. Emit the helpers unconditionally alongside the others.)

- [ ] **Step 3: name mapping (`src/codegen_c.c:615-620`)**

Add `bytes_new → baga_bytes_new`, `bytes_set → baga_bytes_set`, `bytes_push → baga_bytes_push` to the builtin-name→C-symbol table. Verify call emission passes args positionally like `bytes_at`.

- [ ] **Step 4: `tests/std/bytes_mut_test.baga`**

```baga
// bytes_mut_test.baga — S2: bytes mutators (httpdbaga G9).

import "std/str/str.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() {
    let buf = bytes_new(4)
    check("new_zeroed", bytes_len(buf) == 4 && bytes_at(buf, 3) == 0)
    bytes_set(buf, 0, 255)
    bytes_set(buf, 1, 300)           // маскира се до 44
    check("set_roundtrip", bytes_at(buf, 0) == 255 && bytes_at(buf, 1) == 44)

    // alias semantics: set е видим през алиаса (като Vec/Map)
    let alias = buf
    bytes_set(alias, 2, 7)
    check("set_alias", bytes_at(buf, 2) == 7)

    // push: нов bytes, старият не се пипа
    let f = bytes_push(bytes_push(bytes_new(0), 137), 1)
    check("push_build", bytes_len(f) == 2 && bytes_at(f, 0) == 137 && bytes_at(f, 1) == 1)
    let g = bytes_push(f, 2)
    check("push_new_buf", bytes_len(g) == 3 && bytes_len(f) == 2)
    check("push_hex", hex_encode(g) == "890102")

    // frame building — G9 идиомът (be16/be32 без Vec round-trip)
    let frame = bytes_push(bytes_push(bytes_push(bytes_push(bytes_new(0), 0), 8), 0), 1)
    check("frame", hex_encode(frame) == "00080001")

    print("bytes_mut_test: all passed")
}
```

- [ ] **Step 5: run**

`./baga tests/std/bytes_mut_test.baga` → all ok + `bytes_mut_test: all passed`.

- [ ] **Step 6: OOB probe in `scripts/run_tests.sh` (probes section)**

```bash
printf 'fn main() {\n    let b = bytes_new(2)\n    bytes_set(b, 5, 1)\n}\n' > /tmp/baga_bytes_oob.baga
run /tmp/baga_bytes_oob.baga 2>&1 | grep -q "излизане извън границите" \
	&& echo "OK: S2 — bytes_set извън границите е хванат" \
	|| { echo "FAIL: bytes_set OOB трябва да гърми"; exit 1; }
```
(First run the failing program once and copy the EXACT `baga_bounds_fail` message into the grep string.)

- [ ] **Step 7: gates** — `make -j4 && make test` green (baga-test discovers the new file automatically).

---

### Task 2 (S3): std/os positioned IO + durability

**Files:**
- Modify: `std/os/os.baga` (externs after :13; wrappers after `fd_read` :40)
- Create: `tests/std/os_io_test.baga`

**Interfaces:**
- Consumes: nothing from Task 1. Produces: `pread`/`pwrite`/`fsync`/`fdatasync`/`fallocate` externs + `fd_pwrite(fd, s, off) -> i64` / `fd_pread(fd, n, off) -> str` wrappers (all `!IO`).

- [ ] **Step 1: externs (`std/os/os.baga`, after line 13)**

```baga
extern fn pread(fd: i64, buf: str, count: i64, offset: i64) -> i64 !IO
extern fn pwrite(fd: i64, buf: str, count: i64, offset: i64) -> i64 !IO
extern fn fsync(fd: i64) -> i64 !IO
extern fn fdatasync(fd: i64) -> i64 !IO
extern fn fallocate(fd: i64, mode: i64, offset: i64, len: i64) -> i64 !IO
```

- [ ] **Step 2: wrappers (after `fd_read`, :40)**

```baga
// Write all of `data` at absolute offset, looping on partial writes.
// Returns 0, or -1. Не мести файловата позиция на fd (pread/pwrite семантика).
fn fd_pwrite(fd: i64, data: str, off: i64) -> i64 !IO {
    let total = len(data)
    let mut done: i64 = 0
    while done < total {
        let w = pwrite(fd, substr(data, done, total), total - done, off + done)?
        if w <= 0 { return -1 }
        done = done + w
    }
    return 0
}

// Read up to n bytes at absolute offset. "" at EOF or on error.
fn fd_pread(fd: i64, n: i64, off: i64) -> str !IO {
    let buf = str_repeat(" ", n)
    let got = pread(fd, buf, n, off)?
    if got <= 0 { return "" }
    return substr(buf, 0, got)
}
```

- [ ] **Step 3: `tests/std/os_io_test.baga`**

```baga
// os_io_test.baga — S3: positioned IO + durability (pread/pwrite/fsync).

import "std/os/os.baga"
import "std/str/str.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() -> i64 !IO {
    let path = "/tmp/baga_os_io_test.bin"
    // O_RDWR|O_CREAT|O_TRUNC = 66, mode 0644 = 420
    let fd = open(path, 66, 420)?
    check("open", fd >= 0)

    check("pwrite_at_0", fd_pwrite(fd, "аабб", 0) == 0)
    check("pwrite_at_8", fd_pwrite(fd, "вв", 8) == 0)
    check("fdatasync", fdatasync(fd)? == 0)
    check("fsync", fsync(fd)? == 0)

    check("pread_at_0", fd_pread(fd, 4, 0) == "аабб")
    check("pread_at_8", fd_pread(fd, 2, 8) == "вв")
    check("pread_hole", fd_pread(fd, 2, 4) == concat(chr(0), chr(0)))
    check("pread_eof", fd_pread(fd, 4, 100) == "")

    check("fallocate", fallocate(fd, 0, 0, 4096)? == 0)

    // файловата позиция не е помръднала: обикновен read чете от 0
    check("pos_untouched", fd_read(fd, 2) == "аа")

    close(fd)
    print("os_io_test: all passed")
    return 0
}
```

- [ ] **Step 4: run + gates**

`./baga tests/std/os_io_test.baga` → all ok; `make test` green. (fd 1 = stdout writes in os_test stay green — no shared state.)

---

### Task 3 (S4): std/crypto/crc32c.baga

**Files:**
- Create: `std/crypto/crc32c.baga`
- Create: `tests/std/crc32c_test.baga`
- Modify: `std/crypto/README.md` (one line in the file list)

**Interfaces:**
- Produces: `crc32c_update(crc: i64, data: bytes) -> i64` (state, init `4294967295`), `crc32c_final(crc: i64) -> i64`, `crc32c_b(data: bytes) -> i64`.

- [ ] **Step 1: oracle — generate + validate vectors**

Run a throwaway bitwise python CRC-32C (reflected poly 0x82F63B78) and FIRST assert the published check value `crc32c("123456789") == 0xE3069283`; then print decimals for: `"123456789"`, `""`, `"The quick brown fox jumps over the lazy dog"` (expect 0x22620404), 32×0x00 (expect 0x8A9136AA), 32×0xFF (expect 0x62A8AB43), bytes 0..31 (expect 0x46DD794E), bytes 31..0 (expect 0x113FDB5C). Paste the script and output into the task report; embed the decimals in the Baga test.

```python
def crc32c(data):
    poly = 0x82F63B78
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF
assert crc32c(b"123456789") == 0xE3069283
```

- [ ] **Step 2: `std/crypto/crc32c.baga`**

```baga
// crc32c.baga — CRC-32C (Castagnoli, reflected poly 0x82F63B78).
// WAL/page checksums. Masked-i64 u32 discipline (std/crypto README).
// Таблицата се строи при всяко извикване (256 итерации) — ОК на ниво
// WAL record; ако профилът покаже гореща точка, кеширайте в обвивка.

fn u32(x: i64) -> i64 {
    return x & 4294967295
}

fn crc32c_table() -> Vec<i64> {
    let t = vec_new()
    let poly = 2197175160
    for i in 0..256 {
        let mut c = i
        for k in 0..8 {
            if (c & 1) == 1 {
                c = u32((c >> 1) ^ poly)
            } else {
                c = c >> 1
            }
        }
        vec_push(t, c)
    }
    return t
}

// Running CRC state over a chunk; init with 4294967295 (0xFFFFFFFF).
// Chain: st = crc32c_update(st, next_chunk).
fn crc32c_update(crc: i64, data: bytes) -> i64 {
    let t = crc32c_table()
    let mut c = crc
    for i in 0..bytes_len(data) {
        c = u32(vec_get(t, (c ^ bytes_at(data, i)) & 255) ^ (c >> 8))
    }
    return c
}

fn crc32c_final(crc: i64) -> i64 {
    return u32(crc ^ 4294967295)
}

fn crc32c_b(data: bytes) -> i64 {
    return crc32c_final(crc32c_update(4294967295, data))
}
```

- [ ] **Step 3: `tests/std/crc32c_test.baga`**

check-idiom file with: the seven one-shot vectors from Step 1 (hex_decode("00"×32) etc. for the binary ones — `hex_decode` exists; build 0..31 via a bytes_push chain or `bytes_from_vec`), incremental `"1234"+"56789"` == one-shot, and `crc32c_final`/`update` state chaining over 3 chunks. End `crc32c_test: all passed`.

- [ ] **Step 4: run + gates**

`./baga tests/std/crc32c_test.baga` green; `make test` green. Add the file line to `std/crypto/README.md`.

---

### Task 4: docs + CHANGELOG + gaps

**Files:**
- Modify: `std/os/README.md`, `std/crypto/README.md`
- Modify: `docs/language-en.md`, `docs/language-bg.md` (bytes builtins list — find the §bytes section; add the 3 mutators + alias semantics note)
- Modify: `CHANGELOG.md` ([Unreleased] top: `### Storage foundation (S2–S4)` — mirror previous entries' style)
- Modify: `app-product/httpdbaga/gaps.md` (G9 → "**Unblocked 2026-08-05** — `bytes_new`/`bytes_set`/`bytes_push` shipped; h2/hpack builder migration optional.")

**Steps:**
- [ ] **Step 1:** std/os README — new externs + wrappers + buffer contract reminder.
- [ ] **Step 2:** language docs bytes section — `bytes_new`/`bytes_set`/`bytes_push` with the alias/new-buffer semantics, both languages in sync; note LLVM honest unsupported if that section tracks backend support.
- [ ] **Step 3:** CHANGELOG entry (S2 builtins + semantics, S3 externs+wrappers, S4 crc32c + API, tests, spec link).
- [ ] **Step 4:** httpdbaga G9 verdict.
- [ ] **Step 5:** final gates: `make test && make self && make test-llvm` (SKIP-clean).

---

## Self-Review Notes

- Spec coverage: S2 ✓ (Task 1), S3 ✓ (Task 2), S4 ✓ (Task 3), docs/gaps ✓ (Task 4). Honestly-outs are exclusions, not gaps.
- Type consistency: builtin signatures identical across checker rows, test usage, docs. `fd_pwrite`/`fd_pread` names match between os.baga and the test.
- Tasks 1-3 are file-independent (compiler vs std/os vs std/crypto) but stay sequential per SDD (no parallel implementers).
