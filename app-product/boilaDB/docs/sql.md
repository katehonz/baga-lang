# BoilaSQL

BoilaSQL is a **documented PostgreSQL subset**. Anything outside this
page returns `0A000 feature_not_supported` with the construct name —
never a silent semantic difference.

Encoding: **UTF-8 only**. `server_encoding` / `client_encoding` are
`UTF8`. Another encoding → `0A000`. Identifiers are bare (`users`);
there are no double-quoted identifiers.

## Types

| SQL type | Storage tag | Notes |
|----------|-------------|--------|
| `BOOL` / `BOOLEAN` | 1 | |
| `BIGINT` / `INT8` | 2 | 64-bit signed |
| `TIMESTAMPTZ` | 3 | microseconds |
| `BYTEA` | 4 | binary-safe |
| `TEXT` | 5 | raw UTF-8 bytes |
| `JSONB` | 6 | document-as-column (no separate doc store) |
| `VECTOR(n)` | 1000+n | fixed-point ×1e6 i32; see [modalities.md](modalities.md) |

`FLOAT8` is reserved as tag 7 in the value codec (payload only, no
sort-order — gaps V1) but `CREATE TABLE` does **not** accept it yet.
Dual/expression arithmetic is integer-only (no floats).

`NULL` is three-valued: `NULL = NULL` is never true. `IS [NOT] NULL`
uses the secondary index when present.

**Not in v1:** `NUMERIC`, `SERIAL` / `BIGSERIAL`, `DEFAULT`,
`NOT NULL`, `REFERENCES`, arrays, usable `FLOAT8` columns.

Every `CREATE TABLE` requires a `PRIMARY KEY`.

## Databases

```sql
CREATE DATABASE name;
CREATE DATABASE IF NOT EXISTS name;
DROP DATABASE name;
DROP DATABASE IF EXISTS name;
USE name;                    -- MySQL convenience; PG startup already picks db
```

One session = one database. Switching mid-transaction → `0A000`.
`DROP` of the session’s current database → `55006`. Default database
after init: `boila`.

## DDL

```sql
CREATE TABLE [IF NOT EXISTS] t (
  id    BIGINT,
  name  TEXT,
  body  TEXT,
  PRIMARY KEY (id)
) [WITH (ttl_days = N | ttl_sec = N)];

ALTER TABLE t ADD COLUMN col TEXT;          -- nullable add-only
ALTER TABLE t RENAME TO u;
ALTER TABLE t RENAME [COLUMN] a TO b;
ALTER TABLE t DROP COLUMN col;              -- last non-PK only; CASCADE drops ix/FTS/HNSW

DROP TABLE [IF EXISTS] t;
TRUNCATE [TABLE] [IF EXISTS] t;             -- wipe data + modality CFs; keep schema

CREATE [UNIQUE] INDEX [IF NOT EXISTS] name ON t (col);
CREATE INDEX [IF NOT EXISTS] name ON t USING hnsw (emb);
CREATE FTS INDEX [IF NOT EXISTS] name ON t (body);
CREATE GRAPH [IF NOT EXISTS] name ON edges (src, dst [, w]);
CREATE ROLLUP [IF NOT EXISTS] name ON t USING time_bucket('1m', ts) [SUM(col)];

DROP INDEX [IF EXISTS] name ON t;           -- ON is required
DROP GRAPH [IF EXISTS] name ON t;
```

`SHOW TABLES` · `SHOW INDEX[ES] FROM t` · `SHOW COLUMNS FROM t` /
`DESCRIBE t` / `DESC t` · `SHOW CREATE TABLE t`.

`TABLE t [WHERE …] [ORDER BY …] [LIMIT …]` is sugar for
`SELECT * FROM t …`.

## DML

```sql
INSERT INTO t (c1, c2) VALUES (1, 'a'), (2, 'b')
  [ON CONFLICT DO NOTHING]
  [ON CONFLICT DO UPDATE SET c2 = EXCLUDED.c2, c3 = c3 + 1]
  [RETURNING * | col, …];

UPDATE t SET c2 = upper(c2), c3 = c3 + 1
  WHERE id = 1 OR length(c2) = 3
  [RETURNING …];

DELETE FROM t WHERE id IN (1, 2) [RETURNING …];
```

`ON CONFLICT` SET may mix literals and expressions. `EXCLUDED.col` is
the proposed insert row (PG semantics). `UPDATE SET` expressions see
the **old** row.

## SELECT

```sql
SELECT [ALL|DISTINCT] items
  [FROM t]
  [JOIN u ON t.a = u.b]          -- one INNER or LEFT; ON is equality (+ AND eqs)
  [WHERE pred]
  [GROUP BY cols|exprs]
  [HAVING pred]
  [ORDER BY col|alias [ASC|DESC] [NULLS FIRST|LAST]]
  [LIMIT n | LIMIT ALL | FETCH FIRST/NEXT n]
  [OFFSET n];
```

**WHERE** (structural, can use indexes):

- `=` `<>` `!=` `<` `<=` `>` `>=`
- `BETWEEN` / `NOT BETWEEN`
- `IN` / `NOT IN`
- `LIKE` / `ILIKE` / `NOT LIKE` (`%` `_`, optional `ESCAPE`)
- `IS [NOT] NULL`
- `AND` of the above
- `col = a OR col = b` rewrites to `IN`

**WHERE** (expression span — sequential filter, or AND-tail after a
structural predicate): arithmetic, `||`, functions, `CASE`, `CAST`/`::`,
`AND`/`OR`/`NOT`, `IS [NOT] DISTINCT FROM`. Top-level `OR` of mixed
columns is a seq-filter.

**Not in v1:** subqueries, window functions, more than one JOIN,
`INTERSECT`/`EXCEPT ALL` on tables (set ops exist on dual only).

## Dual (`SELECT` without `FROM`)

Literals, session builtins, scalar functions, arithmetic, `CASE`,
`CAST`/`::`, `VALUES`, `generate_series`, `UNION` / `INTERSECT` /
`EXCEPT`, `DISTINCT`, `ORDER BY`, `LIMIT`/`OFFSET`, `WHERE` on aliases.

```sql
SELECT 1 + 2 AS n, current_database(), now();
SELECT * FROM generate_series(1, 5);
VALUES (1, 'a'), (2, 'b');
```

Bare keywords (no `()`): `current_user`, `session_user`, `user`,
`current_database`, `current_schema`, `version`, `current_date`, `now`.

## Scalar functions

Unknown name → `42883`. Nested calls and expression arguments work in
projection / dual / WHERE expression spans.

| Function | Result |
|----------|--------|
| `length` / `char_length` / `character_length` | bigint (bytes, not graphemes) |
| `upper` / `lower` / `trim` / `btrim` / `reverse` | text (`upper` is ASCII-only) |
| `substr` / `substring` / `left` / `right` / `replace` / `repeat` / `lpad` / `rpad` | text |
| `strpos` / `abs` / `sign` / `mod` / `power` / `pow` | bigint |
| `starts_with` / `ends_with` | boolean |
| `coalesce` / `nullif` / `greatest` / `least` / `concat` | first-arg type or text |
| `quote_literal` / `quote_ident` / `quote_nullable` / `pg_typeof` | text |
| `version` / `current_*` / `now` / `current_setting` / `pg_backend_pid` / `pg_is_in_recovery` | session |

`avg` is **integer** division (`sum/cnt` in i64) — gaps A1.

## Aggregates

`COUNT(*)` / `COUNT(col)` / `SUM` / `AVG` / `MIN` / `MAX`, including
`agg(expr)` and `GROUP BY expr`. `HAVING` is a boolean tree (`AND`
tighter than `OR`, parentheses). Mix of agg and row expressions on the
group’s first row.

No `DISTINCT` inside aggregates.

## Transactions

```sql
BEGIN [READ ONLY];
COMMIT;          -- alias: END
ROLLBACK;        -- alias: ABORT
```

Isolation is a **per-session write buffer**: storage is unchanged until
`COMMIT`. Readers see committed data plus the session’s own buffer.
Error mid-statement rolls the whole buffer back. Crash before commit
leaves nothing durable.

`BEGIN` in an open txn → `25001`. Writes in `READ ONLY` → `25006`.
Buffer overflow → `54000` (`BOILA_TXN_MAX`).

This is not multi-version keys + a commit sequencer (gaps T2).
Serializable / 2PC / distributed txns are out of v1.

## Session GUC

```sql
SET name TO value;     -- or SET name = value
SHOW name;
RESET [name | ALL];
DISCARD ALL;           -- clears GUC; on PG also prepared statements
SET ROLE name [PASSWORD '…'];
SET ROLE NONE;         -- aliases: RESET ROLE, SET SESSION AUTHORIZATION
```

Defaults: `server_version`, encodings `UTF8`, `DateStyle`, `TimeZone`.
No real GUC side-effects (you cannot change encoding).

`EXPLAIN` / `EXPLAIN ANALYZE` print a text plan; `ANALYZE` adds actual
rows and execution time in ms.

## SQLSTATE (common)

| Code | When |
|------|------|
| `0A000` | Feature not supported |
| `22023` / type errors | Bad literal / dimension |
| `23505` | Unique violation |
| `25001` | Already in a transaction |
| `25006` | Read-only transaction |
| `28P01` | Password authentication failed |
| `3D000` | Unknown database (DROP IF EXISTS → no-op) |
| `42P01` | Undefined table |
| `42P04` | Duplicate database |
| `42P07` | Duplicate table (IF NOT EXISTS → no-op) |
| `42710` | Duplicate object (index/FTS/…) |
| `42803` | Grouping error |
| `42883` | Undefined function |
| `53300` | Too many connections |
| `54000` | Program limit (budget scan/rows, txn buffer) |
| `55006` | Drop current database |
| `57014` | Query canceled (wall deadline) |
