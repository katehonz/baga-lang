# raftbaga (S7): leader election + log replication over channels

Date: 2026-08-05. Status: **shipped** (MVP).
Parent: `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md`

## Goal

The **distributed exam** for Track S: a working Raft fragment in pure Baga
using CSP (`go` / channels / timeouts), not shared mutable state across
nodes. Prove what the M14–M16 fragment can (local term rules, channel
payload shape); honestly leave full Raft safety as UNKNOWN.

## Architecture (in-process cluster)

```
  client  ──CLIENT_PUT/GET──►  node inboxes (×N)
                                  │
                                  │ AppendEntries / RequestVote
                                  ▼
                              peer inboxes (same set)

  Each node = one go_bg loop with *local* state only:
    term, voted_for, role, log (terms+cmds), commit, store Map
```

- **N = 3** fixed in MVP (majority = 2). Enough for election + failover demo.
- `go` carries only `i64` → node ctx is nested `cell2` of ids + channel
  handles (M17 packing).
- Messages are nested `cell2` trees (type, term, from, args…).
- No TCP in v1 — the probe is the **protocol + concurrency**, not the wire.
  (Wire later: jsonrpcbaga / pbbaga.)

## Roles & timeouts

| Role | Behaviour |
|------|-----------|
| Follower | On `RequestVote` / `AppendEntries`; election timeout → Candidate |
| Candidate | Bump term, vote self, broadcast RequestVote; majority → Leader |
| Leader | Heartbeats (empty AE); client puts → append log → replicate → commit |

- Election timeout: `chan_recv_timeout(inbox, base_ms + id*jitter)`.
- Heartbeat interval ≪ election timeout (e.g. 40 ms vs 150–250 ms).

## Log & state machine

- Log: parallel `Vec<i64>` terms + cmds (cmd = packed `cell2(key,val)` for PUT).
- Index 0 reserved (empty sentinel term 0) so prev_index=0 is always valid.
- Apply on commit advance: PUT → local `Map<i64,i64>` store.
- GET is leader-only read of applied store (MVP; no linearizable read index).

## Message types

```
1 RV      term from last_idx last_term
2 RV_REP  term from vote_granted
3 AE      term from prev_idx prev_term leader_commit entry_term entry_cmd
          (single-entry append; empty entry_term=0 → heartbeat)
4 AE_REP  term from success match_idx
5 PUT     key val reply_ch
6 GET     key reply_ch
7 STOP
```

Client reply on `reply_ch`: `cell2(ok, value)` — ok=1 success, 0 not-leader / err.

## Client API

```baga
fn raft_start() -> RaftCluster !Par        // 3 nodes
fn raft_put(c, key, val) -> i64 !Par       // 0 ok, -1 fail (retry internally)
fn raft_get(c, key) -> i64 !Par            // value or -999999 sentinel miss
fn raft_leader(c) -> i64 !Par              // best-effort id via probe, or -1
fn raft_stop(c) -> i64 !Par
```

`raft_put` broadcasts PUT to all inboxes; only the leader appends and
replies. Client waits on a private reply channel with timeout + retry.

## Verify fragments (honest)

Shipped as pure functions with `--verify` (not the full concurrent system):

1. **Term monotonicity** — accepting a higher term always yields
   `new_term > old_term` (and demotion to follower).
2. **Vote eligibility** — `can_vote(term, voted_for, cand_term, cand_id)` pure rules.
3. Optional channel invariant demo: outbound vote terms `>= 1`.

Full Raft (election safety, log matching across crashes) stays **UNKNOWN**
in the verifier — stated in README and thesis-open-problems spirit.

## Out of scope

- Persistent log / fsync (lsmbaga owns durability; Raft log is in-memory MVP)
- Cluster membership change, snapshots, pre-vote
- Network partitions as first-class (can sleep/delay later)
- N ≠ 3

## Tests

- `tests/raft_test.baga`: elect → put/get → stop one leader path via STOP
  all; multi-put; follower has applied value after replication wait.
- `examples/verify/raft_term.baga`: term/vote pure specs ДОКАЗАНО.
- `demo.baga`: print leader path + replicated KV.

## Success

1. `sandak build` ok.
2. `raft_test` green under baga-test.
3. `--verify examples/verify/raft_term.baga` proves the local fragments.
4. CHANGELOG + cloud plan S7 marked shipped.
