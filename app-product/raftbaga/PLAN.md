# raftbaga — plan

Date: 2026-08-05
Status: **P0 MVP done** (S7)
Goal: distributed exam — election + replication over channels.

## P0 ✅

1. Message packing (`msg.baga`)
2. Pure rules + specs (`rules.baga`)
3. Node loop follower/candidate/leader
4. Cluster client put/get/stop
5. `raft_test` + demo + verify fragment

## P1

- Inbound message queue on leader PUT wait (R1)
- Optional 5-node packing
- Redirect hints (leader id in reply_err)

## P2

- Durable log lite (term/vote/log file) — **done (B4.2)**
- Durable log via lsmbaga (stronger)
- TCP/jsonrpc transport
- Pre-vote + snapshot
