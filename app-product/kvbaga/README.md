# kvbaga

A **Redis-compatible KV server** for Baga — RESP2 protocol over TCP, backed by
the language's `Map<str, str>`. The first product built deliberately on the new
map type: the app is the probe, the gaps land in [`gaps.md`](gaps.md).

## What works

| Capability | Notes |
|------------|--------|
| RESP2 wire | `*N` + `$len` frames; buffered parse, pipelined commands per read |
| Commands | `PING` `SET [EX s]` `GET` `DEL` `EXISTS` `INCR` `KEYS` `EXPIRE` `TTL` `DBSIZE` `QUIT` |
| Store | `Map<str,str>` values + `Map<str,i64>` deadlines (lazy expiry, Redis-style) |
| TTL | `SET ... EX`, `EXPIRE`, `TTL` (-2 missing / -1 no expiry / seconds) |
| Errors | `-ERR ...` for unknown command / wrong arity / non-integer INCR |
| Concurrency | serial connections, one shared store (Redis-1.x model; see gaps) |
| Idle guard | per-connection `SO_RCVTIMEO` via `tcp_set_timeouts` (default 60 s) |

## API

```baga
struct KvStore { vals: Map<str,str>, expires: Map<str,i64> }

fn kv_new() -> KvStore
fn kv_alive(st, key, now) -> i64      // exists + not expired (drops if expired)
fn kv_set / kv_set_ex / kv_put        // SET clears TTL; kv_put keeps it (INCR)
fn kv_keys(st, now) -> Vec<str>       // live keys only
fn kv_is_int(s) -> i64                // strict integer text

fn kv_exec(st, args) -> bytes !Time   // one command → one RESP reply
fn kv_serve(port) -> i64 !Net !IO !Time !Par   // accept loop, 60s idle timeout
fn kv_serve_to(port, timeout_s) -> i64 !Net !IO !Time !Par

// client helpers (resp.baga)
fn resp_encode_command(args) -> bytes
fn resp_round_trip(fd, args) -> RespReply !Net !IO
```

`resp.baga` is a pure RESP2 codec (server parse + reply builders + client
round-trip) reusable by any future Redis-protocol peer.

## Run

```bash
# server + client demo (boots a worker on :16379, drives it, prints replies)
./baga app-product/kvbaga/demo.baga

# live test (go_bg server on :16481, 27 checks)
./baga tests/kv_test.baga
```

Both are wired into `make test`.

## Honest limits

- **Serial**: one connection at a time. `go()` carries only `i64`, so the
  store cannot move to a second thread — an event loop (`poll`) or a
  request-channel design is the P1 path (gaps.md).
- **Text values**: `str` is NUL-terminated, so binary-safe RESP values do not
  round-trip (`bytes` values later).
- **No persistence** yet (no RDB/AOF); **no AUTH**; `KEYS` is exact (no globs).
- No eviction policy — TTL only.

## Architecture

```
  client (resp_round_trip)
        ↓  RESP2 over TCP
  kv_serve (accept loop, serial)
        ↓
  kv_exec  →  KvStore { vals: Map<str,str>, expires: Map<str,i64> }
```

Same probe model as the other `app-product` packages: ship a working product,
log the language friction.
