# raftbaga — gaps

## R1 — PUT path drops concurrent RPCs

While the leader waits for AE_REP after a client PUT, other message types
(RequestVote, extra AE, client GET) may be received and ignored.

**Severity.** Medium under load; fine for sequential client tests.

**Path.** Small inbound queue (Vec) drained each loop iteration.

## R2 — durable Raft log (lite — B4.2)

**Shipped.** `persist.baga` writes term / voted_for / commit_idx / log to
`/tmp/baga_raft_<id>.state`. Nodes load on start and re-apply committed
entries; flush on vote/log/commit/stop. `raft_start` wipes files for a clean
cluster.

**Residual.** Not fsynced on every entry (best-effort `write_file`); no
stable storage path config; not lsmbaga-backed. Kill mid-write can corrupt
the text file (no CRC).

## R3 — N fixed at 3

Peer table is nested `cell2` for three inboxes. General N needs a different
handle table (or a network router thread).

## R4 — full Raft not in `--verify`

By design. Pure rules in `rules.baga` are proven; the concurrent system is
a runtime exam only.

## Language

- `go` / channels carry `i64` only — message trees via nested `cell2` (M17).
- No `vec_pop` — log truncate rebuilds via `vec_set` + `log_len`.
