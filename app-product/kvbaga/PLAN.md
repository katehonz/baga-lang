# kvbaga — Redis-compatible KV server (plan)

Date: 2026-08-03
Status: P0 done (RESP2 subset on Map<str,str>, live-tested)
Goal: the first product on the language's new `Map<K,V>` type — a real server,
not a demo of the type. The app is the probe: friction lands in gaps.md.

## Why KV first

A KV store is the minimal product where a map type is load-bearing (every
command touches it), and RESP is a small honest wire protocol — the same
"stable low-level core, product on top" layering as pgbaga→ormbaga.

## Phases

### P0 — MVP (this iteration) ✅

1. `resp.baga` — RESP2 codec: buffered command parse, reply builders,
   client round-trip.
2. `store.baga` — `KvStore` = `Map<str,str>` values + `Map<str,i64>`
   deadlines; lazy expiry; strict integer check for INCR.
3. `server.baga` — serial accept loop (`kv_serve` for `go_bg`), dispatch:
   PING SET(EX) GET DEL EXISTS INCR KEYS EXPIRE TTL DBSIZE QUIT.
4. `demo.baga` boots a worker and drives it; `tests/kv_test.baga` — 27
   live checks (both wired into `make test`).

### P1 — usable at work (next)

- Event loop via `poll(2)`: adopt `std/net/poll` like chatbaga (K1 path
  open — language/primitive done; kv_serve still serial).
- Persistence: SAVE/LOAD snapshot (line or RDB-lite format) via write_file.
- AUTH (password from env), multi-DB index (SELECT n).
- KEYS glob matching (K5 → std/str fnmatch).

### P2 — framework scale

- bytes values in the store (`Map<str, bytes>` — language kind exists).
- Replication stream / SUBSCRIBE-style notifications (channels of str?).
- Eviction policies beyond TTL (allkeys-lru needs ordered metadata).
- Benchmark rig vs real redis-server (bench/).

## Non-goals (P0)

Cluster/slots, Lua scripting, streams, full RESP3, binary values, TLS.

## Files

| File | Role |
|------|------|
| `resp.baga` | pure RESP2 codec (server + client) |
| `store.baga` | Map-backed store + TTL semantics |
| `server.baga` | dispatch + accept loop (`kv_serve`) |
| `demo.baga` | boot + client walkthrough |
| `gaps.md` | language / protocol gaps (K1–K5) |
| `tests/kv_test.baga` | live loopback test (in repo root tests/) |

## Success criteria (P0) — met

1. `./baga tests/kv_test.baga` prints `kv_test: all passed` (27 checks).
2. The store is a real `Map` — no Vec/index workarounds anywhere.
3. Gaps logged honestly (serial model, text-only values, exit-code trap K3).
