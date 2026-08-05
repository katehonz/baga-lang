# raftbaga — gaps

## R1 — PUT path drops concurrent RPCs

While the leader waits for AE_REP after a client PUT, other message types
(RequestVote, extra AE, client GET) may be received and ignored.

**Severity.** Medium under load; fine for sequential client tests.

**Path.** Small inbound queue (Vec) drained each loop iteration.

## R2 — no durable Raft log

Log lives in process memory. Process kill loses uncommitted and committed
state. Pairing with lsmbaga for durable log is S8-adjacent product work.

## R3 — N fixed at 3

Peer table is nested `cell2` for three inboxes. General N needs a different
handle table (or a network router thread).

## R4 — full Raft not in `--verify`

By design. Pure rules in `rules.baga` are proven; the concurrent system is
a runtime exam only.

## Language

- `go` / channels carry `i64` only — message trees via nested `cell2` (M17).
- No `vec_pop` — log truncate rebuilds via `vec_set` + `log_len`.
