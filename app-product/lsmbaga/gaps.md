# lsmbaga — language & storage gaps

Probe log from building the Track S flagship on S2–S4 + MEM-1/2.

## L1 — struct scalar fields do not mutate through by-value params

**Symptom.** `fn f(pc: PageCache) { pc.hand = 1 }` does not update the
caller; Maps inside the struct do (shared). Vec field *reassignment*
(`pc.keys = nk`) also does not stick; `vec_push` on the shared vec does.

**Workaround.** Return-updated struct (`fn f(pc) -> PageCache` / `PcGet`
wrappers). Documented in page.baga.

**Severity.** Medium ergonomics tax for every engine mutator.

## L2 — no `rmdir`; cleanup leaves empty dirs

**Symptom.** Tests can `mkdir` but not remove the directory after
`unlink` of children (only `unlink` for files).

**Workaround.** Prefer path **prefix** files (no directory), like queuebaga.

## L3 — SST full-file load on every get

**Symptom.** `sst_get` loads and parses each SST (page cache warms pages
but parse still walks all rows). Fine at probe scale; not a real engine
read path.

**Path.** Block index + restart keys; keep page cache for block bodies.

## L4 — TTL / binary values / concurrent writers

Same product residuals as kvbaga (K1 poll migration, K2 `Map<str,bytes>`,
no EXPIRE column). Out of MVP scope by design.

## Closed by this package

- Binary file IO with embedded NUL — `fd_*_bytes` (std/os).
- `mkdir` / `unlink` / `rename` — std/os (queuebaga Q4 path open).
- Durable put across reopen — WAL + MANIFEST + SST.
