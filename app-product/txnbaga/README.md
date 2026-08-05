# txnbaga

**2PC coordinator + MVCC store** — Track S step **S8** (distributed
transactions probe).

| Piece | Notes |
|-------|--------|
| MVCC | Versioned i64→i64 map; snapshot `mvcc_get(k, read_ts)` |
| 2PC | Coordinator + 2 participants over channels; all-yes to commit |
| Locks | Exclusive prepare locks; conflict → VOTE_NO → ABORT |
| Verify | `rules.baga` — `tpc_decide`, `mvcc_visible`, `lock_conflict`, `next_ts` |

## API

```baga
fn mvcc_new() / mvcc_put_version / mvcc_get
fn tpc_start() -> TpcCluster !Par
fn tpc_put(c, key, val) -> i64 !Par          // 0 committed, -1 aborted
fn tpc_txn(c, k1, v1, k2, v2) -> i64 !Par  // k2 < 0 → single key
fn tpc_txn_id(c, tx, k1, v1, k2, v2) -> i64 !Par  // concurrent: disjoint tx ids
fn tpc_get(c, key, read_ts) -> i64 !Par
fn tpc_stop(c) -> i64 !Par
```

Both participants apply the same write set (replicated KV demo). Real
sharded 2PC would partition keys; the protocol is the same.

Concurrent PREPARE (B4.3): non-overlapping write-sets can prepare in
parallel; lock conflicts still VOTE_NO. Stress: `tests/txn_stress_test.baga`.

## Run

```bash
./baga -I . -I app-product app-product/txnbaga/demo.baga
./baga -I . -I app-product tests/txn_test.baga
./baga -I . -I app-product tests/txn_stress_test.baga
./baga --verify -I app-product examples/verify/tpc_decide.baga
```

## MVCC notes (design)

- **Versions** are immutable once published at `commit_ts`.
- **Snapshot isolation (lite):** readers use a `read_ts`; they never see
  prepared-but-uncommitted data (write-set is private until COMMIT).
- **2PC + MVCC:** PREPARE locks keys and stages the write-set; COMMIT
  allocates/publishes `commit_ts` versions; ABORT drops the write-set.
- **Not claimed:** persistent coordinator log, recovery after crash,
  deadlock detection, GC of old versions, serializability proofs.

See [gaps.md](gaps.md) and the design spec under `docs/superpowers/specs/`.
