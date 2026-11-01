# Baga in November 2026: worlds, a REPL, and storage in the RocksDB class

**Baga** (⚔️) is a language for systems and product code in which the compiler is the **judge**: you write specifications with `requires`/`ensures`/`decreases`, and the static verifier proves or refutes the implementation before anything runs — with a concrete counterexample on refutation. Errors are effects in the type (`!IO`, `?`, `catch`), not exceptions. The compiler compiles itself to a fixed point (`make self`: baga2 == baga3 byte for byte) and has two backends — zero-dependency C and LLVM.

> *"The question is not 'what is new'. The question is 'what has not yet been glued together'."*

This post is about what is new in recent weeks: **worlds** (a new memory model with a machine-checked protocol), **rocksbaga** with an honest benchmark against RocksDB, a REPL, and a wider verifier.

## Worlds: a whole graph of data as a value

The two older memory disciplines have a documented ceiling: RC (`--rc`) leaks on cycles, and the bump arena is thread-local and does not travel across threads. Real server structures, though, are **worlds** — a query plan cache, a document graph, a table snapshot: a whole that is born, frozen, shared, and dies at once.

Worlds in Baga (W0–W4.2, `world_*`/`wvec_*`/`wmap_*`/`wcache_*` builtins in both the C **and** LLVM backends) answer the question: *how do I make an entire owned graph — cycles included — one transferable, freezable, serializable value?*

```
world_new() ──▶ mutable ──world_freeze──▶ frozen ──chan_send──▶ moved
                    │                        │ (readers only,
                    └──world_free──▶ freed   │  zero-copy across threads)
                        (the whole chain at once)
```

The difference from Project Verona / Pony / Cyclone is not the idea but the **verdict**: the protocol is not a convention, it is a conclusion of the verifier. `mutate-after-freeze`, `use-after-free`, and `use-after-move` are refuted with a CONCRETE counterexample, and `--json` turns them into a machine channel for AI agents:

```bash
$ ./baga --verify examples/verify/worlds.baga     # 16 ensures + 10 protocols
$ ./baga --verify --json examples/verify/worlds_cap.baga
```

The oracles are split up: region core, sharing and contracts, capability graphs, delegation, channel rely–guarantee, world blocks — plus `bench/worlds.baga` (streaming) and `bench/worlds_micro.baga` (hot loop in cache), with checksum validation, not just timing.

Worlds already run in the ecosystem's production packages (see [docs/app-worlds-bg.md](../docs/app-worlds-bg.md)): boilaDB statistics and live connections live in a frozen world (`wcache_add`) and the hot path takes no global mutex; `/metrics` in `fmrbaga` and `cloudbaga` is in a frozen world via the new `WMet` in `metbaga`; `queuebaga` read-through caches terminal statuses. boilaDB, rocksbaga, and queuebaga now also pass through LLVM (`tests/llvm_storage.sh`, C↔LLVM parity).

## rocksbaga vs RocksDB — now with honest numbers

**rocksbaga** is a durable LSM KV store in the RocksDB class, written in pure Baga: page cache, crc32c WAL, memtable + L0…L3 compaction, SST (BAGASST5) with bloom sidecar, a RESP2 server with poll multi-conn and MT mode.

The benchmark is head-to-head on the **pure engines** — same keys, values, and op counts, single writer, no RESP/TCP (`bench/rocks/run_vs_rocksdb.sh`, the other side is official RocksDB via Python `rocksdict`):

```bash
BENCH_DIR=/var/tmp/rocksbench BENCH_N=5000 ./bench/rocks/run_vs_rocksdb.sh            # durable
BENCH_DIR=/var/tmp/rocksbench BENCH_N=5000 BENCH_MODE=batch ./bench/rocks/run_vs_rocksdb.sh
```

Latest scorecard (2026-11-01, real disk — btrfs on NVMe, n=5000, vlen=100 — rocksbaga as % of RocksDB ops/s):

| mode | PUT | GET_SEQ | GET_RND |
|------|----:|--------:|--------:|
| durable (fsync per put) | **124%** | **127%** | **114%** |
| batch (bulk load) | **82%** | **142%** | **148%** |

An honest note the runner itself now prints: **by default the bench writes to `/tmp`, which is often tmpfs** — there fsync is a no-op and durable numbers measure RAM, not disk. That is why the harness now supports `BENCH_DIR` on a real disk and warns on tmpfs. Full history: `bench/rocks/results/vs-rocksdb-scorecard.md`.

This is a throughput bar under this harness, not a claim of feature parity with RocksDB — but a pure-Baga LSM in the same league as a decade of C++ storage work is a number worth owning.

## Other new things

- **REPL** — `make repl`: declarations and `let`s persist across lines; handy for teaching and quick language checks.
- **The verifier grows without losing honesty** — M31: while-loop induction (wait-for scan); M32: CDCL clause learning in the bitvector SAT solver, enabled after a formal soundness audit (`docs/cdcl-audit-2026-10-31.md`). Outside the fragment the answer stays UNKNOWN — never falsely proven.
- **Onboarding** — `docs/GETTING-STARTED-bg.md` with machine-checked examples.
- **The ecosystem**: boilaDB (multimodal SQL database on the PG v3 wire), fmrbaga, ormbaga, jwtbaga (RS256/ES256 now deterministic), raftbaga, txnbaga, queuebaga, and dozens more packages under `app-product/`, built with `sandak`.

## Quick start

```bash
git clone git@github.com:katehonz/baga-lang.git && cd baga-lang
git submodule update --init --recursive
make
./baga examples/zdravei.baga        # Hello, warrior. The battle begins.
make repl                            # interactive shell
./baga --verify examples/verify/fact_full.baga   # factorial, fully proven
```

## Honest status

Baga is a **working prototype**, not a production language. The verifier is deliberately small and says UNKNOWN instead of guessing; general nonlinear arithmetic is the remaining staircase. MIT license, code on [GitHub](https://github.com/katehonz/baga-lang), docs in Bulgarian and English, packages under `github.com/bagalang/*`.

---

*Nothing is new. But nothing is timely either. Regional memory is from the 90s — maybe now is the time for it to become a checked discipline, not a convention.*

⚔️
