# chatbaga — language & protocol gaps

Probe log from apps-roadmap №3 (chat layer on wsbaga + poll). Same shape
as kvbaga/wsbaga/pgbaga.

## Closed by this product

- **W1 / K1 — serial accept.** `poll_wait` + one-thread state closes the
  "one connection at a time" wall for chat. kvbaga can adopt the same loop
  without a language change.
- **K2 (partial) — binary map values.** `Map<i64, bytes>` residual buffers
  (and general `Map` bytes values) land in the checker/C runtime. kvbaga can
  store binary RESP values when it chooses to migrate off `Map<str,str>`.

## C1 — no presence / roster query

**Symptom.** Clients learn who joined/left only through push events. There
is no `{"type":"who"}` → member list, so a late joiner does not see who is
already in the room.

**Workaround.** Application-level; store names are already in `st.names` /
`st.rooms` and can be walked.

**Severity.** Low for the probe; medium for a usable chat product.

**Verdict.** Small P1: walk maps and reply with a JSON array (needs either
hand-built arrays or a tiny list encoder).

## C2 — hand-rolled JSON builders (json_escape is quoted)

**Symptom.** `json_escape` in std returns a full JSON string literal
(including the surrounding quotes). Easy to double-quote by mistake when
building objects by concat.

**Workaround.** Documented in `chat.baga` (`jpair` uses escape directly as
the value token). No bare `"` wrappers around escaped strings.

**Severity.** Low — footgun, not a blocker.

**Verdict.** Optional: `json_escape_raw` (contents only) or a real object
builder in std/json when more products need it.

## C3 — broadcast is O(rooms) full key walk

**Symptom.** `chat_broadcast` walks every fd in `st.rooms` and filters by
room name. Fine for tens/hundreds of connections; not for tens of thousands.

**Workaround.** Acceptable for P0 probe scale.

**Severity.** Medium at scale.

**Verdict.** Secondary index `Map<str, Vec<i64>>` room→fds (or a room
struct) when a product needs it — pure app-level, no language change.

## Inherited from wsbaga (still open)

- **W2** — no fragmented message reassembly (FIN=0 closes).
- **TLS** — shared std/net gap (G6).
- No `permessage-deflate`, no close-status interpretation beyond echo.

## Fine / deliberate

- One poll thread owns all maps — no shared-mutable store across `go()`.
- Invalid JSON / msg-before-join / unknown type → `error` frames, connection
  stays up (close only on bad frames / EOF / explicit close).
- Moving rooms announces `left` on the old room before join on the new one.
