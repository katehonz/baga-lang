# txnbaga (S8): 2PC coordinator + MVCC notes

Date: 2026-08-05. Status: **shipped** (MVP).
Parent: `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md` step S8.

## Goal

Close Track S distributed-transactions probe: an in-process **two-phase
commit** coordinator with participants that hold an **MVCC key-value**
store (snapshot reads + versioned commits). Channels only (`go` / CSP);
pure decision rules under `--verify` where the fragment allows.

## MVCC (local store)

```
// Physical: ver_map key = k * TS_STRIDE + ts  → value
//           head_map key = k → latest commit ts
// Snapshot read at read_ts: largest ts' <= read_ts with a version for k
// Commit at commit_ts: write ver(k, commit_ts)=v, head[k]=commit_ts
```

- `TS_STRIDE = 1_000_000` — keys are non-negative i64 in MVP.
- No GC in v1 (honest); watermark note in README.
- Prepare stage of 2PC stages writes in a **write-set** without publishing
  versions until COMMIT (read-committed / snapshot isolation lite:
  concurrent snapshot readers do not see prepared data).

## 2PC protocol (channels)

```
Coordinator                         Participants (×N)
    |  PREPARE(tx, writes) →              |
    |  ← VOTE_YES | VOTE_NO               |
    |  COMMIT | ABORT →                   |
    |  ← ACK                              |
```

- N = 2 fixed (majority not required — classic 2PC needs **all** YES).
- Messages: nested `cell2` (same packing discipline as raftbaga).
- Participant: exclusive prepare locks per key (`lock_map: key → tx_id`);
  conflict → VOTE_NO.
- Coordinator timeout: if a vote is missing within T ms → ABORT.

## Pure rules (`rules.baga`, `--verify`)

- `tpc_decide(yes_count, n) → COMMIT(1)/ABORT(0)` — all-yes iff commit.
- `mvcc_visible(write_ts, read_ts) → 0|1` — version visible to snapshot.
- `lock_conflict(holder_tx, req_tx) → 0|1`.

Full blocking/recovery (coordinator log, presumed abort) is **not** claimed
proven — runtime probe + notes only.

## API

```baga
fn mvcc_new() -> MvccStore
fn mvcc_get(s, key, read_ts) -> MvccGet
fn mvcc_put_version(s, key, val, commit_ts)  // after 2PC commit

fn tpc_start() -> TpcCluster !Par          // 2 participants
fn tpc_txn(c, writes: Vec of key/val) -> i64 !Par  // 0 committed, -1 aborted
fn tpc_get(c, key, read_ts) -> i64 !Par    // routed read
fn tpc_stop(c) -> i64 !Par
```

## Tests

- MVCC: put v1/v2, snapshot sees older.
- 2PC: two keys commit; conflicting prepare aborts; reader snapshot isolation.
- `examples/verify/tpc_decide.baga` pure specs.

## Out of scope

- Persistent coordinator log, 3PC, XA, network partitions as first-class,
  N≠2, SQL.
