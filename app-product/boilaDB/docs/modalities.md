# Modalities

Four disk-persistent modalities sit under the same SQL executor. They
do not import each other; composition happens in `sql/`. Everything
is a rocksbaga key-space — indexes survive restart without rebuild.

## Full-text search

```sql
CREATE FTS INDEX docs_fts ON docs (body);

SELECT id, title FROM docs
 WHERE body @@ to_tsquery('бага & език')
   AND lang = 'bg'
 LIMIT 20;
```

| Piece | Behaviour |
|-------|-----------|
| Tokenizer | ASCII + Latin-1 + Latin-Ext + Greek + Cyrillic case fold |
| Query | `to_tsquery` / `plainto_tsquery` — `&` AND, `\|` OR |
| Phrase | `a <-> b` ≡ `a <1> b`; `a <N> b` (PG `FOLLOWED BY`); `phraseto` |
| Rank | BM25 (integer ln idf + per-doc length norm). Not bit-identical to PG `ts_rank` |
| Storage | One posting record per term (point GET, not a scan) |
| DML | INSERT/UPDATE/DELETE keep the index in the same WAL batch |

Mixing `&`/`|` with phrase in one query → `0A000`. `@@` AND
eq/range post-filters FTS hits (or uses a structural predicate first).

Measured (20k docs): AND 73 µs / OR 579 µs / single 264 µs
(`bench/boila/results/fts-2026-08-08.md`). Gate was &lt; 10 ms.

## Vectors (HNSW)

```sql
CREATE TABLE items (
  id  BIGINT,
  emb VECTOR(128),
  cat TEXT,
  PRIMARY KEY (id)
);
CREATE INDEX items_hnsw ON items USING hnsw (emb);

SELECT id FROM items
 WHERE emb <-> '[0.1, 0.2, …]'
   AND cat = 'news'
 LIMIT 10;
```

| Operator | Metric |
|----------|--------|
| `<->` | L2 (squared; order matches Euclidean) |
| `<=>` | Cosine |
| `<#>` | Negative inner product |

- Payload is fixed-point ×1e6 (`i32`), not IEEE float4 (gaps V1 / V7).
  Good enough for ranking; not bit-identical to pgvector.
- `VECTOR(n)` dimension is checked at insert.
- ≤ 512 rows: exact brute force. Above: HNSW in the `vec` CF.
- Metadata: indexed `=` / PK range **pre-filter** the candidate set;
  other predicates overfetch + post-filter.
- DML syncs the graph; delete strips reverse edges and reassigns the
  entry point.

Honest scope: multimodal SQL vectors, not a dedicated ANN service.
5k×128d ~12 ms; 10k×128d ~22–23 ms (GET-bound; PLAN 100k gate not
claimed). See `gaps.md` V2.

## Time-series

Not a separate language. Tables + TTL + `time_bucket` + optional
continuous rollup.

```sql
CREATE TABLE pts (
  ts    TIMESTAMPTZ,
  sensor BIGINT,
  val    BIGINT,
  PRIMARY KEY (ts)
) WITH (ttl_days = 7);
-- ttl_sec is a boila extension for sub-day TTL (tests, fine grain)

CREATE INDEX pts_ts ON pts (ts);

CREATE ROLLUP pts_1m ON pts USING time_bucket('1m', ts) SUM(val);

SELECT time_bucket('1m', ts) AS bkt, count(*), sum(val)
  FROM pts
 GROUP BY bkt
 ORDER BY bkt;
```

| Interval | Suffix |
|----------|--------|
| seconds | `s` |
| minutes | `m` |
| hours | `h` |
| days | `d` |

`time_bucket` without `GROUP BY` auto-groups. Unfiltered
`GROUP BY time_bucket` reads the rollup (O(buckets)). A `WHERE ts`
window uses complete rollup cells plus tight secondary-index bands
for partial buckets.

TTL: `lsm_put_ex` (lazy expire on GET) + optional sweeper
(`BOILA_SWEEP_MS`). Measured: 100k-point rollup GROUP BY ~2.8–2.9 ms
(gate &lt; 50 ms).

## Graph

```sql
CREATE TABLE edges (
  src BIGINT,
  dst BIGINT,
  w   BIGINT,
  PRIMARY KEY (src)
);
CREATE GRAPH g ON edges (src, dst, w);

-- BFS (default), depth < 3
WITH RECURSIVE walk(node, depth) AS (
  SELECT 1, 0
  UNION ALL
  SELECT dst, depth + 1 FROM edges, walk
   WHERE src = node AND depth < 3
)
SELECT * FROM walk;

-- explicit mode
WITH RECURSIVE walk MODE DFS (node, depth) AS ( … ) SELECT * FROM walk;
WITH RECURSIVE walk MODE DIJKSTRA (node, depth) AS ( … ) SELECT * FROM walk;
```

Mode can also be a CTE-name suffix: `*_dfs`, `*_dij`.

| Fact | Detail |
|------|--------|
| Storage | Bidirectional adjacency in the `graph` CF |
| DML | INSERT/UPDATE/DELETE sync edges (same endpoints upsert weight) |
| CTE | Fixed pattern — not a general SQL CTE (no multi-CTE, no arbitrary JOIN in the recursive leg) |
| Missing | PageRank, Louvain, PG `SEARCH` / cycle syntax |

Measured: 100k-chain BFS depth 3 ≈ 42 µs (gate &lt; 100 ms).
`DROP GRAPH name ON table` wipes adjacency; `IF EXISTS` is accepted.

## Hybrid queries

The executor composes modalities:

- `WHERE body @@ q AND city = 'София'` — FTS hits + eq/range filter
- `WHERE emb <-> '[…]' AND id >= 100` — kNN with PK-range candidates
- Aggregates over FTS / kNN / `IS NULL` hit sets stream into the
  hash-agg fold (no full result `Vec`)
