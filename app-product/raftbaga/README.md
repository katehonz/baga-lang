# raftbaga

**Raft fragment** for Baga — Track S step **S7** (the distributed exam).
Three in-process nodes, CSP only (`go` / channels / `chan_recv_timeout`),
no shared mutable state between nodes. Leader election + single-entry log
replication + i64 KV apply.

The verifier does **not** claim full Raft safety. It proves local pure
fragments (`rules.baga`: term adoption, vote grant, log up-to-date,
majority, prev-log match) via `--verify`. Everything concurrent is a
runtime probe.

## What works

| Piece | Notes |
|-------|--------|
| Cluster | `raft_start()` → 3 nodes, staggered election timeouts |
| Election | Follower timeout → candidate → majority votes → leader |
| Heartbeats | Leader AE every ~40 ms; election timeout ~180–260 ms |
| Log | Single-entry AppendEntries; index 0 sentinel |
| Commit | Majority `match_idx`; apply to per-node `Map<i64,i64>` |
| Client | `raft_put` / `raft_get` broadcast; leader replies on private chan |
| Verify | `examples/verify/raft_term.baga` + rules specs |

## API

```baga
fn raft_start() -> RaftCluster !Par
fn raft_put(c, key, val) -> i64 !Par    // 0 ok, -1 fail after retries
fn raft_get(c, key) -> i64 !Par         // value or -999999
fn raft_stop(c) -> i64 !Par
```

## Run

```bash
./baga -I . -I app-product app-product/raftbaga/demo.baga
./baga -I . -I app-product tests/raft_test.baga
./baga --verify -I app-product examples/verify/raft_term.baga
```

## Honest limits

- N = 3 only; in-memory log (no fsync — durability is lsmbaga’s job).
- PUT wait may drop non-AE_REP messages (no internal queue) — fine at probe load.
- No pre-vote, snapshots, membership change, network partitions.
- Full election safety / log matching across crashes: **not proven**.

See [gaps.md](gaps.md).
