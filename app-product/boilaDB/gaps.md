# boilaDB — gaps (честен списък)

Попълва се с всяка фаза (моделът на rocksbaga/docs/gaps.md). Буквите:
V = value/codec/vector, K = key/scan, S = storage, M = metrics/monitoring,
H = HTTP/API, Q = SQL (от P1), C = cache/planner, A = агрегати,
T = транзакции, W = wire protocol, F = FTS.

## Открити при P11

- **P11-1 — (PARTIAL) concurrent ladder, not 10k.** `bench/boila/mt_ladder`
  : 1/4/8/16/32 HTTP clients × 200 point SELECT against MT serve
  (~3–8k ops/s). Not 10k OS-thread clients; residual true pool /
  shard-owner workers.
- **P11-2 — (FIXED) wall deadline + max_scan/max_rows.** `BoilaBudget`
  begin at fetch; cooperative `boila_budget_tick` every 64 keys →
  57014 on timeout. Env `BOILA_BUDGET_MS` (default 5000; 0 = immediate).
  Residual: not a preemptive mid-op kill inside rocksbaga get.
- **P11-3 — (FIXED) DROP TABLE full wipe.** `catalog/drop.baga`
  `boila_table_drop`: wipe data/index/fts/vec/graph CFs + modality
  catalog (f/fl/fs, vh/vl/vm, gh/gl, x|<tid>|*) + name/schema/ttl.
  Global `m|next_*` counters untouched.
- **P11-5 — (FIXED) ALTER TABLE ADD COLUMN (nullable).** `parse_alter.baga`
  + `catalog/alter.baga` rewrite schema row; short rows pad NULL on
  project. No DEFAULT / NOT NULL / DROP COLUMN / VECTOR. kind=22.
- **P11-6 — (FIXED) DROP INDEX name ON table.** `parse_index` +
  `index/ix_drop.baga`: cascade secondary → FTS → HNSW. Cat/list/stats
  rewrite; wipe ix entries (index CF / fts CF if last / vec n|prefix).
  `IF EXISTS`. kind=23. Residual: multi-FTS same table shares CF keys.
- **P11-7 — (FIXED) DROP GRAPH name ON table.** `graph/g_drop.baga` +
  `exec_drop.baga`: cat/list rewrite, wipe `o|`/`i|` gid prefixes.
  `IF EXISTS`. kind=24.
- **P11-8 — (FIXED) IF [NOT] EXISTS on TABLE.** `CREATE TABLE IF NOT
  EXISTS` (42P07 → no-op ok); `DROP TABLE IF EXISTS` (42P01 → no-op).
  Schema not replaced on IF NOT EXISTS.
- **P11-9 — (FIXED) CREATE … IF NOT EXISTS for INDEX/FTS/HNSW/GRAPH.**
  42710 → no-op ok. Same for `CREATE INDEX IF NOT EXISTS … USING hnsw`.
- **P11-10 — (FIXED) TRUNCATE [TABLE] [IF EXISTS] name.** Wipe data/
  index/fts/vec/graph CFs; keep schema + modality defs; reset FTS
  stats + HNSW ep. kind=25.
- **P11-11 — (FIXED) CREATE/DROP DATABASE IF [NOT] EXISTS.** 42P04 /
  3D000 → no-op ok.
- **P11-12 — (FIXED) ALTER TABLE RENAME.** `RENAME TO` (table name
  key swap); `RENAME [COLUMN] a TO b` (schema only, positional data).
  kind=22.
- **P11-13 — (FIXED) ALTER TABLE DROP COLUMN.** Last non-PK only
  (positional rows). CASCADE drops secondary/FTS/HNSW on col; GRAPH
  still 2BP01. kind=22.
- **P11-14 — (FIXED) SHOW TABLES / SHOW INDEX[ES] FROM t.** Catalog
  list (n| / x| + FTS/HNSW/GRAPH). GUC SHOW unchanged.
- **P11-15 — (FIXED) SHOW COLUMNS / DESCRIBE / DESC.** column_name,
  data_type (incl vector(n)), is_nullable, is_primary_key.
- **P11-16 — (FIXED) SHOW CREATE TABLE.** Reconstructs CREATE TABLE
  + PK + ttl_sec WITH clause from catalog.
- **P11-4 — barabadb/SQLite сравнение не е в repo run.** Изисква
  външни бинарници; суров rocksbaga baseline остава P1 scorecard.

## Открити при P10

- **G1 — perf gate 1M edges BFS d=3 < 100 ms не е измерен.** Functional
  P10 е зелен; bulk seed bench чака (Q2 arena).
- **G2 — (FIXED) DML sync + edge weight upsert.** `graph_sync_row`;
  G2b: same src→dst updates weight (simple graph). del removes first
  match. Residual: no true multi-edges; two data rows same endpoints
  share one adjacency slot.
- **G3 — WITH RECURSIVE е фиксиран pattern**, не пълен SQL CTE (без
  множествени CTE, без произволни JOIN-и в recursive leg, без `.`
  qualifiers — lexer няма `.`).
- **G4 — (FIXED) MODE BFS|DFS|DIJ[KSTRA] + CTE name suffix.** Explicit
  `WITH RECURSIVE cte MODE DFS (...)` overrides; name `*_dfs`/`*_dij`
  still works. Residual: no SEARCH clause / PG cycle syntax.

## Открити при P9

- **S5 — perf gate 1M точки / <50 ms не е измерен.** Functional P9 е
  зелен (boila_ts_test); bulk seed + range+bucket bench чака chunked
  insert (Q2).
- **S6 — (FIXED) TTL sweeper flush + per-key purge.** `boila_ts_sweep`:
  flush then GET every data key of TTL tables (rocksbaga lazy-del on get).
  `boila_ttl_sweeper` / `BOILA_SWEEP_MS`. S6c: `BOILA_SWEEP_SYS_ROUNDS`
  (500) / `BOILA_SWEEP_DATA_ROUNDS` (2000). Residual: no expires index.
- **S7 — (FIXED) time_bucket + [AS] alias + ORDER BY out names.** `biv` +
  `alias` on `BoilaSelItem`; parse `AS ident` or bare alias (not clause
  kw); out name via `boila_sel_item_out_name`. ORDER BY after projection
  when needed. S7d: `time_bucket`+agg without GROUP BY auto-groups.
  S7e: GROUP BY out names use SELECT aliases (`bkt`, `d`, …).
- **S8 — (FIXED intentional) `ttl_sec` extension.** `WITH (ttl_days|ttl_sec)`
  both accepted; `ttl_sec` is boila-native fine grain (tests + sub-day TTL).
  Documented in PLAN/ARCHITECTURE; not a PG dialect gap to close.

## Открити при P8

- **V2 — perf gate 100k×128d още не е измерен.** Functional P8 е зелен
  (boila_vec_test); 100k seed + recall@10 bench чака chunked seed заради
  arena OOM (Q2), моделът на FTS 20k.
- **V3 — (FIXED) metadata + PK-range pre-filter преди kNN.** `eq` по
  PK/secondary → cands; PK `>=`/`<=` → range scan cands or narrow eq
  set (`boila_knn_pref` / V3b). Non-PK range/eq → HNSW overfetch + V6
  post-filter.
- **V4 — (FIXED) RAM neighbor cache per search.** `hnsw_nbrs_get_c` +
  `hnsw_warm_upper` (ep + 1-hop levels max..1) в `Map<bytes,bytes>`;
  greedy/beam ползват cache. Residual: няма process-global cross-query
  cache (invalidate при DML); cache живее за една search/index op.
- **V5 — (FIXED) unindex strips reverse edges + ep reassign.** Strip pk
  from nbr lists L0..4; if deleted node was ep, reassign to an L0
  neighbor (else clear). V5c: ep = L0 nbr with highest degree. Residual:
  degree-only heuristic (not distance-to-centroid).
- **V6 — (FIXED) kNN + AND eq/range.** Parse приема AND; V3 pre-filter
  при indexed eq; иначе overfetch + post-filter; LIMIT = k на offlim.
- **V7 — fixed-point ×1e6** вместо IEEE payload (V1 bit-cast липсва).
  Достатъчно за ranking; не е bit-identical с pgvector float4.

## Открити при P7

- **F1 — (FIXED) multi-script case fold.** ASCII; Latin-1; Latin-Ext
  (İ→i, Ğğ Şş…); Greek Α-Ω + ς→σ; Cyrillic А-Я/Ё. Residual: full
  Unicode casefold tables; locale-Turkish I→ı not applied.
- **F3 — (FIXED) BM25 idf + per-doc dl.** `fts_bm25_idf` integer ln;
  F3b: `fts_doc_get` doclen in score loop (k1/b length norm). Residual:
  not bit-identical to PG ts_rank float; one GET per candidate.
- **F4 — posting списъците са един запис на term (read-modify-write при
  индексация).** Алтернативата (append-only + merge) идва при нужда от
  по-бързо писане; point GET query-тата са целта (K2).
- **F5 — (FIXED) phrase + `<N>` distance.** `a <-> b` ≡ `a <1> b`;
  `a <N> b` → b at offset N (PG FOLLOWED BY). `phraseto` gaps=1.
  `fts_phrase_match_gaps`. Смесване с &/| → 0A000.
- **F6 — (FIXED) @@ + AND eq/range.** Post-filter върху FTS hits
  (`boila_row_pass_filters`); parse вече приема AND след @@.

## Открити при P6

- **W1 — (FIXED go_bg + multi-DB + per-shard + shared pc).** HTTP/PG:
  `go_bg` per-conn; live conn; `boila_open_mt` hop-less shards (per-shard
  scan, no all_lock). Shared per-db plan cache (pc_mu off — baga mutex
  owner-flag races under fan-out; puts rare after warmup). Schema DDL
  serial per-db. W1b: dmu held for all store SQL (SELECT/DML/DDL) so
  no SELECT vs DROP race. Residual: per-db full serialize (no shared lock).
- **W2 — (FIXED) prepared SELECT/INSERT/UPDATE/DELETE AST.** kind 1–4;
  `$N` = tag 100 placeholder; Bind fills; Execute без re-parse.
  FTS/kNN `$N` lit / parse fail → text subst fallback.
- **W3 — (FIXED) typed OIDs on Describe + Execute.** `BoilaResult.tags`
  + `pgw_oid_of` (bool=16 int8=20 text=25 bytea=17 json=114 tstz=1184).
  RowDescription на Execute ползва `pgw_row_desc_typed`.
- **W4 — (FIXED при P11-3) DROP TABLE full wipe.** `catalog/drop.baga`
  + parse/exec; modality CF + name/schema/ttl.
- **W5 — (FIXED) Describe → ParameterDescription + RowDescription.**
  Prepared SELECT: catalog resolve cols/types; stmt → 't'+'T', portal →
  'T'; non-SELECT / JOIN/GROUP → NoData (+ empty param desc за stmt).
- **W6 — (FIXED users) ACL + token/trust.** `CREATE/DROP/ALTER USER|ROLE`,
  `GRANT/REVOKE`, `SET ROLE`, `SHOW USERS|ROLES|GRANTS [FOR u]`. Privs:
  SELECT/INSERT/UPDATE/DELETE/CREATE/DROP/ALTER/CONNECT/ALL on TABLE/`*`/
  DATABASE. Meta `u|`/`a|`. Empty catalog = open. PG: user+pw or token.
  HTTP: Basic user:pass + Bearer/X-Boila-Token. FNS_MAX raised 1024→2048.
  Residual: no SCRAM/TLS; SET ROLE without pw only for superuser.
- **W7 — (FIXED) SET/SHOW/RESET/DISCARD session GUC.** SET name {TO|=}
  value → store in `srv.guc` (ST) / `BoilaPgSess.guc` (PG) /
  HTTP-MT `sess_guc` (`http_guc.baga` + keepalive). SHOW reads map then
  defaults. RESET [ALL|name]; DISCARD ALL clears GUC (+ prep on PG).
  kind 19/20/21. Residual: no real GUC side-effects (encoding etc.).
- **Q-dual — (FIXED) SELECT without FROM.** `exec_dual.baga`: literals
  (i64/str/bool/NULL), `version()`, `current_database()`, `current_schema()`,
  `current_user`/`session_user`/`user()`, `now()`/`current_timestamp`,
  `pg_is_in_recovery()`, `pg_backend_pid()`, `current_setting(name)` (via
  session guc), AS/bare alias, multi-col. W7d: `boila_mt_exec_guc` +
  PG/HTTP pass per-conn guc into dual. Arith `+ - * /` (*/ over +-),
  `||` concat, `CAST(x AS t)` / `x::t`, unary `-`, parens. Residual:
  no floats; `COALESCE`/`NULLIF`/`GREATEST`/`LEAST`/`CONCAT`;
  `length`/`upper`/`lower`/`trim`/`abs`/`substr`/`strpos`/`mod`/
  `left`/`right`/`reverse`/`replace`; dual cmp (`=<>…`) + searched
  `CASE [x] WHEN…THEN…ELSE…END`; dual `AND`/`OR`/`NOT`; dual
  `[NOT] BETWEEN` / `[NOT] IN` / `[NOT] LIKE|ILIKE` / `IS [NOT] NULL`.
  `repeat`/`lpad`/`rpad`/`sign`/`starts_with`/`ends_with`; dual
  `IS [NOT] DISTINCT FROM`; dual `generate_series` + `DISTINCT` /
  `ORDER BY` [NULLS FIRST|LAST] / `LIMIT` / `OFFSET`; bare `VALUES` multi-row.
  dual `UNION`/`INTERSECT`/`EXCEPT`; dual `WHERE` (bind col aliases).
  `FETCH FIRST/NEXT`; dual `power`/`current_date`/`pg_typeof`/
  `quote_literal`/`quote_ident`/`quote_nullable`; `EXPLAIN` /
  `EXPLAIN ANALYZE` (actual rows + Execution Time ms). Residual:
  length=bytes; upper ASCII-only; no INTERSECT/EXCEPT ALL;
  series cap 100k; one ORDER col; VALUES cols columnN; unknown fn → 42883.
- **Q-like — (FIXED) WHERE col [NOT] LIKE|ILIKE 'pat'.** `%`/`_`;
  ILIKE fold; NOT LIKE/ILIKE; ESCAPE 'c'; post-filter. Residual: no
  index acceleration; one LIKE per query.
- **Q-between/IN — (FIXED).** `col [NOT] BETWEEN lo AND hi` → lo/hi
  (PK early-stop; NOT BETWEEN = full scan+invert). `col IN|NOT IN`;
  `<>`/`!=`; `col = a OR col = b` → IN. Residual: one IN/ne/between;
  OR only same-col equality; no subquery.
- **Q-cmp — (FIXED) WHERE `<` / `>` exclusive.** `lo_excl`/`hi_excl` on
  Sel; PK range early-stop + post-filter. Mixed-col range → 0A000. Residual: no
  conflict beyond one lo/one hi.

## Открити при P5

- **C1 — (FIXED) plan cache покрива и JOIN.** Кешира се sel + лява
  таблица; дясната се резолва при exec (`sel.join_table`). DDL → invalidate.
  Residual: wide schema / right-table schema drift без DDL на left — ok
  докато CREATE INDEX/TABLE invalid-ва.
- **C2 — planner-ът е rule-based, без статистики/cost model.** Ред:
  pk point > index eq > seq scan; JOIN: index nested loop при индекс по
  вътрешната колона, иначе nested loop. Catalog статистики (histograms)
  и cost-based избор остават за P11 или при реална нужда.
- **C3 — (FIXED при P11-2) query budget.** `BoilaBudget` wall deadline +
  max_scan/max_rows; cooperative tick.
- **A1 — avg е целочислено деление** (sum/cnt в i64). f64 резултат чака
  f64 поддръжка в codec-а (V1).
- **A2 — (FIXED) hash join + light cost.** Index → NL; no ix → hash if
  outer > 8 else nested loop over fetched inner; empty outer short-circuit.
  A2c: `BOILA_JOIN_NL_MAX` (default 8). Residual: no stats/histograms;
  build still full inner.
- **A3 — (FIXED) WHERE eq/range на дясната JOIN таблица.** Eq +
  `>=`/`<=` на right col → post-filter (`boila_join_filter_right` /
  `_range`); left pushdown when col is left. LEFT+WHERE right →
  null-elimination. A3c: `ON a=b AND c=d` multi-eq. Residual: един
  JOIN; ON still equality-only; lo/hi must target same right col.

## Открити при P4

- **T2 — MVCC-ът е едно-сесийен buffered модел.** Изолацията идва от
  buffer-а (storage непроменен до COMMIT), не от multi-version ключове —
  тях и commit sequencer-ът за конкурентни сесии идват в P6 заедно с
  wire/pool concurrency-то. Честна ревизия на PLAN текста (вж. PLAN P4).
- **T5 — (FIXED) CREATE + DROP TABLE през txn buffer.** `boila_cat_create_txn`
  / `boila_table_drop_txn` + auto-commit; ROLLBACK възстановява DROP.
  Residual: large DROP may hit `BOILA_TXN_MAX` (54000); CREATE INDEX
  buffered от P4.
- **T6 — (FIXED) txn buffer cap.** `BOILA_TXN_MAX` (default 100000;
  0=unlimited) на BEGIN; put/del на нов ключ → 54000 при превишаване.
  Overwrite на съществуващ buffer entry не брои.

## Открити при P3

- **T1 — (FIXED при P4) няма statement atomicity/rollback.** Грешка
  midway в multi-row INSERT/UPDATE/DELETE персистираше редовете дотам.
  От P4 писанията минават през txn buffer-а и грешка → rollback на целия
  буфер (за имплицитните auto-commit txn-и и за явните).
- **K5 — row + index entries са отделни WAL записи.** Една заявка се
  fsync-ва като група (statement batch), но rocksbaga няма multi-record
  атомен WAL запис: torn pwrite в рамките на един statement би оставил
  частични записи при replay (тесен прозорец). Пълна оправия = WAL group
  record в rocksbaga (бъдеща промяна, вж. ARCHITECTURE §1 принцип 1).
- **K6 — (FIXED) NULL се индексира + IS [NOT] NULL.** Index build/DML
  пишат null entries; `IS NULL` → index lookup; `IS NOT NULL` → scan
  filter; `col = NULL` → празен (SQL 3VL).
- **K7 — (FIXED при P3 rev.2) boila_ix_list скенваше sys на всяко DML
  заявление.** Комбинацията с rocksbaga SCAN snapshot-а (материализира
  ВСИЧКИ ключове на клъстера при смяна на write epoch) правеше всяко
  заявление O(общия брой ключове) — измерено: OOM при ~1000 заявления
  върху 97k реда (~20 MB garbage/заявление). Поправено: индексните
  дефиниции живеят в schema row-а на таблицата (point GET, O(1));
  CREATE INDEX build е двупасов (събиране без писания → запис batch).
  Урок: в rocksbaga никога не се скенва по време на фаза, която пише.
- **Q2 — baga bump arena-та не reclaims; дългоживеещите тежки процеси
  OOM-ват.** Измерено при P3 bench: 1M INSERT-а (10×100k) OOM-ва на
  chunk 2 дори след K7 fix-а; 100k (10×10k) минава чисто и е измереният
  durability еталон. За сървъра това налага предвиденото в ARCHITECTURE
  §6: per-request arena (`arena_new/arena_reset`) още с P6 wire фазата —
  иначе дългият процес ще повтори OOM-а.

## Открити при P2

- **S4 — mkdir е extern FFI към libc** (езикът няма builtin mkdir/
  readdir/rmdir). boilaDB го ползва за сървърния root; алтернатива без
  FFI би била flat layout само в съществуваща директория. DROP DATABASE
  чисти файлове по известни шаблони, но празни директории не се трият
  (няма rmdir).
- **H3 — (FIXED) HTTP per-conn session db.** Keepalive `boila_http_conn`
  tracks `sess_db` (default `boila`); `USE` / successful `?db=` update it;
  bare `/sql` uses session. CREATE/DROP DATABASE do not switch session.
  H3b/c: `Set-Cookie: boila_db=` on USE (HMAC-signed when BOILA_TOKEN
  set); `X-Boila-Db` same signed form. pick `?db=` > header > Cookie >
  conn sess; all signed when BOILA_TOKEN set. H3d: `BOILA_CSRF=1`
  requires matching Cookie + X-Boila-Db (double-submit).

## Открити при P1

- **Q1 — SQL слоят е 5.1× по-бавен от суров GET за point заявки.**
  Измерено (bench/boila/results/point-read-2026-08-08.md): 6102 ns/op
  срещу 1193 ns/op (511%). Разбивка: lex+parse+catalog GET ≈ 4.9 µs
  върху 1.2 µs storage. Планът на ARCHITECTURE §4 вече предвижда
  plan cache + PK fast path; гейтът ≤ 1.25× от PLAN.md се премества
  като задължаващ при P5 (plan cache) / P6 (prepared statements).
  Числото от P1 е baseline-ът, срещу който се сравнява всяко подобрение.
- **K3 — (FIXED) PK range sort+lb+early-stop; non-PK range post-filter.**
  PK range: sort+lb+early-stop. Non-PK/unindexed eq: seq filter. K3d:
  `boila_ix_range` for SELECT+DML on i64/bool/ts secondary indexes.
  K3e: txn buf merge on ix range. K3f: var-width col range when PK is
  fixed-width (i64/ts/bool) — pk is key tail. Residual: str PK + str
  index still seq.
- **H2 — (FIXED) HTTP go_bg + per-shard hop-less + multi-DB + live conn.**
  `BOILA_MAX_CONN` → 503/53300. mode=`mt-shard`.

## Открити при P0

- **V1 — f64 няма sort-order кодировка.** Езикът още няма f64→i64
  bit-cast builtin (само `f64_to_str`). Без него IEEE битовата
  трансформация за byte-подредба е невъзможна. До появата на builtin:
  f64 колони само payload (без `ORDER BY`/range по f64). Таг 7 е
  резервиран в `core/value.baga`.
- **K1 — MVCC версионен суфикс липсва (до P4).** Ключът е
  `[cf][table][pk]`; P4 добавя суфикса тук (дизайнът е в codec.baga
  коментара). Операциите дотгава са точка/prefix — еднозначни са.
- **K2 — (FIXED) true prefix scan + index exact prefix.** `lsm_scan_prefix_kb`
  / cluster + `boila_scan_pref` (arbitrary key prefix). Table scan =
  cf|table_id; `boila_ix_lookup` = cf|table|ix_id|val (no full index CF
  page + filter). Residual: multi-value index range still needs sort-order
  iterator if added later.
- **S1 — shard-маршрутизация по hash на целия ключ** (djb2-подобен на
  rocksbaga), не само по pk. pk е доминиращо-вариращата част, така че
  разпределението е ефективно по pk — но не е гарантирано перфектно
  равномерно при къси pk-та. Измерва се на P11.
- **S2 — MVCC GC (P4+) ще е background sweeper**, докато rocksbaga няма
  compaction filter. Риск от write amplification под тежки update товари
  — измерва се на P11, не се крие.
- **M1 — (FIXED) request counters + build info + /ready.** `boila_mt_stat_*`
  + `/health` version, `/ready` (503 at max_conn), `/metrics`
  `boila_build_info`, PG `server_version` boilaDB 0.1.0.
- **H1 — (FIXED) go_bg + BOILA_MAX_CONN admission.** HTTP/PG:
  `boila_mt_try_conn` → 503/53300 when over cap (default 64). Residual:
  still OS-thread-per-conn (not fiber pool); P11-1 ladder not 10k clients.
