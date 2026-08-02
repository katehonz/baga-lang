# std — the Baga standard library

Minimal, zero-dependency standard library. Each library lives in its own
folder and is included with textual imports, e.g.
`import "std/str/str.baga"` (paths resolve relative to the importing file,
then the working directory; every file is included at most once).

| Module  | Contents                                            | Effects      |
|---------|-----------------------------------------------------|--------------|
| str     | split, find, replace, join, trim, repeat, parse_int | pure         |
| bytes   | byte buffers, hex, base64, base64url                | pure         |
| sort    | quicksort, binary search for Vec<i64>               | pure         |
| json    | JSON parser + serializer                            | pure         |
| os      | env, write_file, fd_read/fd_write, mem_i64          | !IO          |
| time    | time_now_ms, monotonic_ms                           | !Time        |
| random  | random_bytes, random_i64                            | !Random      |
| io      | buffered Reader/Writer over fds                     | !IO          |
| net     | tcp_listen/accept/connect/read/write/close          | !Net (+!IO)  |
| crypto  | sha256, hmac_sha256, ct_eq                          | pure         |
| par     | go/join tasks, i64 channels (CSP) — see std/par/    | !Par         |

## Memory policy

Pure modules follow the language's leak-tolerant default (malloc, never
freed). Programs that need bounded memory use the arena builtins:
`arena_new` / `arena_alloc` / `arena_reset` / `arena_free` — one arena per
request/connection, `arena_reset` (or `arena_free`) at the end of each cycle.
String buffers created by `str_repeat` are heap-allocated and may be
overwritten in place by libc externs that take a `str` buffer (never do this
with string literals).

## Effects policy

Every std function declares its exact effects: `!IO` for file/fd operations,
`!Net` for sockets, `!Random` for randomness, `!Time` for clock reads,
`!Par` for tasks/channels (`go`/`join`/`chan_*` are language builtins —
documented under `std/par/`). Pure modules have no effects — visible purity
in the type.
