# Changelog

## [Unreleased]

## [1.0.1] — 2026-08-22

### runtime — `--rc`: release на локали при изход през ефект (двата бекенда)
- `raise !E(p)` и `?` propagate пътят (и catch-веригата без handler) вече
  release-ват RC локалите на функцията преди изхода — преди течаха и в C, и
  в LLVM (`emit_eff_return_zero`/`h_ret_zero` без scope release). Heap
  payload, който не е fresh (ident/поле/borrowed), се retain-ва в слота
  преди release-ите и оцелява; fresh payload (owned call) се move-ва в
  слота без retain. Затваря readiness точка 7. Gate: новият
  `tests/raise_rc_test.baga` в `tests/llvm_rc.sh` (C `--rc` ↔ LLVM
  `--emit-llvm --rc` оракул); без `--rc` emit-c е бит-идентичен.

### docs — LLVM `--rc` паритетът е пълен; последните „изключения" са споделени граници
- Проверка в кода: closure capture-ите са borrowed и не се retain-ват и в C
  бекенда (`is_param=1` в wrapper-а, codegen_c.c:3542), а `raise`/`?` пътят
  не release-ва локали и в C (`emit_eff_return_zero` без scope release,
  codegen_c.c:1781). Двете са прекласифицирани от „LLVM изключения" на
  споделени граници в `docs/language-bg.md` / `docs/language-en.md`;
  release при изход през ефект е записан като отделна post-1.0 идея за двата
  бекенда в `docs/v1.0-readiness-bg.md` (точка 7). Без промяна в поведението.

### runtime — `--rc`: интерполация със str стойност вече не е underflow
- `rc_tmp_fresh` регистрираше всеки `NODE_TO_STR` като fresh heap temp
  (rc=1 owned), но `to_str` върху str е identity (borrowed — емитира се
  пряко, без копие). Две проявления под `--rc`: (1) `"x${s}y"` с owned str
  локал `s` — release под краката на `s` (`rc underflow` + четене на
  освободена памет); (2) `"${concat(...)}"` — двойна регистрация
  (to_str + fresh call-ът вътре) и двоен release. Сега `NODE_TO_STR` е fresh
  само при не-str вътрешност (`baga_i64_to_str`/`baga_f64_to_str` дават нов
  rc=1 низ; bool литералът е guarded no-op). Порт и в LLVM бекенда
  (`lrc_tmp_fresh`). Бонус чистка: мъртвият `node_has_payload_effects`
  (codegen_c) е изтрит (беше `-Wunused-function`). Gate: новият
  `tests/interp_rc_test.baga` в `tests/llvm_rc.sh` (C `--rc` ↔ LLVM
  `--emit-llvm --rc` оракул) + батерията.

### codegen LLVM — RC4/RC5 v0.11 паритет: release на temp-ове под `--rc`
- `src/codegen_llvm.c`: per-statement temp регистър под `--emit-llvm --rc`,
  огледало на RC4 в C бекенда (`rc_tmp_*`, codegen_c.c:1078-1521). Fresh
  heap temp-овете (owned call с heap тип — не borrowed, не enum ctor — плюс
  `NODE_TO_STR`) се събират pre-order от root израза на statement-а със
  същите правила за не-слизане (struct литерал, ламбда, try/catch, if-израз,
  десен операнд на `&&`/`||`, `drop(...)` и enum ctor аргументи). SSA
  вариант на hoist-инга: temp изразите се emit-ват веднъж преди root-а в
  текущия block (вътрешните първи), `LLVMValueRef` се кешира по AST възел и
  `emit_expr_llvm` връща кеша при удар — без двойна оценка. Release-ът е в
  края на statement-а през новия release-by-value път
  (`lrc_emit_release_val`) към същите `baga_rc_release_*` helper-и.
- Hook сайтове: let init, expr statement (вкл. bare call и assign), return
  (temp release преди release на локалите, като `emit_return_val`),
  implicit return във fn и ламбда, if/while условия и for-range lo/hi
  (условията се emit-ват в cond block-а — per-iteration release без GNU
  ({…}) wrap-а на C).
- RC5 v0.7/v1.0b consume: temp аргумент на `vec_push`/`vec_set`/`map_set`
  се прехвърля в контейнера без release в края на statement-а. Box
  пътеките (struct/enum/bytes) пропускат retain-а при temp (move);
  str/Vec helper-ите retain-ват вътрешно, затова след повикването temp-ът
  се консумира и се release-ва веднага — нетен move, наблюдаемо
  еквивалентен на `_move` вариантите в C.
- RC5 v0.11: match scrutinee temp-ове — enum ctor scrutinee с heap payload
  (tag 6) и owned fn резултат като scrutinee се release-ват СЛЕД рамената
  (в края на statement-а), така че borrowed binding-ите са валидни в
  рамената.
- Бонус затворена дупка: `match` върху `str` scrutinee в LLVM (преди:
  честен отказ „неподдържан конструкт") — огледало на C lowering-а
  (`int64_t _mv == "литерал"`, указателно равенство) чрез `ptrtoint` от
  двете страни. Без това `tests/match_temp_rc_test.baga` не се компилираше.
- `tests/llvm_rc.sh`: C страната на оракула върви с `BAGA_CFLAGS=-w` —
  gcc warning-ите от генерирания C (указателното сравнение при str match)
  не са семантика, но `2>&1` ги слива в изхода и diff-ът ги виждаше.
- Без `--rc` генерираното IR е бит-идентично с предишното за всички
  програми, които се компилираха и преди (всичко ново е зад `lg.rc`
  guard-ове; единствената не-RC промяна е str match-ът, който преди беше
  compile-time грешка).
- Gate: `make test-llvm-rc` (match_temp_rc_test вече е OK, не SKIP) +
  `./baga-llvm --emit-llvm --rc -I . tests/{temp_test,match_temp_rc_test,
  calltemp_rc_test}.baga` + `lli-14 -load lib/libbaga_par.so` — и трите
  зелени; `scripts/run_tests.sh` без промяна (C страната не е пипана).

## [1.0.0] — 2026-08-22

### чистка — двата стари TODO-та преди v1.0 са затворени
- `src/proofs.c`: мъртвата евристика `is_recursive` (винаги 0, без извикващ)
  е изтрита — рекурсията се доказва от `--verify` с decreases мярка, а
  `--proofs` termination теоремата се захранва от `has_base_case` /
  `count_returns`.
- `src/checker.c` NODE_INDEX: TODO-то „array elem type" е заменено с честен
  коментар — `x[i]` е нисконивов примитив (суров C subscript), елементен тип
  не се следи; Vec/bytes/str се индексират през `vec_get`/`bytes_get`/
  `str_char_at`. Без промяна в поведението.

### docs — v1.0 readiness: границите са маркирани финални или post-1.0
- Нов `docs/v1.0-readiness-bg.md`: инвентар на честните граници, разделени на
  финални по дизайн (кандидати за замразяване в v1.0) и временни (roadmap
  след 1.0). LLVM `--rc` изключенията в `docs/language-bg.md` /
  `docs/language-en.md` вече не са „засега" — (1) RC4 statement temp-ове,
  (2) match scrutinee temp-ове, (3) closure capture retain и (5) release по
  raise пътя са официално отложени за post-1.0; (4) циклите без weak са
  финална граница, споделена с C бекенда. Без промяна в поведението.

### codegen C — неподдържан AST възел е грешка, не тиха константа
- `emit_expr`/`emit_stmt` default клоновете вече викат `c_unsupported_node`
  (`baga: C backend: неподдържан израз/statement (AST възел #N)`, exit 1)
  вместо да емитват `0 /* unhandled expr %d */` — същата политика на шумен
  отказ като `llvm_unsupported` в LLVM бекенда. Цялата батерия
  (`run_tests.sh`, включително emit-c probes) остава зелена — fallback-ите не
  бяха достижими от валидни програми.

### runtime — `--rc` release на генерик контейнери + чист UBSan guard
- `let v: Vec<U> = …` в генерик тяло вече не чупи `--rc` емисията:
  node fallback-ите на RC resolver-ите (`rc_vec_elem_kind_node`,
  `rc_map_val_tag_node`) четат проверения от checker-а тип на възела
  (конкретен за инстанцията след `checker_recheck_inst`) вместо суровото
  име — преди типовата променлива `U` се емитираше като `sizeof(b_U)`
  и gcc гърмеше. Налична граница: `Vec<Vec<T>>` с типова променлива
  остава leak-safe (като досега).
- `baga_rc_lo` вече е `NULL` вместо `(char *)-1` sentinel и guard-ът в
  `baga_rc_hdr` го проверява — преди проверките преди първата арена
  даваха UBSan „pointer index expression overflow" (безвредно на хардуер,
  но шумно под `-fsanitize=undefined`).
- Gate: `tests/generic_fn_param_test.baga` под `--rc` (новият тест в
  батерията); 16-файловата `--rc` батерия и бит-идентичен `--emit-c`
  без флага.

### checker — M21 re-infer отново работи (регресия от MEM-3 флаговете)
- `check_fn` ранният изход по `FnRec.checked` (MEM-3) блокираше двата
  пътя, които по дизайн преинферират тялото: новата инстанция в
  `generic_instantiate` и `checker_recheck_inst` преди емисия на всеки
  вариант. Понеже `restore_method_calls` нулира типовете в тялото,
  генерик телата оставаха без типове: fn-тип параметър губеше L5
  маркера „TYPE_FN без име" и `f(...)` се емитираше като директно
  извикване на `int64_t` вместо през cell2 handle; втора инстанция
  оставаше с параметрите на първата. Флагчетата се нулират само по тези
  два пътя — `mem3_ensure_fn` и основният пас не се променят. Gate:
  `tests/generic_fn_param_test.baga` (нов) + `examples/generics.baga`.

### checker — i64 param не е арена; C2 verify на raft/tpc фрагменти
- `let x = n` за i64 параметър вече не гърми като забравена арена
  (`is_arena` само за `arena_new` и реални handle алиаси). Gate:
  `examples/verify/liveness_struct.baga`.
- `can_grant_vote` / `prev_log_ok` nested-if → ensures ДОКАЗАНО.
  `run_verify.sh` включва `raft_term` и `tpc_decide` (не се твърди пълен
  Raft).

### checker — забравена арена е грешка
- `let a = arena_new()` без `arena_free` никъде във fn и без `return a`
  е compile error. Vec/Map остават само под `--warn-leaks`. Maybe-leak
  (`?` пътека при наличен free в другото рамо) не гърми по подразбиране.
  Gate: `scripts/run_tests.sh` forgotten-arena probe; tcp/poll/examples/arena
  остават зелени.

### checker — MEM-3 handle през `return a` + field assign
- `fn wrap(a: i64) -> i64 { return a }` пренася handle identity на call
  site (`let b = wrap(a)`). `s.p = arena_alloc(a, n)` тагва struct-а.
  Vec от указатели остава честна граница. Gate: `scripts/run_tests.sh`
  wrap/field-assign probes.

### checker — MEM-3 region през fn резултат и struct литерал
- Функция, която връща `arena_alloc(param)` (или alias на такъв payload),
  тагва call site-а с region-а на подадения handle — редът в файла не
  пречи (check_fn при нужда се влага). Struct литерал с поле от региона
  тагва целия binding. `add(x,y)` не се бърка. Gate: `scripts/run_tests.sh`
  MEM-3 fn/struct probes.

### checker — MEM-3 handle алиаси споделят identity
- `let b = a` (и `b = a`) където `a` е `arena_new` споделя `arena_id`.
  `arena_free` на което и да е име убива payload-ите и всички алиаси;
  двоен free през второто име е грешка. Gate: `scripts/run_tests.sh`
  MEM-3 handle-alias probes.

### checker — MEM-3 region tags върху alias и p±n
- `let q = p`, `p + n` / `n + p` / `p - n` и `if` с еднакъв region в
  двата клона наследяват тага на `arena_alloc`. `p - q` (два указателя)
  не се тагва. `p = p + 8` пази региона. Gate: `scripts/run_tests.sh`
  MEM-3 alias / arith probes.

### parser — гол `{ }` блок на statement ниво
- `{ print("x") }` вече е statement блок, не EXPR_STMT около израз.
  Преди C гълташе print-овете (`0 /* unhandled expr 11 */`), LLVM отказваше
  с AST възел #11. Блок като стойност (`let n = { 1 }`) работи и в двата
  бекенда. Gate: `tests/block_stmt_test.baga`.

### checker — MEM-3 leak предупреждения (`--warn-leaks`)
- Нов severity „предупреждение" (не спира компилацията). Под `--warn-leaks`
  `Vec`/`Map`/`bytes`/`fn` без `drop` и `arena_new` без `arena_free` при
  изход от scope се диагностицират; върнат ident не е теч. Без флага
  поведението е непроменено. Gate: `scripts/run_tests.sh` MEM-3 leak probes.

### runtime — C `--rc`: heap let в block catch handler
- `let h = …` вътре в `{ … }` catch handler вече има собствен RC scope
  в GNU stmt-expr-а (`emit_catch_handler_block`). Преди release-ът се
  емитваше в enclosing fn → gcc `'b_h2' undeclared`. Последният IDENT
  от handler-локалите е move към catch резултата (като LLVM). Без `--rc`
  emit-c е непроменен. Gate: `tests/catch_rc_test.baga`.

### LLVM backend — `drop` + `--rc` refcounting паритет с C бекенда
- `--emit-llvm --rc` вече дава същия RC модел като C бекенда: scope
  release, retain при alias, move при return, транзитивен release за
  Vec/Map/struct/enum; `drop` работи и в LLVM (free без `--rc`, RC
  release с `--rc`). Изключения: statement-ниво и match scrutinee
  temp-ове не се release-ват, closure captures и borrowed init не се
  retain-ват, цикли текат (няма weak). Gate: `make test-llvm-rc`
  (C↔LLVM diff оракул, `tests/llvm_rc.sh`).

### boilaDB P22-1 — COPY FROM in the MT worker pool
- `COPY … FROM STDIN` under `BOILA_WORKERS>0` no longer returns
  `0A000`. Wire CopyData is applied through `boila_mt_copy_from`
  (shared data lock + hop-less checkout). Gate: `tests/boila_copy_test`
  mt_in / mt_out / mt_rollback. TLS still answers `'N'` on SSLRequest
  (`std/net` has a TLS 1.3 client only).

### boilaDB P22-2 — NUMERIC(p,s) + sort-order keys
- DDL `NUMERIC(p[,s])` (p 1..28, s 0..p); INSERT/UPDATE round
  half-away-from-zero to s, overflow → `22003`. Keys are 31-byte
  sort-order so PK and secondary range scans work. Gate:
  `tests/boila_numeric_test` + `boila_declex_test` npk_range.

## [0.9.2] — 2026-08-16

### boilaDB — version 0.7
- `server_version` / `version()` / `/health` report **boilaDB 0.7**
  (README, docs sync).

### boilaDB N1a — unquoted NUMERIC literals
- `12.50` / `-0.5` са NUMERIC (tag 9) още в lexer-а (`tok_dec`): работят
  в INSERT VALUES / UPDATE SET / ON CONFLICT SET / DEFAULT / WHERE /
  HAVING. Dual/projection изрази ги отказват честно (0A000); range по
  NUMERIC PRIMARY KEY → 0A000 (преди тихо грешно — byte-enc не е
  sort-order). `tests/boila_declex_test` 22/22. Filesize split:
  `sql/parse_lit.baga`.

### boilaDB U5b — to_char month/day names + meridiem
- `Month Mon MON mon Day DY Dy dy D AM PM am pm` (English имена,
  PG 9-знаково padding на `Month`/`Day`). Gate: `boila_tochar_test`.

### boilaDB N2b — премахнат мъртъв HAVING fallback
- Непроектираните HAVING агрегати вървят през синтетичните слотове на
  `boila_hav_setup` (Q-havx) и сравняват decimal-но; старият
  `boila_agg_extra` път беше мъртъв код (brows винаги празен) — махнат,
  липсващ слот → шумна грешка. Gate: `boila_numagg_test` non-projected
  checks.

### boilaDB W2b — window MIN/MAX over NUMERIC
- Kind 7/8 ползват decimal слота като SUM/AVG (`dec_cmp`, NULL skip,
  OID 1700). Gate: `boila_winnum_test` min/max + nullskip.

### boilaDB P21-1 — multi-column UNIQUE index
- `CREATE UNIQUE INDEX name ON t (a, b, …)`; tuple-wide 23505; NULL in
  any indexed column → no collide (PG); var-width false positives
  filtered by re-reading candidates (also fixed a latent committed-row
  read bug). `tests/boila_munique_test` 16/16 + regression battery.

### boilaDB P21-3 — REFERENCES / foreign keys
- Column/table-level `REFERENCES parent (pcol)` with
  `ON DELETE {NO ACTION|RESTRICT|CASCADE|SET NULL}`. Child
  INSERT/upsert/UPDATE require the parent row (23503); parent DELETE
  applies the child FK action. `tests/boila_fk_test` 16/16 incl.
  restart.

### boilaDB deps — restore §3 layering (kimi-deps gate back to 0)
- `repl/` gets a layer rank (above server) in scripts/deps.sh.
- Cross-db SELECT execution (boila_exec_xdb) moved sql/ → server/
  (it drives the multi-db registry); pure FDW pk helpers + needs-xdb
  predicate stay in sql/exec_xdb.baga. Eliminates the sql→server upward
  edge. deps.sh back to 0 grandfathered exceptions.

### boilaDB P21-4 — CHECK constraints
- Table-level `CHECK (expr)` + column-level `col TYPE CHECK (expr)`;
  enforced on INSERT/upsert/UPDATE (TRUE/NULL pass, FALSE → 23514);
  rendered by SHOW CREATE TABLE. `tests/boila_check_test` 15/15 incl.
  restart.

### boilaDB P21-5 — window SUM/AVG over NUMERIC
- `SUM(col) OVER (…) / AVG(col) OVER (…)` on a NUMERIC column are
  decimal-exact and return NUMERIC (OID 1700); closes the P13 frame
  residual. `tests/boila_winnum_test` + `boila_window_test` green.

### boilaDB P21-2 — numeric HAVING
- `HAVING sum(col) > '12.5'` compares decimally against NUMERIC
  aggregates (SUM/AVG/MIN/MAX); int literal vs numeric agg also
  decimal; decimal literal vs integer agg → 0A000.
  `tests/boila_numagg_test` (5 HAVING checks).

### boilaDB P20-5 — to_char(timestamptz, fmt)
- Date/time formatting: YYYY/MM/DD/HH24/HH12/HH/MI/SS tokens,
  Hinnant civil-date (pre-1970-safe). `tests/boila_tochar_test` 8/8
  incl. leap day.

### boilaDB P20-4 — sum/avg over NUMERIC (P12b)
- Decimal-exact aggregates: SUM/AVG/MIN/MAX over NUMERIC stay NUMERIC
  (OID 1700), AVG scale ≥ 6, NULLs skipped. `tests/boila_numagg_test`
  15/15 (sum 31.05, avg 7.7625, GROUP BY, restart, i64 no-regression).

### boilaDB P20-3 — SERIAL / BIGSERIAL auto-number PK
- Omitted PK in INSERT gets the next per-table counter (txn-buffered;
  ROLLBACK returns the number); RETURNING gives it back; SERIAL must
  be the PK. `tests/boila_bigserial_test` 15/15 incl. restart.

### boilaDB P20-2 — NOT NULL + DEFAULT column constraints
- CREATE TABLE constraints (any order after the type); INSERT fills
  omitted columns with defaults (`DEFAULT <literal>` / `DEFAULT now()`);
  23502 on NOT NULL violations (INSERT/UPDATE/upsert); SHOW renders
  them. `tests/boila_constraints_test` 16/16 incl. restart.

### boilaDB P20-1 — UNIQUE indexes (app-ready phase opens)
- `CREATE UNIQUE INDEX`: 23505 on INSERT/UPDATE/upsert duplicates,
  NULLs distinct (PG), build-time dup refusal, txn-buffer-aware check.
  `tests/boila_unique_test` 20/20 incl. restart.

### boilaDB P19 — raftbaga replica
- In-process N=3; leader SQL + LSN ticket; follower replay;
  `pg_is_in_recovery()` / write `25006`. `tests/boila_repl_test`.

### boilaDB P18 — cross-database / local FDW
- `db.table` JOIN без USE; `CREATE SERVER` + `FOREIGN TABLE` алиас.
  DML в чужда база → `0A000`. `tests/boila_xdb_test`.

### boilaDB P17 — SERIALIZABLE
- `BEGIN ISOLATION LEVEL SERIALIZABLE` / `REPEATABLE READ`.
  Commit ww/rw → `40001`. `tests/boila_serial_test`.

### boilaDB P16 — compaction filter GC
- rocksbaga per-CF `filter_kind` + `lsm_compact_full`.
- boilaDB data CF versioned keys; `VACUUM`; 10k UPDATE → 1 version.
  `tests/boila_gc_test`, `tests/lsm_filter_test`.

### boilaDB P15 — SCRAM-SHA-256
- CREATE USER пише `s1:` verifier. PG auto-SASL за такива users;
  pgbaga connect + грешна парола покрити в `tests/boila_scram_test`.

### boilaDB P14 — COPY
- `COPY t FROM STDIN` / `TO STDOUT` (text + csv). 1k IN/OUT гейт;
  лош ред → `22P04` + rollback.

### boilaDB P13 — window функции
- `ROW_NUMBER`/`RANK`/`DENSE_RANK`/`SUM`/`AVG`/`COUNT`/`MIN`/`MAX`
  `OVER (PARTITION BY … ORDER BY …)`; default RANGE UNBOUNDED
  PRECEDING. `tests/boila_window_test`.

### boilaDB P12 — NUMERIC
- `NUMERIC`/`DECIMAL` през bagadecimal (tag 9, PG OID 1700).
- Следващите фази са в плана: COPY, SCRAM, compaction-filter
  GC, serializable, FDW, raftbaga репликация.

### relbaga — отделно репо
- `app-product/relbaga` е git submodule към
  [bagalang/relbaga](https://github.com/bagalang/relbaga).
- `std` остава тук; `tests/rel_test` също.

### License
- Пакетите в `app-product/` без собствен файл получават MIT
  (`LICENSE`, Copyright (c) 2026 Dim Gigov), като baga и boilaDB.

### zipbaga — отделно репо
- `app-product/zipbaga` е git submodule към
  [bagalang/zipbaga](https://github.com/bagalang/zipbaga).
- `std` остава тук; `imgbaga` / `officebaga` / `reportbaga`
  продължават да сочат `../zipbaga`; `tests/zip_test` /
  `office_test` / `report_test` също.

### xmlbaga — отделно репо
- `app-product/xmlbaga` е git submodule към
  [bagalang/xmlbaga](https://github.com/bagalang/xmlbaga).
- `std` остава тук; `officebaga` / `reportbaga` продължават да
  сочат `../xmlbaga`; `tests/xml_test` също.

### wsbaga — отделно репо
- `app-product/wsbaga` е git submodule към
  [bagalang/wsbaga](https://github.com/bagalang/wsbaga).
- `std` остава тук; `chatbaga` продължава да сочи `../wsbaga`;
  `tests/ws_test` / `chat_test` също.

### wasmtimebaga — отделно репо
- `app-product/wasmtimebaga` е git submodule към
  [bagalang/wasmtimebaga](https://github.com/bagalang/wasmtimebaga).
- `std` остава тук; smoke/demo остават в самото репо.

### uuidbaga — отделно репо
- `app-product/uuidbaga` е git submodule към
  [bagalang/uuidbaga](https://github.com/bagalang/uuidbaga).
- `std` остава тук; `tests/uuid_test` също.

### txnbaga — отделно репо
- `app-product/txnbaga` е git submodule към
  [bagalang/txnbaga](https://github.com/bagalang/txnbaga).
- `std` остава тук; `tests/txn_test` / `txn_stress_test` също.

### tplbaga — отделно репо
- `app-product/tplbaga` е git submodule към
  [bagalang/tplbaga](https://github.com/bagalang/tplbaga).
- `std` остава тук; `oauthbaga` / `reportbaga` продължават да
  сочат `../tplbaga`; `tests/tpl_test` също.

### testbaga — отделно репо
- `app-product/testbaga` е git submodule към
  [bagalang/testbaga](https://github.com/bagalang/testbaga).
- `std` остава тук; тестовете, които импортират
  `testbaga/assert.baga`, също.

### statusbaga — отделно репо
- `app-product/statusbaga` е git submodule към
  [bagalang/statusbaga](https://github.com/bagalang/statusbaga).
- `std` остава тук; `pbbaga` / `ctxbaga` продължават да сочат
  `../statusbaga`; `tests/status_test` / `ctx_test` / `grpc_*`
  също.

### reportbaga — отделно репо
- `app-product/reportbaga` е git submodule към
  [bagalang/reportbaga](https://github.com/bagalang/reportbaga).
- `std`, `csvbaga`, `officebaga`, `pdfbaga`, `xmlbaga`,
  `bufbaga`, `tplbaga` остават тук; `apps/report` продължава да
  сочи path към `reportbaga`; `tests/report_test` също.

### raftbaga — отделно репо
- `app-product/raftbaga` е git submodule към
  [bagalang/raftbaga](https://github.com/bagalang/raftbaga).
- `std` остава тук; `tests/raft_test` / `raft_persist_test` също.

### queuebaga — отделно репо
- `app-product/queuebaga` е git submodule към
  [bagalang/queuebaga](https://github.com/bagalang/queuebaga).
- `std` остава тук; `tests/queue_test` също.

### querybaga — отделно репо
- `app-product/querybaga` е git submodule към
  [bagalang/querybaga](https://github.com/bagalang/querybaga).
- `std` остава тук; `tests/query_test` също.

### pgbaga — отделно репо
- `app-product/pgbaga` е git submodule към
  [bagalang/pgbaga](https://github.com/bagalang/pgbaga).
- `std` остава тук; `ormbaga` / `boilabaga` / `oauthbaga` /
  `bagadecimal` продължават да сочат `../pgbaga`; `tests/pg_test` също.

### pdfbaga — отделно репо
- `app-product/pdfbaga` е git submodule към
  [bagalang/pdfbaga](https://github.com/bagalang/pdfbaga).
- `std` и `bufbaga` остават тук; `reportbaga` продължава да сочи
  `../pdfbaga`; `tests/pdf_test` също.

### pbbaga — отделно репо
- `app-product/pbbaga` е git submodule към
  [bagalang/pbbaga](https://github.com/bagalang/pbbaga).
- `std`, `statusbaga`, `mdtbaga` остават тук; `cloudbaga` продължава
  да сочи `../pbbaga`; тестовете `pb_test` / `grpc_*` също.

### pathbaga — отделно репо
- `app-product/pathbaga` е git submodule към
  [bagalang/pathbaga](https://github.com/bagalang/pathbaga).
- `std` остава тук; `imgbaga` продължава да сочи `../pathbaga`;
  `tests/path_test` също.

### otelbaga — отделно репо
- `app-product/otelbaga` е git submodule към
  [bagalang/otelbaga](https://github.com/bagalang/otelbaga).
- `std` остава тук; `fmrbaga` / `cloudbaga` продължават да сочат
  `../otelbaga`; `tests/otel_test` / `otel_http_test` също.

### ormbaga — отделно репо
- `app-product/ormbaga` е git submodule към
  [bagalang/ormbaga](https://github.com/bagalang/ormbaga).
- `std`, `pgbaga`, `boilabaga` остават тук; `fmrbaga` / `oauthbaga` /
  `apps/api` / `apps/registry` продължават да сочат path към `ormbaga`;
  `tests/orm_test` / `orm_boila_test` също.

### officebaga — отделно репо
- `app-product/officebaga` е git submodule към
  [bagalang/officebaga](https://github.com/bagalang/officebaga).
- `std`, `zipbaga`, `xmlbaga`, `bufbaga`, `mdbaga` остават тук;
  `reportbaga` продължава да сочи `../officebaga`; `tests/office_test`
  също.

### oauthbaga — отделно репо
- `app-product/oauthbaga` е git submodule към
  [bagalang/oauthbaga](https://github.com/bagalang/oauthbaga).
- `std`, `httpdbaga`, `jwtbaga`, `tplbaga`, `ormbaga`, `pgbaga`
  остават тук; `tests/oauth_test` / `oauth_pg_test` също.

### metbaga — отделно репо
- `app-product/metbaga` е git submodule към
  [bagalang/metbaga](https://github.com/bagalang/metbaga).
- `std` остава тук; `fmrbaga` / `cloudbaga` / `boilaDB` продължават
  да сочат `../metbaga`; `tests/met_test` също.

### mdtbaga — отделно репо
- `app-product/mdtbaga` е git submodule към
  [bagalang/mdtbaga](https://github.com/bagalang/mdtbaga).
- `std` остава тук; `pbbaga` продължава да сочи `../mdtbaga`;
  `tests/mdt_test` също.

### mdbaga — отделно репо
- `app-product/mdbaga` е git submodule към
  [bagalang/mdbaga](https://github.com/bagalang/mdbaga).
- `std` остава тук; `officebaga` продължава да сочи `../mdbaga`;
  `tests/md_test` също.

### lsmbaga — отделно репо
- `app-product/lsmbaga` е git submodule към
  [bagalang/lsmbaga](https://github.com/bagalang/lsmbaga).
- Deprecated shim: `std` и `rocksbaga` остават тук; shims-ът
  продължава да сочи `../rocksbaga`.

### logbaga — отделно репо
- `app-product/logbaga` е git submodule към
  [bagalang/logbaga](https://github.com/bagalang/logbaga).
- `std` остава тук; `fmrbaga` / `cloudbaga` продължават да сочат
  `../logbaga`; `tests/log_test` също.

### kvbaga — отделно репо
- `app-product/kvbaga` е git submodule към
  [bagalang/kvbaga](https://github.com/bagalang/kvbaga).
- `std` остава тук; `rocksbaga` продължава да сочи `../kvbaga`;
  `tests/kv_test` и RESP клиентите в rocks/lsm тестовете също.

### jwtbaga — отделно репо
- `app-product/jwtbaga` е git submodule към
  [bagalang/jwtbaga](https://github.com/bagalang/jwtbaga).
- `std` и `httpdbaga` остават тук; `fmrbaga` / `oauthbaga`
  продължават да сочат `../jwtbaga`; `tests/jwt_test` също.

### jsonrpcbaga — отделно репо
- `app-product/jsonrpcbaga` е git submodule към
  [bagalang/jsonrpcbaga](https://github.com/bagalang/jsonrpcbaga).
- `std` и `httpdbaga` остават тук; `tests/jsonrpc_test` също.

### imgbaga — отделно репо
- `app-product/imgbaga` е git submodule към
  [bagalang/imgbaga](https://github.com/bagalang/imgbaga).
- `std`, `zipbaga`, `pathbaga` остават тук; `tests/img_test` също.

### httpdbaga — отделно репо
- `app-product/httpdbaga` е git submodule към
  [bagalang/httpdbaga](https://github.com/bagalang/httpdbaga).
- `std` остава тук; потребителите (`fmrbaga`, `boilaDB`, `cloudbaga`,
  `wsbaga`, `jwtbaga`, `oauthbaga`, `jsonrpcbaga`) продължават да сочат
  `../httpdbaga`; тестовете `http_test` / `h2_test` / `hpack_test` също.

### grebaga — отделно репо
- `app-product/grebaga` е git submodule към
  [bagalang/grebaga](https://github.com/bagalang/grebaga).
- `std` остава тук; `tests/grep_test` също.

### globbaga — отделно репо
- `app-product/globbaga` е git submodule към
  [bagalang/globbaga](https://github.com/bagalang/globbaga).
- `std` остава тук; `tests/glob_test` също.

### fmrbaga — отделно репо
- `app-product/fmrbaga` е git submodule към
  [bagalang/fmrbaga](https://github.com/bagalang/fmrbaga).
- `std`, `httpdbaga`, `jwtbaga`, `ormbaga`, `otelbaga`, `logbaga`,
  `metbaga` остават тук; `apps/api` / `apps/registry` продължават да
  сочат `../../app-product/fmrbaga`; тестовете `fmr_*` също.

### flagbaga — отделно репо
- `app-product/flagbaga` е git submodule към
  [bagalang/flagbaga](https://github.com/bagalang/flagbaga).
- `std` остава тук; `tests/flag_test` също.

### ctxbaga — отделно репо
- `app-product/ctxbaga` е git submodule към
  [bagalang/ctxbaga](https://github.com/bagalang/ctxbaga).
- `std` и `statusbaga` остават тук; `tests/ctx_test` също.

### csvbaga — отделно репо
- `app-product/csvbaga` е git submodule към
  [bagalang/csvbaga](https://github.com/bagalang/csvbaga).
- `std` и `bufbaga` остават тук; `reportbaga` / `apps/report`
  продължават да сочат path към `csvbaga`; `tests/csv_test` също
  остава тук.

### cloudbaga — отделно репо
- `app-product/cloudbaga` е git submodule към
  [bagalang/cloudbaga](https://github.com/bagalang/cloudbaga).
- `std`, `httpdbaga`, `metbaga`, `logbaga`, `otelbaga`, `pbbaga`
  остават тук; `tests/cloud_test` също.

### chatbaga — отделно репо
- `app-product/chatbaga` е git submodule към
  [bagalang/chatbaga](https://github.com/bagalang/chatbaga).
- `std` и `wsbaga` остават тук; `tests/chat_test` също.

### bufbaga — отделно репо
- `app-product/bufbaga` е git submodule към
  [bagalang/bufbaga](https://github.com/bagalang/bufbaga).
- `std` остава тук; `csvbaga` / `officebaga` / `pdfbaga` /
  `reportbaga` продължават да сочат `../bufbaga`; `tests/buf_test`
  също остава тук.

### boilabaga — отделно репо
- `app-product/boilabaga` е git submodule към
  [bagalang/boilabaga](https://github.com/bagalang/boilabaga).
- `std` и `pgbaga` остават тук; `ormbaga` продължава да сочи
  `../boilabaga`; `tests/orm_boila_test` също остава тук.

### bagadecimal — отделно репо
- `app-product/bagadecimal` е git submodule към
  [bagalang/bagadecimal](https://github.com/bagalang/bagadecimal).
- `std` и `pgbaga` остават тук; тестовете `tests/decimal_test` /
  `decimal_pg_test` също.

### rocksbaga — отделно репо
- `app-product/rocksbaga` е git submodule към
  [bagalang/rocksbaga](https://github.com/bagalang/rocksbaga).
- `std` и `kvbaga` остават тук; тестовете `tests/lsm_*` /
  `page_cache` / `sst_scan` / `shard` и `bench/rocks/` също.
- `lsmbaga` (deprecated alias) и `boilaDB` продължават да сочат
  `../rocksbaga`.

### boilaDB — отделно репо
- `app-product/boilaDB` е git submodule към
  [bagalang/boilaDB](https://github.com/bagalang/boilaDB).
- Общите пакети (`std`, `httpdbaga`, `metbaga`) остават тук;
  `rocksbaga` е отделен submodule; тестовете `tests/boila_*` и
  `bench/boila/` също остават тук.
- Clone: `git submodule update --init --recursive`.

### imgbaga 0.6.0 — lossy VP8 WebP
- VP8 keyframe decode (RFC 6386 / `golang.org/x/image/vp8`): bool
  decoder, tokens, iDCT/WHT, intra pred, loop filter, BT.601 YUV→RGB
  (libwebp-exact).
- `webp_decode` вече чете и `VP8 ` chunk, не само VP8L.
- ImageMagick lossy goldens: 8×8 red, 2×2 gray, 8×8 RGB quadrants.

### imgbaga 0.5.0 — VP8L lossless WebP
- Пълен VP8L decode: Huffman + LZ77 + color cache и четирите
  transform-а (predictor, color, subtract-green, indexing).
- ImageMagick lossless 2×2 / 4×4 / 8×8 goldens в `img_test`.
- Lossy VP8 остава info-only.

### imgbaga 0.4.0 — 16-bit PNG, GIF anim, TIFF, WebP info
- PNG 16-bit gray/RGB/grayA/RGBA + tRNS → RGBA8 (`sample/257`).
- GIF анимация: `gif_anim_decode`, кадри, delay (cs), disposal 2/3.
- TIFF: uncompressed + PackBits, 8-bit gray/RGB/RGBA, II/MM endian.
- WebP: info за VP8 / VP8L / VP8X; lossless decode само без transforms.

### imgbaga 0.3.0 — QOI, ICO, bilinear
- QOI encode/decode (Quite OK Image, lossless RGBA).
- ICO: decode PNG или 24/32-bit DIB (+ AND mask); encode като PNG-in-ICO.
- `img_resize_bilinear` (фиксирана точка); `img_resize` остава nearest.

### imgbaga — растерни изображения (PNG/JPEG/GIF/BMP/PNM)
- Нов пакет `app-product/imgbaga` по модела на Rust
  [`image`](https://github.com/image-rs/image): RGBA8 `Image`
  (`DynamicImage`), `img_guess` по magic bytes, `img_load` /
  `img_encode` / `img_open` / `img_save`, `imageops` (crop, flip,
  rotate90, nearest resize, grayscale, invert).
- PNG: 8-bit + packed 1/2/4, Adam7, tRNS; encode RGBA8 през
  `zipbaga` DEFLATE + zlib/Adler-32.
- JPEG: baseline SOF0 decode (Huffman, integer IDCT) и encode
  4:2:0 Annex K (`jpeg_encode` / `jpeg_encode_q`).
- GIF: първи кадър (LZW, GCE прозрачност, interlace).
- BMP 24/32 BI_RGB; PNM P2/P3/P5/P6. Sniff още за WebP/ICO/TIFF/QOI/HDR.
- Тест: `tests/img_test.baga`. Demo: `app-product/imgbaga/demo.baga`.

### boilaDB — топъл checkout без gmu write (P11-1c)
- Вече отворена база: checkout само `vec_get` (без `srv_write` на
  целия сървър). Data SQL не прави checkin — шардовете се пишат
  in-place. Schema DDL още putback-ва. 4 workers c=32 ≈7090;
  8w още не бие 4-те.

### boilaDB — shared data lock (P11-1b)
- Data SQL вече не държи exclusive `dmu`. Shared lock + hop-less
  per-shard mutex; schema DDL (CREATE/DROP/ALTER/TRUNCATE…) е
  exclusive (SELECT||DROP чака). `api/serve_mt_lock.baga`.
  Тест: `tests/boila_lock_test.baga`. Не е hop-per-GET owner
  нишка — 8 worker-а още не бият 4 на `mt_ladder`.

### boilaDB — mt_ladder върху pool (P11-1 измерен)
- Същ бинарник, 12 cores: `BOILA_WORKERS=4` c=32 **7070 ops/s**
  срещу `=0` (go_bg) **4769 (+48%)**. 8/12 worker-а са по-бавни
  (dmu). `bench/boila/results/mt-ladder-2026-08-14.md`.

### rocksbaga / boilaDB — WAL group над 64 KiB (K5b)
- `lsm_wal_group_begin/end`: докато групата е отворена, WAL буферът
  расте (таван 16 MiB) вместо да се цепи на няколко `WAL_OP_BATCH`.
  `boila_stmt_begin/commit` отварят/затварят групата. Torn tail на
  голямо INSERT+index маха цялото заявление. Residual: >16 MiB.

### boilaDB — P11-1 bounded worker pool
- HTTP и PG accept вече подават fd на фиксирани `BOILA_WORKERS` нишки
  (default 4, cap 64; `0` = стар `go_bg` per-conn). Пълната опашка
  блокира accept (backpressure). `/health` `mode=mt-pool` + `workers`;
  `/metrics` `boila_workers`. Тест: `tests/boila_pool_test.baga`.

### rocksbaga / boilaDB — WAL BATCH (K5) + torn-tail chaos
- Flush прозорецът е един `WAL_OP_BATCH` (op 19) с CRC. Replay пропуска
  скъсания batch цял — row+index заедно. `lsm_recover_test` chaos_* и
  `boila_chaos_test` (truncate на WAL опашката, reopen, DURABLE).

### rocksbaga / boilaDB — cost-based levels (Q2 остатък)
- Same-level pair-collapse на L1/L2/L3 (препрочит на целия слой при
  всеки втори файл) е махнат. Compact само при `nfiles ≥ compact_at` или
  byte target. boilaDB default `BOILA_TARGET_BYTES=1MiB`.
  1M insert: **10.4 GB / 3281 s → 1.35 GB / 169 s**; group commit 3120%.

### effects — M20 raise наистина дивергира
- `raise !E(v)` вече излиза от функцията (C: `return` след записа на слота;
  LLVM: `h_ret_zero` + недостижим `raise_dead`). Преди това само задаваше
  tag-а и продължаваше — код след `raise` (напр. `char_at` върху къс низ)
  можеше да падне преди caller-ът да види ефекта. M19 брои `raise` като
  изход (`if { raise } else { return }` е пълен). Оракул:
  `examples/effects_raise_diverge.baga`.

## [0.9.0] — 2026-08-14

### llvm — M21 generic fns (пълна мономорфизация)
- LLVM backend-ът вече емитва generic fn-ове per инстанция (recheck +
  synthetic имена + типова substitution) — M21, M23, M24 са с паритет:
  traits.baga, generics.baga, generic_structs.baga дават идентичен
  изход под C и LLVM (lli). Fix по пътя: литералните полета се инферират
  преди типовата проверка (M24) и instance lookup по type_eq вместо
  указателно равенство.

### llvm — M23/M24 паритет (traits + generic structs)
- LLVM backend-ът вече емитва impl методите (M23) и мономорфизираните
  struct типове `b_Pair_i64_str` per инстанция (M24): named типове + тела
  под substitution, литерали/полета по конкретния cname. Оракулът минава.
  Generic fn-ове (M21) остават C-only (ясна грешка).

### llvm — M20 effect payload runtime (raise/try/catch)
- LLVM backend-ът вече изпълнява payload ефектите runtime: thread-local
  `baga_eff` слот (tag + i/f/s/b), raise/try/catch lowering и phi фикс —
  incoming стойности само от хендлъри, които реално достигат merge
  (return вътре в catch). `examples/effects_payload.baga` дава идентичен
  изход под C и LLVM (lli).
- Оракулът пропуска `noreturn_bad` (очаквана checker грешка, M19).

### generics — M24 generic structs (мономорфизация)
- `struct Pair<A, B> { first: A, second: B }` — извод от литералите,
  изрични аргументи `Pair<i64, str> { … }` (lookahead, без сблъсък със
  сравненията), полета под substitution, swap-фабрики, impl за конкретни
  инстанции, Vec от инстанции. C имена `b_Pair_i64_str` per инстанция.
- Fix по пътя: вложена substitution (literal/field) се копира настрани —
  презаписването на активния fn-контекст даваше грешни свързвания.
- v1: LLVM backend-ът не ги поддържа (C-only); --rc helper-и за heap
  полета на generic struct не се генерират.
  Пример: `examples/generic_structs.baga`.

### traits — M23 trait/impl + статичен dispatch
- `trait Area { fn area(self) -> i64 }`; `impl Area for Rect { fn area(self: Rect) -> i64 { … } }`.
  Методите се викат `obj.m(args)` и се свързват статично (вътрешни имена
  `Trait.Type.method`, мономорфизация). Trait bounds на generic fn:
  `fn total<T: Area>(x: T) -> i64` — при инстанциране типът трябва да
  имплементира bound-а. Вериги (`r.scale(10).area()`), impl за builtin
  типове, нееднозначност е compile грешка.
- Fix по пътя: recheck-ът на generic инстанции чисти мемоизираните типове
  (infer кешира през n->type) — методният rewrite е деструктивен и се
  възстановява преди всяка инстанция.
- v1: статичен dispatch; LLVM backend-ът не ги поддържа (C-only).
  Пример: `examples/traits.baga`.

### verify — M22 статично проверяеми guarantee редове
- `spec guarantees:` ред, който се парсира като булев израз (върху input и
  `output`), се верифицира от `--verify` със същата дисциплина като
  `ensures` (PROVEN/REFUTED+контрапример/UNKNOWN); оброчено guarantee е
  червен резултат (exit 1). Прозата излиза като „проза — извън фрагмента".
  JSON: масив "guarantees" (result: "prose" за прозата).
  Пример: `examples/verify/spec_guarantees.baga`; gate в run_verify.sh.

### generics — M21 типови параметри (мономорфизация)
- `fn identity<T>(x: T) -> T`, `fn map<T, U>(v: Vec<T>, f: fn(T) -> U) -> Vec<U>`.
  Извод от аргументите + явни типови аргументи `map<i64, i64>(…)`.
- Проверка на тялото веднъж per инстанция (checker snapshot + re-infer);
  codegen емитва специализирани варианти `<име>__iN`. Транзитивни инстанции,
  ефекти (вкл. payload), struct полета през `T`, `Vec<T>`/`Map<K,V>`.
- v1: без generic fn като стойност / ламбди в тялото / spec / LLVM backend.
  Пример: `examples/generics.baga`.

### checker — M19 missing-return (не-void fn не може да падне от края)
- Fallthrough анализ: return/break/continue не падат; block → последен
  stmt; `if` без `else` пада; `while` пада, освен literal `while true` без
  break; `match` пада (arm-ският `return e` е стойност, не изход).
  Последният изразен statement е implicit return. M19b: типът на implicit
  return се сверява с връщания тип. Същият check в self компилатора
  (c2_check). Примери: `examples/noreturn_bad.baga`, `tail_return.baga`.

### effects — M20 payload-и (`!E(T)`, `raise`, `catch !E(name)`)
- Ефектът може да носи стойност: `fn f() -> i64 !NotFound(str)`.
  `raise !E(v)` предизвиква ефекта; `catch !E(name) => …` свързва payload-а
  с `name`; `?` го разпространява. Runtime модел: thread-local `baga_eff`
  слот (tag + i/f/s/b) — payload-less ефектите остават compile-time фикция
  без цена. Строга проверка на payload сигнатурата (raise/catch/?).
  Payload тип v1: i64/f64/bool/str/bytes. LLVM backend: compile-time
  фикция (C-only runtime). Пример: `examples/effects_payload.baga`.

### runtime — RC5 v1.0 owned-конвенция за struct/enum fn резултати (зад `--rc`)
- **v1.0a.** `return` на borrowed struct/enum (vec_get, поле, параметър,
  untrack-нат ident / match binding, if-клон) вече retain-ва през
  `rc_heap_tag` + `retain_S`/`retain_E` — същата конвенция „fn резултат =
  owned", която RC1 прилага за str/bytes/Vec. Match рамена ползват същия
  helper (без `void *` cast върху стойност). Ламбда параметри/capture-и
  се регистрират с heap tag. Struct литерал с untrack-нат ident поле
  (match binding) retain-ва през `__rc_sl`. Тест:
  `tests/owned_ret_rc_test.baga` (вкл. boila_ps_tok сценарий: Token от
  vec_get, drop на източника). Bare payload-less variant (`return None` /
  `return GBad`) минава през `__rc_ret` — няма C локал с името на варианта.
- **v1.0b.** Call site-ът вече знае, че резултатът е owned: struct/enum
  call temp в `vec_push`/`vec_set`/`map_set` е move (без retain, temp-ът
  се консумира); `s.inner = f()` / `s.e = f()` — owned дясно без retain;
  `match f()` scrutinee се регистрира през `rc_tmp_fresh`/`rc_heap_tag` и
  се release-ва след рамената. Enum ctor не е temp (иначе push(Some(x))
  би го пуснал). Разширени `calltemp_rc_test`, `nested_assign_rc_test`,
  `enum_box_rc_test`, `match_temp_rc_test`.
- Без флаг: бит-идентичен emit-c.

### runtime — RC5 v0.11 match scrutinee temp (зад `--rc`)
- Scrutinee на `match`, който е fresh heap temp, вече се track-ва от RC4
  регистъра: `rc_tmp_collect` слиза в `match_expr` (не в рамената — условни
  statement-и). Scrutinee-то се оценява безусловно веднъж преди рамената, а
  release-ът идва в края на statement-а — СЛЕД телата им, така че borrowed
  binding-ите (v0.6 конвенция) остават валидни. Покрива str/bytes/Vec/Map
  call temp-ове (`match concat(...)`), вкл. вложени temp-ове в тях, във
  всички позиции (void statement, let init, return, if/while cond wrap).
- Enum ctor scrutinee (`match Some(concat(...))`) се регистрира като temp
  с tag 6 и се release-ва с `release_E` след рамената — enum-ът притежава
  payload референциите от ctor сайта (v0.6 пр. 4) и досега никой не ги
  release-ваше (леак per-match).
- Свързан фикс (задължителен за v0.11): enum ctor с untrack-нат ident
  payload (match binding — borrowed копие; enum/struct fn резултат —
  неразличим) вече retain-ва payload-а (v0.6 пр. 4 документира това, но
  `rc_find` не виждаше binding-ите). Без това rebox от temp scrutinee
  (`let o2 = match Some(concat(...)) { Some(s) => Some(s), ... }`) обесваше
  новия enum след release-а на scrutinee-то (UAF). dead (drop()нати)
  локали — както досега, без retain.
- Граници (leak-safe): enum/struct fn резултат scrutinee (`match mk()`)
  НЕ се release-ва — payload-ът може да е borrowed (`return vec_get(...)`
  на enum не retain-ва, §v0.7 границата); temp-ове в рамената и в
  pattern-ите остават непокрити (условна оценка); вложени temp-ове в ctor
  payload аргумент — както досега (RC4 не слиза в ctor аргументи).
- `tests/match_temp_rc_test.baga` (11 случая, минава с и без `--rc`).
  Leak repro 500k итерации (ctor + str + fn-result scrutinee): RSS
  143.0 MB → 72.1 MB (остатъкът е `match mk()` границата); само покритите
  форми: 95.7 MB → 24.9 MB (остатъкът е вложеният temp в ctor payload).
  Без флаг: бит-идентичен emit-c (6 файла `cmp` срещу base build).

### runtime — RC5 v0.10 enum в контейнер/struct поле (зад `--rc`)
- Drop/scope exit/reassign/`drop()` на `Vec<E>`/`Map<K,E>` (E = sum enum с
  heap payload, v0.6) release-ват payload-ите на box елементите: `rc_box_rel`
  вече резолвира и enum типове — codegen генерира shim-ове
  `baga_rc_relf_<E>`/`baga_rc_retp_<E>` (release_E/retain_E по runtime tag
  през указател) до struct-овите от v0.2. Същият shim покрива `vec_set`/
  `map_set` overwrite (`*_box_rc` вариантите от v0.3) и `map_del`
  (`baga_map_del_*_rc`).
- Push/set сайтовете (`vec_push`/`vec_set`/`map_set`) за enum елемент/
  стойност: свеж ctor е move (payload-ът е owned от ctor сайта, v0.6);
  last-use ident е move (RC2); всичко останало (ident, borrowed, call
  резултат) се retain-ва — задължително, щом drop вече release-ва
  payload-ите. `vec_slice`/`vec_concat` на `Vec<E>` вървят през
  `*_box_rc` с `retp_<E>` shim (shallow box копия). Общ предикат:
  `rc_box_tracked` (struct с heap полета или enum с heap payload).
- `s.e = x` (enum-типизирано поле на track-нат struct локал): release_E на
  стария payload преди assign, retain_E на borrowed/неразличимо дясно
  (alias-safe ред като v0.4/v0.8), move при last-use ident, owned при свеж
  ctor. Механизъм: `rc_field_assign_enum` + `rc_emit_enum_field_release`.
- Struct с enum поле: `rc_struct_has_heap` е транзитивен и през enum полета
  (depth-aware взаимна рекурсия `rc_struct_has_heap_d`↔`rc_enum_has_heap_d`),
  `retain_S`/`release_S` викат `retain_E`/`release_E` за тях
  (`rc_nested_enum_field`). Покрива и struct САМО с enum поле.
- Бонус фикс: borrowed enum (`vec_get(...)`/поле), вграден директно в struct
  литерал, emit-ваше `baga_rc_retain((void *)__rc_sl.<field>)` върху enum
  стойност — compile error под `--rc`. Сега tag 6 отива в `retain_E`.
- Граници (leak-safe): enum payload в enum payload не се брои (както v0.6);
  `Vec<Vec<E>>` — shim-ът за вложен Vec е struct-only (v0.9), enum най-
  вътрешен елемент тече както досега; `s.e = f()` с fresh enum fn резултат
  leak-ва една референция (§v0.7 неразличимост borrowed/fresh); само плоско
  `ident.field`.
- `tests/enum_box_rc_test.baga` (22 случая, минава с и без `--rc`). Leak
  repro 500k итерации (Vec<E> push+drop, map_set overwrite, `s.e = x`
  overwrite, struct с enum поле scope exit): RSS 93.5 MB → 10.2 MB. Без
  флаг: бит-идентичен emit-c (6 файла `cmp` срещу base build).

### runtime — RC5 v0.9 `Vec<S>` във `Vec` (вложен контейнер, зад `--rc`)
- Drop/scope exit/reassign/field overwrite на `Vec<Vec<S>>` (S = struct с
  heap полета) release-ват полетата на S-овете във вътрешния vec: kind 3 на
  `baga_rc_release_vec` вече приема destructor (`elem_rel`); codegen
  генерира shim `baga_rc_relv_<S>` (до `relf`/`retp` от v0.2) и го подава
  от всички release сайтове (scope exit, temp release, `drop()`, reassign,
  `s.f = x`, `release_S`/`release_E` за Vec поле/payload). Покрива и
  транзитивни случаи (struct с `Vec<Vec<S>>` поле в контейнер).
- `vec_set` overwrite на външния върви през нови
  `baga_vec_set_vec_rc`/`baga_vec_set_vec_move_rc` (destructor fn pointer;
  retain на новия преди release на стария — alias-safe).
  `Map<K, Vec<S>>` не се покрива от checker-а изобщо (Vec не е валидна Map
  стойност) — няма какво да се пипа там.
- Граница (leak-safe, не корупция): дълбочина >2 (`Vec<Vec<Vec<S>>>`) и
  `Vec<Vec<str>>` остават на старото поведение — shim-ът е едно ниво, а
  str/bytes вътрешни елементи нямат shim. Enum като най-вътрешен елемент
  също (както v0.2 не покрива enum box-ове).
- `tests/vecvec_rc_test.baga` (15 случая, минава с и без `--rc`). Leak
  repro 500k push+drop и 500k vec_set overwrite: RSS 48.5 MB → 10.8 MB
  (като leak-free базата от v0.2). Без флаг: бит-идентичен emit-c.

### runtime — RC5 v0.8 `s.inner = x` (struct-типизирана цел, зад `--rc`)
- Field assign върху struct-типизирано поле (с heap полета, транзитивно)
  на track-нат struct локал: старото поле се release-ва рекурсивно
  (`baga_rc_release_<Inner>` от v0.5) преди assign. Alias-safe ред като
  v0.4 — новото се retain-ва ПРЕДИ release: `s.inner = s.inner` и
  `s.inner = t.inner` при alias не underflow-ват, borrowed източник
  оцелява смъртта на източника си.
- Дясно: свеж литерал е owned (без retain); tracked ident — retain, а
  при last-use — move (без retain); всичко останало (call/поле/vec_get/
  untrack-нат ident) се retain-ва — struct fn резултат може да е
  borrowed (§v0.7 границата), посоката е leak-safe (fresh call leak-ва
  една референция — RSS не пада за `s.inner = mk(...)`, документирано).
  Само плоско `ident.field` (като v0.4). Механизъм:
  `rc_field_assign_struct` + `rc_emit_struct_field_release`.
- `tests/nested_assign_rc_test.baga` (12 случая; `borrowed_outlive`
  фейлва на стария codegen — четене на освободено поле). Leak repro
  500k литерален overwrite: RSS 24.9 MB → 10.6 MB. Без флаг:
  бит-идентичен emit-c.

### runtime — RC5 v0.7 call temp-ове в box push (зад `--rc`)
- Temp резултат от call (`f()`/`to_str`), директен аргумент на
  `vec_push`/`vec_set`/`map_set` (str/bytes/Vec стойност), се прехвърля в
  контейнера: `_move` helper без retain и без release в края на
  statement-а (като RC3 за last-use ident; fn резултатът е owned по
  конвенция). Вложени temp-ове (`push(v, g(f()))`): външният е move,
  вътрешният се release-ва както досега. Механизъм: `rc_tmp_find` +
  консумация на temp записа (site=NULL) след emission.
- struct/enum box temp-ове НЕ се move-ват: резултатът може да е borrowed
  (`return vec_get(...)` на struct не retain-ва — boilaDB `boila_ps_tok`
  връща `Token` от vec_get), move би споделил чуждата референция →
  UAF/underflow при drop на източника. Остава retain + temp-ът тече
  (leak-safe, 48.5 MB vs 25.0 MB на 500k push+drop спрямо литералния
  move — документирана граница).
- `tests/calltemp_rc_test.baga` (12 случая, вкл. identity/borrowed-result
  fn, temp ползван два пъти → не е move). Typed temp-овете бяха
  балансирани и преди (retain+release двойка) — RSS flat (33 MB на 500k
  push(f())), печалбата е елиминираната двойка. Без флаг: бит-идентичен
  emit-c.

### runtime — RC5 v0.6 enum payload-и като собственици (зад `--rc`)
- Enum с heap payload (str/bytes/Vec/Map/struct с heap) получава
  `baga_rc_retain_<E>`/`baga_rc_release_<E>` със switch по runtime tag.
  Локал се регистрира (tag 6) само при свеж ctor или alias на track-нат.
- Ctor call site: fresh payload owned, borrowed се retain-ва, last-use
  ident е move. Reassign release-ва стария payload. Match binding-ите
  остават borrowed. Design: `docs/memory-rc-enum-bg.md`.
- `tests/enum_rc_test.baga` (8 случая). Leak repro 500k ctor: RSS
  39.6 MB → 10.9 MB. Без флаг: бит-идентичен emit-c.

### runtime — RC5 v0.5 вложени struct полета (зад `--rc`)
- `rc_struct_has_heap` е транзитивен; `retain_S`/`release_S` рекурсират в
  struct-типизирани полета. Forward декларации на всички RC helper-и.
- Литерал с borrowed вложен struct (`Pair { w: p.w }`) retain-ва през
  `baga_rc_retain_<T>(__rc_sl.<field>)`. Случаи 18-21 в struct_rc_test.
- Leak repro 500k вложени литерала: RSS 38.5 MB → 10.8 MB.

### runtime — RC5 v0.4 release на старото поле при `s.f = x` (зад `--rc`)
- Heap поле (str/bytes/Vec/Map) на track-нат struct локал се release-ва
  преди overwrite. Alias-safe ред (retain преди release): `w.s = v.s` и
  `w.s = w.s` не underflow-ват. Fresh дясно — owned, без retain. Само
  плоско `ident.field` (по-дълбоки пътеки = двойна оценка).
- Случаи 14-17 в `tests/struct_rc_test.baga`. Leak repro 500k field
  overwrite: RSS 39 MB → 10.9 MB.

### runtime — RC5 v0.3 overwrite/del на box елементи (зад `--rc`)
- `vec_set`/`map_set` върху съществуващ slot/ключ release-ват полетата на
  стария box преди memcpy; `map_del` release-ва полетата + free-ва pv на
  откаченото entry. `*_box_rc`/`baga_map_del_*_rc` варианти с destructor
  fn pointer; без флаг пътеките са непроменени.
- Alias-safe ред (retain преди release): `vec_set(v, 0, vec_get(v, 0))` и
  `map_set(m, k, map_get(m, k))` не underflow-ват — случаи 10-13 в
  `tests/struct_rc_test.baga`. Leak repro 300k overwrite+del: RSS
  72 MB → 10.9 MB.

### runtime — RC5 v0.2 container drop на struct полета (зад `--rc`)
- Drop/release на `Vec<S>`/`Map<K,S>` release-ва полетата на box
  елементите: destructor fn pointer в `baga_rc_release_vec`/`_map` +
  shim `baga_rc_relf_<S>` (forward-declared). Leak repro 500k push+drop:
  RSS 64 MB → 10.9 MB.
- `vec_push`/`vec_set`/`map_set` на свеж struct литерал е move в box-а
  (без втори retain — литералният temp няма кой да го пусне).
- `vec_slice`/`vec_concat` на `Vec<S>` под `--rc`: `*_box_rc` с retain shim
  `baga_rc_retp_<S>` — shallow box копията споделят полетата, иначе drop на
  двата вектора пуска два пъти (`std/vec_struct_test` underflow).
- Без флаг: бит-идентичен emit-c. Пълна батерия `--rc`: 151/156 (база).

### runtime — RC5 v0.1 struct field owners (зад `--rc`)
- Локален struct със `str`/`bytes`/`Vec`/`Map` полета се пуска при scope
  exit. Само свеж литерал + alias. `tests/struct_rc_test.baga`.

### runtime — go/chan heap: решение A
- Без tag-нат payload и без `go` capture retain. Hop-ът остава през
  `*_h` + RC1.4 immortal retain (`docs/memory-rc-chan-bg.md`).

### runtime — RC4 v0.3 cond temps (зад `--rc`)
- Temp-ове в `if`/`while` условия и `for`-range hi се release-ват на всяка
  оценка (GNU statement-expression). Leak repro 200k итерации: 10 MB vs 141 MB.
- Тестове: `while_cond_temp`, `if_cond_temp`, `for_hi_temp` в temp_test.

### runtime — RC1.4 handle-escape retain (зад `--rc`)
- `map_h`/`str_h`/`bytes_h` retain-ват обекта: handle-ът е immortal escape,
  иначе scope release обесва картата (`boila_open_mt` → `nb=0` SIGFPE).
- `tests/boila_ts_test.baga` минава с `--rc`. Пълна батерия: 150/155.

### boilabaga + ormbaga — boilaDB PG-wire adapter
- New package `app-product/boilabaga`: client adapter to boilaDB over
  PostgreSQL wire (`boila_connect_env`, dialect DDL helpers).
- ormbaga: `orm_from_conn`, `orm_connect_boila_env`, `orm_connect_auto_env`
  (`ORM_BACKEND=boila|pg`), dual-safe `sql_ident` + history DDL,
  `ormbaga_boila_migrations`, `orm_pool_from_boila_env` /
  `orm_pool_from_auto_env`, `examples/demo_boila.baga`.
- fmrbaga: `fmr_open_db` + config honor `ORM_BACKEND=boila` (BOILA_* DSN).
- apps/api: `api_migrations_auto`, boila-safe models (MAX id), `fmr-run`
  + `.env.example` document dual backend.
- Live test: `tests/orm_boila_test.baga` (needs `serve_pg` on :6575).

### boilaDB — Q-distinct-limit: DISTINCT during project + LIMIT stop
- Projection keeps uniques via `boila_distinct_offer` (no full project
  then `distinct_rows`).
- `DISTINCT … LIMIT n` without order_after stops after `offset+n` uniques.
- Tests: `distinct_limit`, `distinct_limit_off`, `distinct_order_lim`.

### boilaDB — Q-fetch-topn: ORDER BY + LIMIT bounded fetch
- Plain `SELECT … ORDER BY col … LIMIT n` keeps only `offset+n` best
  source rows via `boila_topn_offer` during GET (no full-table sort).
- Same path for IS [NOT] NULL fetch; ORDER BY alias/expr still order_after.
- Tests: `fetch_topn`, `fetch_topn_off`, `fetch_topn_multi`.

### boilaDB — Q-fetch-limit: early-stop plain SELECT LIMIT
- `SELECT … LIMIT n` without ORDER BY / DISTINCT / expression WHERE stops
  GETs after `offset+n` kept rows (`boila_fetch_need` in fetch + isnull).
- `boila_fetch_pks` unchanged (JOIN outer / kNN cands need the full set).
- Tests: `fetch_limit`, `fetch_limit_off`, `fetch_limit_where` in
  `boila_p5_test`.

### boilaDB — Q-knn-pref-pks: lighter kNN pre-filter cands
- PK-range pre-filter builds cand pks via `boila_fetch_pks` (no row Vec).
- `hnsw_brute_cands` scores into a size-k top window (not full score lists).

### boilaDB — Q-join-inner-pks: hash without full inner rows
- No-right-index JOIN builds `key → Vec<pks>` (`boila_hj_build_pks`);
  probe GETs each matching pk (no `fetch_all` row materialize).
- Applies to both `join_rows` and `join_into_fold`.

### boilaDB — Q-join-sorted-outer: left ORDER BY + LIMIT early-stop
- When `ORDER BY` is only outer-table cols: sort outer pks by those keys,
  probe in order, stop after `offset+limit` kept rows (no full join output).
- Mixed/right ORDER BY still uses top-N heap (`boila_topn_offer`).
- Tests: `join_sorted_outer`, `join_topn_right`.

### boilaDB — Q-join-topn: ORDER BY + LIMIT bounded join output
- `JOIN … ORDER BY … LIMIT n` keeps only `offset+n` best wide rows via
  `boila_topn_offer` (sorted window); avoids materializing full join output.
- Tests: existing `join_hash` LIMIT 2; `join_topn_desc`.

### boilaDB — Q-join-limit: early-stop JOIN probe
- Non-agg `JOIN … LIMIT n` (no ORDER/DISTINCT/expression WHERE) stops
  the outer probe after `offset+n` kept rows (right filters during emit).
- Tests: `join_limit`, `join_limit_off` in `boila_p5_test`.

### boilaDB — Q-stream-join-outer: stream left probe
- `boila_fetch_pks` collects matching outer pks (no row Vec); join probe
  GETs one outer row at a time (index NL + hash).
- Agg fold path uses the same outer stream; no-ix always hashes on inner.
- Unfiltered left scan is live keys only until probe.

### boilaDB — Q-stream-ix: isnull / fts / knn into hash-agg
- Stream path folds `IS [NOT] NULL`, FTS `@@`, and kNN hits without
  building a full result `Vec` (kNN via `boila_knn_hit_pks` + GET+fold).
- Tests: `isnull_agg`, `fts_agg`, `knn_agg`.

### boilaDB — Q-stream-join / stream xw for agg
- `JOIN` + `GROUP BY`/agg folds matches during probe (`boila_join_into_fold`)
  without materializing the join output row set.
- Stream agg also applies expression `WHERE` (`has_xw`) per source row.
- Tests: `boila_p5_test` join_agg_*; `boila_xwhere_test` agg_over_xw.

### boilaDB — K3j/K3k continuous ROLLUP (S5 pre-agg)
- `CREATE ROLLUP name ON t USING time_bucket('1m', ts) [SUM(col)]`
  maintains per-bucket count(*) + optional sum on INSERT/UPDATE/DELETE.
- Unfiltered `GROUP BY time_bucket` → rollup cells (O(buckets)).
- **K3k partial window:** complete buckets via point GETs; partial
  edge bands via tight secondary range + fold.
- S5 @100k: full **~2.9 ms**; ts window last-10k **~21 ms** (was ~38 ms
  raw). Tests: `tests/boila_rollup_test.baga`.

### rocksbaga / boilaDB — K3i prefix live fold (S5 cold path)
- Prefix SCAN rebuild no longer builds a full-shard live map then
  filters: `lsm_live_map_prefix_kb` folds only matching keys.
- SST path (`sst_fold_prefix_into`): skip disjoint files via
  first/last key, restart-block seek to prefix, early-stop once keys
  pass the prefix range; mem/tomb filtered by prefix. Cluster rebuild
  uses the same per-shard fold.
- boilaDB table / secondary scans (`boila_scan_pref*`) inherit the
  cheaper cold rebuild when index/sys keys share the shard.
- S5 rebench: full-table still ~GET-bound (~174 ms @100k); ts-ix window
  ~45 ms OK. Tests: `sst_scan_test` K3i, `lsm_test` k3i_prefix_*.

### Language — LLVM parity for `Map` (full runtime in IR)
- The whole chained hash-table runtime is built as lazy IR functions,
  mirroring the C preamble function by function: `baga_map_hash_{str,i64,
  bytes}` (FNV-1a / splitmix mix), `baga_map_new`, `baga_map_slot` /
  `baga_map_slot_b` (memcmp compare for bytes keys, R67), `baga_map_put` /
  `baga_map_put_b`, `baga_map_rehash` (doubling at load 3/4, rehash per
  ktag), `baga_map_len`, and the typed ops `baga_map_{set,get}_<key>_<val>`
  for str/i64/bytes keys × i64/str/f64/bytes/box values plus
  `baga_map_{has,del,keys}_<key>` — generated from decoded name suffixes.
  `map_h`/`h_map` (R55) are PtrToInt/IntToPtr casts like `str_h`.
- `TYPE_MAP`/`Map` lower to `baga_Map*`; the NODE_CALL lowering mirrors
  codegen_c: key/value suffixes from the checker-fixed `mt->key`/`mt->elem`,
  struct and sum-enum values ride the box path (`pv` = malloc'd copy;
  missing key → zero struct / tag-0 enum via a zeroed alloca).
- Oracle example `examples/map.baga` (str/i64 keys, overwrite, has/del/len,
  rehash past the 3/4 load factor, struct box values, keys aggregation) —
  both backends agree byte-for-byte, including bucket iteration order.
- Docs: the "Map — C backend only" notes are gone (en+bg); the stale
  "bytes mutators C-only" note (S2) is fixed too. `drop` is now the only
  documented C-only builtin.

### Language — LLVM parity for sum enums (L3 closed)
- Sum enums lower to a named `{ i64 tag, [N x i64] u }` struct (LLVM has
  no unions — `N = max(1, ceil(max payload ABI size / 8))`, sized via a
  default-layout `TargetData`); payload store/load is a GEP into the
  array + bitcast to the payload pointer type. Lazy IR constructors
  `b_Res__b_Ok(payload)` mirror the C `static inline` ones; payload-less
  variants are `{ tag, 0 }` values; `match` emits the same tag chain as
  codegen_c with the binding in an arm-scoped alloca. Tags follow
  declaration order in both backends.
- Works for `bytes`/`f64`/struct payloads, sum enums as struct fields and
  fn params/returns, nested `match`, qualified `Enum::Variant` paths
  (`NODE_PATH` values are new in the LLVM backend), and `Vec<enum>` via
  the existing box helpers. Enum bodies are set in a fixed-point pass
  over payload sized-ness, so struct↔enum acyclic graphs just work and
  by-value cycles are an honest compile error.
- The five `sum types (L3) — само C бекенда` refusals are gone; oracle
  example `examples/sum_enum.baga` diffs both backends.

### oauthbaga — O7: DB connection pool (`OAUTH_WORKERS=N`)
- PG mode gains a fixed worker pool per node (fmrbaga `FMR_WORKERS`
  idiom): N go_bg workers pull accepted fds from a buffered chan, each
  holding one long-lived DB connection — SCRAM once per worker instead
  of connect+auth per HTTP request.
- Self-healing: store ops thread `OrmDb` by value, so every checkout
  probes `SELECT 1` (1 RTT) and reconnects on a silently dropped
  connection. `OAUTH_WORKERS=0` (default) keeps the per-connection idiom.
- `scripts/run_tests.sh` runs `oauth_pg_test` a second time with
  `OAUTH_WORKERS=2` (verified: both nodes boot in pool mode, full OAuth
  flow + DB-level proofs pass).

### Language — L6 struct module qualification
- Same-named structs in different modules no longer collide in gcc: a
  pre-pass renames them to `module.Type` (like functions), type
  annotations and literals resolve scoped (own module wins, unique
  import, else ambiguity error with hint), and `mod.Type` works in both
  type position and literals (`util2.Rec { v: 7 }`). Duplicate struct in
  one module and duplicate enum across modules are now clean checker
  errors instead of C compiler noise.
- Tests: struct probes in `tests/ns_alias_test.baga` + two negatives in
  `run_tests.sh`; both backends.

### Language — L6 import alias (`import "p" as name`)
- `import "modb/util.baga" as util2` renames the module: qualified calls
  `util2.f()`, ambiguity hints use the alias. Closes the last L6 gap —
  two imported files sharing a basename no longer collide as "duplicates
  in one module". New `as` keyword; one alias per file (same alias
  re-import is idempotent, a different one is a compile error).
- Tests: `tests/ns_alias_test.baga` + `tests/ns_alias/{mod1,mod2}/util.baga`,
  three negatives in `run_tests.sh`; works in both backends.

### Language — LLVM parity for function values / closures (L5 closed)
- fn values are `cell2(code, env)` i64 handles in both backends:
  `TYPE_FN`/`NODE_TYPE_FN` lower to i64; named refs and module-qualified
  refs get lazy `__clo` IR wrappers; `NODE_LAMBDA` emits an env struct
  (by-value captures) + wrapper; calls through fn values are indirect
  calls through the handle.
- Oracle example `examples/closures.baga` (named refs, capture-by-value
  semantics, closure factory, str captures, `Vec<fn>`) — both backends
  agree.

### Language — LLVM parity for `Vec<struct>` (L4 closed)
- LLVM box helpers `baga_vec_{push,get,set,slice,concat}_box` (malloc-boxed
  element copies, size from the call site) mirror the C preamble; the
  `Vec<struct>` honest refusal is gone for named struct types.
- Oracle example `examples/vec_struct.baga` (push/get/set, nested
  `Vec<Pt>` field, slice/concat, grow past cap) — both backends agree.
- `Map<K,struct>` stays C-only (the LLVM backend has no `Map` at all).
- Docs: `Map` keys are `i64`/`str`/`bytes` (R67 follow-up), `map_keys`
  row, `Vec<struct>` both-backends note (en+bg).

### Language — LLVM parity for `bytes_h`/`h_bytes`
- LLVM builders: `baga_bytes_h` (malloc-boxed `{data,len}` header → i64)
  and `baga_h_bytes` (inverse; `h=0` → empty bytes), wired into bmap.
- Oracle example `examples/bytes_handle.baga` diffs both backends.
- With this, every cross-thread handle builtin has LLVM parity except
  `map_h`/`h_map`, which stay C-only by design (the LLVM backend has no
  `Map` at all).

### RocksDB path R66 — PARALLEL binary job payloads
- Language: `bytes_h` / `h_bytes` builtins (C backend) — box `baga_bytes`
  header for zero-copy hop on `chan(i64)`.
- workers: SET/SETEX values via `bytes_h` + `put_b`; GET bulk via
  `bytes_h`; `lsm_mb_rep_to_bytes` for RESP framing.
- PARALLEL serve stores ordered replies as `Map<i64, bytes>`.
- Tests: `r66_*` in `tests/workers_test.baga` (NUL round-trip).

### RocksDB path R65 — binary RESP values (safe)
- `kvbaga/resp`: `resp_parse_command_b` / `resp_encode_command_b` /
  `resp_round_trip_b` / `resp_parse_bulk_b` (`Vec<bytes>`).
- Legacy `resp_parse_command` kept (implemented via `_b` + `str_of_bytes`)
  so kvbaga + existing tests are unchanged.
- rocksbaga serve (poll / MT / CF) parses with `_b`; SET/SETEX/MSET/APPEND
  and CF.SET use `*_put_b` (NUL-safe values). Keys remain `str`.
- PARALLEL workers still str-pack values (documented residual).
- Tests: `r65_*` in `tests/lsm_test.baga`; full kv/cf/shard/scan green.

### RocksDB path R64 — per-CF block-cache policy
- `opt_cache` / `.cfs` 5th field `cache_pages`; `lsm_cf_setopt_one` for
  single-key updates (`flush_at` | `compact_at` | `cache_pages`).
- RESP `CF.SETOPT name cache_pages N` (alias `cache`); live `pc_new`.
- Fix: single-key SETOPT no longer clears sibling options (R61 bug).
- Tests: `r64_*` in `tests/cf_test.baga`.

### RocksDB path R63 — streaming SCAN
- Epoch snapshot cache on `LsmDB` / `LsmCluster`: one live fold per
  `write_epoch`×MATCH; later SCAN pages are O(count) slices.
- put/del invalidate via `lsm_scan_touch`; snapshot freed at cursor end.
- Unordered (Redis SCAN); `KEYS` still sorted. Tests `r63_*`.

### RocksDB path R62 — backup tool (checkpoint + ship)
- **`db/backup.baga`:** BAGABK1 meta (`<dest>.backup`) with per-file size +
  crc32c; `lsm_backup_create` / `lsm_cluster_backup_create`,
  `lsm_backup_verify`, `lsm_backup_ship` / `lsm_backup_restore`.
- **CLI:** `tools/backup.baga` — `BACKUP_MODE=create|verify|ship|restore`.
- **RESP:** `BACKUP dest` (poll + MT).
- Backup layout is openable with plain `lsm_open` / `lsm_cluster_open`.
- Tests: `r62_*` in `tests/lsm_test.baga` (incl. corrupt→verify fail).

### Language — LLVM parity for R51/R54 builtins
- `str_h`/`h_str`/`bytes_put` have LLVM builders now (PtrToInt / IntToPtr /
  bounds-checked memcpy); llvm_oracle green. `map_h`/`h_map` stay C-only
  (LLVM backend has no `Map`); the R52 thread-local arena is C-runtime
  only (LLVM allocs are plain `malloc`, already thread-safe).
- Docs: builtins table + free-list text updated (mutex → `__thread`).

### RocksDB path R60/R61 — CHECKPOINT over RESP + per-CF options
- **R60:** `CHECKPOINT dest` command (poll + MT); per-shard copies when
  sharded. Wire-tested end-to-end.
- **R61:** `lsm_cf_setopt` + RESP `CF.SETOPT name flush_at|compact_at v`,
  persisted in `.cfs`; `r61_*` tests.

### RocksDB path R58/R59 — checkpoint + poll command parity
- **R58:** `lsm_checkpoint(db, dest)` — RocksDB-style Checkpoint: flush,
  copy live SST/bloom files, fresh MANIFEST; the copy opens with plain
  `lsm_open(dest)`. Tests `r58_*`.
- **R59:** poll path (cluster exec) gains MGET/MSET/DECR/APPEND/TYPE —
  same command surface as MT mode.

### RocksDB path R56/R57 — MT command parity + active expire
- **R56:** `lsm_mt_exec` — MGET/MSET, INCR/DECR, APPEND, TYPE, EXISTS,
  EXPIRE/TTL/PERSIST, DBSIZE, SAVE/BGSAVE inline in MT mode (one shard
  locked at a time). Perf held at 97/94/98% of Redis (pipe=16, shards=8).
- **R57:** Redis-style active expire — shared `"sid:key" -> deadline`
  index + 100 ms sweeper thread; tombstones due keys verified still
  expired. DBSIZE 3→1 three seconds after SETEX 1, no reads.

### Language R55 + RocksDB path — hop-less MT serve (Redis parity)
- **`map_h` / `h_map` builtins** (C backend): shared `Map<i64, LsmDB>`
  through the go_bg i64 ctx.
- MT serve no longer hops commands through worker threads: shard dbs are
  boxed map entries; conn threads execute `lsm_put/get/del/put_ex` inline
  under per-shard mutexes (all shard keys inserted before the first go_bg,
  so the map never rehashes at runtime).
- vs Redis 8.x (8 clients, shards=8): pipe=16 **104/93/98%**, pipe=64
  **98/94/99%** (PING/SET/GET). Smoke: SET/GET/DEL/SETEX + restart
  recovery pass.

### Language R54 — `bytes_put` builtin (bulk in-place append)
- `bytes_put(dst, off, src)` — memcpy append into a preallocated buffer.
  rocksbaga MT serve assembles pipelined replies in a persistent per-conn
  scratch instead of an O(batch²) `bytes_concat` chain: GET pipe=64
  114k→289k (34→87% of Redis); pipe=16 shards=8 → 99/85/86% PING/SET/GET.

### RocksDB path R53 — worker drain
- Shard workers drain up to 64 queued jobs per wakeup (`lsm_worker_one` +
  `chan_try_recv`). Neutral on throughput (R50 already batched).

### Language/runtime R52 — thread-local arena (MT scalability fix)
- **arena + free lists are `__thread` now.** Before, one global
  `baga_alloc_mu` serialized every allocation in every thread — a real GIL
  for `go_bg` workloads. MT RESP soak vs Redis (8 clients, pipe=16):
  SET 96k→175k (33→70%), GET 71k→189k (31→85%), PING ~99%.
- Safe because `str` is arena-bound (never freed); free-list blocks are
  interchangeable raw memory across threads. C backend only.

### Language R51 — `str_h` / `h_str` unsafe handle casts
- `str_h(str) -> i64`, `h_str(i64) -> str` — zero-copy cross-thread handoff
  through i64 channels. Used by rocksbaga workers (job/reply payloads no
  longer byte-loop packed). C backend (LLVM parity TODO).

### RocksDB path R47–R50 — MT serve correctness + CF polish
- **R47 (fix):** job carries its own reply chan; MT conn threads no longer
  steal each other's replies from one shared done chan (soak hang at 8
  clients). `lsm_mt_submit` takes `rc`.
- **R48:** `lsm_cf_flush` runs the per-family L0..L3 compaction chain;
  shared WAL rotates once every family is clean (no unbounded WAL/replay).
- **R49:** `lsm_cf_drop` + RESP `CF.DROP`; WAL replay skips dropped CFs.
- **R50:** MT conn thread batch-submits a whole pipeline, then waits in
  order.
- bench harness: `setsid` + process-group kill; refuse pre-bound ports
  (orphaned zombie servers were silently benched).
- Tests: `r48_*`/`r49_*` in `tests/cf_test.baga`.

### RocksDB path R46 — durable CF names + RESP CF.*
- **`<dir>.cfs`:** atomic name→id map (`next_id` + lines). Loaded on
  `lsm_cf_open`, saved on create/close.
- **`lsm_cf_resolve(name, create)`** / **`lsm_cf_list`**.
- **RESP (`LSM_CF=1`):** `CF.CREATE`, `CF.SET` (auto-create), `CF.GET`,
  `CF.DEL`, `CF.LIST`; plain `SET`/`GET`/`DEL` hit default CF 0.
- Tests: name persistence + `lsm_exec_cf` in `cf_test`.

### RocksDB path R44 — shared-WAL column families
- **`db/cf.baga`:** multiple CFs share one `<dir>.wal`; SST/MANIFEST per CF
  (`dir` for CF 0, `dir.cf{n}` for others).
- **WAL v2 ops 17/18:** CF put/del with `cf_id`; legacy 1/2 = CF 0. Replay
  returns `cfs` vector; single-store open applies only CF 0.
- API: `lsm_cf_open/create/put/get/del/flush/close`. Flush writes SST without
  rotating the shared WAL.
- Tests: `tests/cf_test.baga` (isolation + reopen).

### RocksDB path R45 — multi-core RESP (MT serve)
- **`LSM_SERVE_MT=1`:** accept loop `go_bg`s each connection; connections
  submit to the same per-shard workers (R33–R37). Fd-scoped job ids +
  packed work channels (no shared Map across threads).
- Multi-submitter: `lsm_parallel_alloc_id` mutex restored; `lsm_mt_submit` /
  `lsm_mt_wait` for conn threads.
- Smoke: concurrent SET/GET on two conns via shared workers.

### RocksDB path R43 — SCAN
- **`lsm_scan` / `lsm_cluster_scan`:** cursor pages over sorted live keys;
  optional `MATCH` glob (`*`, `pre*`, `*suf`, `*mid*`) and `COUNT` (default 10).
- **RESP `SCAN cursor [MATCH pat] [COUNT n]`:** reply `[next, [keys…]]`.
  Cursor is a 0-based index; `0` starts/ends (Redis-compatible shape).
  Honest: each call still folds the live key set (like KEYS); paging is for
  client UX, not a cheaper fold.
- Tests: `tests/scan_test.baga`.

### RocksDB path R42 — FLUSHDB / FLUSHALL
- **`lsm_path_wipe` / `lsm_flushdb`:** close, delete WAL/MANIFEST/SST for a
  path, reopen empty (knobs preserved).
- **`lsm_cluster_flushdb` / `lsm_multi_flushdb` / `flushall`:** per-DB or all
  logical DBs (0..max_db-1), including not-yet-opened paths.
- **RESP:** `FLUSHDB` on selected cluster; `FLUSHALL` at multi-DB serve layer.
- Tests: `multidb_test` flushdb isolation + flushall.

### RocksDB path R41 — multi-DB / SELECT namespaces
- **`db/multidb.baga`:** Redis-style logical DBs (0..15, `LSM_MAX_DB`).
  Path: `dir` for 0, `dir.db{n}` for n>0; each is a full `LsmCluster`
  (independent WAL/SST/MANIFEST). Lazy open; not RocksDB ColumnFamily
  (no shared WAL).
- **RESP `SELECT index`:** per-connection active DB on poll + serial serve.
  Parallel mode returns an explicit error for SELECT.
- Tests: `tests/multidb_test.baga`.

### RocksDB path R40 — version edit log
- **`dir.manifest.log`:** append-only edits `A gen level` / `D gen` / `N next`.
  Load = atomic snapshot + log replay. Flush/compact append one fsync batch
  instead of rewriting the full gens list every time.
- **Compact threshold:** after 32 edit lines (or on `lsm_close`) fold log into
  an R39 atomic snapshot and clear the log.
- API: `lsm_manifest_log_add` / `log_merge` / `maybe_compact` / `log_lines`.
- Tests: `manifest_test` log apply + crash reopen; recover/lsm green.

### RocksDB path R39 — atomic MANIFEST publish
- **`lsm_write_manifest`:** write `dir.manifest.tmp` + `fsync` on the same fd,
  then `unlink` live + `fs_rename` tmp→live. Crash mid-write leaves the
  previous MANIFEST intact (was in-place truncate via `write_file`).
- **`lsm_format_manifest`:** pure encode helper for tests/tools.
- Tests: `manifest_test` rewrite/reopen; `lsm_recover_test` green.
- Durable engine still ~94% of RocksDB PUT (fsync-bound); this is a
  correctness ship, not a throughput win.

### RocksDB path R38 — reply coalesce (writev-like)
- **Poll / serial / parallel RESP:** batch all replies from one read (or one
  ordered flush) into a **single `tcp_write_bytes`** instead of one syscall
  per command. Parallel drain marks dirty fds and flushes each once.
- Sample (vlen=100, `LSM_SYNC_EVERY=10000`):

  | config | SET before (R37) | SET R38 |
  |--------|-----------------:|--------:|
  | P0 c4×pipe16 | ~104k | **~207k** |
  | P0 c1×pipe64 | (not R37) | **~195k** |
  | P1 c4×pipe16 | ~70k | **~77k** |

  pipe=1 unchanged (one reply per RTT). Coalesce is the pipeline win.

### RocksDB path R37 — cheaper parallel hop
- **Typed reply codes** on the done channel (`OK/NIL/INT0/INT1/BULK/ERR`) —
  no packing of `+OK\r\n` / `$-1\r\n` every SET/miss.
- **Short-string pack:** empty → `0`; `n≤8` → single `cell2(n, word)`;
  longer strings keep the word-list form. GET/DEL skip packing unused val.
- **Id alloc lock-free** (single submitter: network thread / tests).
- Sample P1×4 after R37 (pipe=1 unless noted): c1 SET ~12.8k; c8 SET ~32k
  (~86% of P0); c4×pipe16 SET ~70k (+~6% vs R36). Still does not beat
  single-thread poll — hop + key/val pack remain the tax.

### RocksDB path R36 — multi-conn RESP soak harness
- **`resp_client.py --clients C` / `BENCH_CLIENTS`:** concurrent connections;
  aggregate wall-clock OPS; keys namespaced `c{cid}:k{i}`.
- **`run_vs_redis.sh`:** prints `clients=` / `parallel=` / `shards=` in the header.
- Sample soak (pipe=1, vlen=100, `LSM_SYNC_EVERY=10000`):

  | clients | SET P0 | SET P1×4 | P1/P0 |
  |--------:|-------:|---------:|------:|
  | 1 | 18.7k | 12.6k | 67% |
  | 4 | 37.2k | 26.7k | 72% |
  | 8 | 37.3k | 33.4k | **90%** |

  pipe=16 × 4 clients: P0 SET ~110k, P1 ~66k (60%). Parallel closes the gap
  under concurrency but does **not** beat single-thread poll yet (network
  thread still serial for parse + ordered flush; hop cost remains).
- Artifact: `bench/rocks/results/multi-par-soak-latest.txt`.

### RocksDB path R35 — in-memory job mailbox
- **`db/workers.baga`:** job payload + reply are packed into `cell2` trees
  (8-byte words) and ride the i64 channels — **no `dir.jobs.*` disk hop**.
  Id allocation is an in-memory `Map` under mutex; replies stashed on the
  main thread (`p.reps`) after `try_done`/`wait`.
- **RESP poll:** when outstanding worker jobs exist, `poll_wait` timeout is
  **0** (spin drain) so single-client RTT is not capped at the idle 50 ms tick.
- Sample single-client pipe=1 n=3000 `LSM_PARALLEL=1 LSM_SHARDS=4`:
  SET/GET ~13k ops/s (was ~1k with job files; single-thread ~20k).
- Harness unchanged: `LSM_PARALLEL=1 LSM_SHARDS=4 ./bench/rocks/run_vs_redis.sh`

### RocksDB path R34 — RESP → per-shard workers
- **`LSM_PARALLEL=1`:** poll accept loop enqueues SET/GET/DEL/SETEX to R33
  workers; **ordered replies per fd** (seq maps + flush).
- PING/QUIT stay on the network thread; KEYS/DBSIZE return unsupported in
  parallel mode (use `LSM_PARALLEL=0`).
- Harness: `LSM_PARALLEL=1 LSM_SHARDS=4 ./bench/rocks/run_vs_redis.sh`

### RocksDB path R33 — per-shard parallel workers
- **`db/workers.baga`:** N `go_bg` workers, each exclusively owns one shard
  (`dir` / `dir.s{i}`). Job ids on channels; R35 packs payloads in-memory
  (i64-only chan constraint, same as queuebaga — without disk files).
- API: `lsm_parallel_start/submit/wait/set/get/del/stop` / `try_done` /
  `take_rep(p, id)`.
- Tests: `tests/workers_test.baga` (`LSMPATH` + `LSM_SHARDS` required).

### RocksDB path R32 — key-hash multi-shard cluster
- **`db/shard.baga`:** `LsmCluster` of N independent `LsmDB` (paths
  `<dir>.s0`…; **N=1 keeps `<dir>`** for compat). Hash routing for put/get/del;
  KEYS/DBSIZE merge across shards.
- **RESP serve:** always opens a cluster (`LSM_SHARDS`, default 1);
  `lsm_exec_c` on the poll path.
- **Honest limits:** still single-threaded event loop — shards partition the
  store for scale-out of working sets / future per-shard workers, not concurrent
  memtable writers.
- Tests: `tests/shard_test.baga`; `lsm_test` tcp durable green.

### RocksDB path R31 — scored compact pick + DBSIZE
- **`lsm_pick_scored`:** when `merge_pick>0`, pick the k largest SSTs (tie-break
  older index) then sort ascending for correct fold order. Equal sizes keep
  R8 oldest-N behaviour.
- **`lsm_dbsize` / `lsm_live_map`:** shared live-key fold; DBSIZE skips
  `sort_strs` (KEYS still sorts). RESP `DBSIZE` uses the fast path.
- Tests: `r31_*` in `lsm_test`.

### RocksDB path R30 — RESP pipeline readiness
- **Serve:** `TCP_NODELAY` on accept; 64 KiB read scratch (pipeline batches).
- **`resp_bulk_b`:** single prealloc + fill (no double concat).
- **Client harness:** `BENCH_PIPE=N` true pipeline (send batch, then read).
  Default pipe=64. pipe=1 keeps one-RTT mode.
- Sample vs Redis 8.x n=5000 `LSM_SYNC_EVERY=10000`:
  - pipe=1: PING/SET/GET ~58–64% of Redis (~25k ops/s)
  - pipe=64: absolute SET ~108k / GET ~114k ops/s (~33–36% of Redis pipeline)

### RocksDB path R29 — RESP parse / dispatch allocs
- **`resp_parse_command`:** `*N` / `$len` parsed as digits on the wire
  (`resp_parse_dec`) — no `str_of_bytes` for framing; only arg payloads
  become strings.
- **`resp_pong` / `resp_ok`:** fixed reply helpers.
- **`lsm_eq_ci` + dispatch:** case-insensitive command match without
  uppercasing every command; SET `EX` same path.
- **Serve buffer:** drop consumed command buffer when fully drained (no
  useless slice of empty residue).
- Harness: unique `LSMPATH` + ephemeral ports for stable soaks.

### RocksDB path R28 — RESP serve bench + sync env
- **`tools/serve.baga`:** long-running RESP entry for benches.
- **`LSM_SYNC_EVERY`:** WAL fsync cadence on serve (default 1 = durable).
- **`lsm_upper`:** O(n) bytes buffer (was O(n²) concat).
- **`bench/rocks/run_vs_redis.sh` + `resp_client.py`:** pure-socket RESP
  microbench (PING/SET/GET); Redis optional when `redis-server` is present.
  Sample (n=5000, `LSM_SYNC_EVERY=10000`): PING ~26k, SET ~11k, GET ~24k ops/s.

### RocksDB path R27 — block CRC on scan path
- **`SstMeta.bcrcs`:** load BAGASST5 per-restart-block crc32c table once.
- **`sst_block_crc_ok` / `sst_scan_load_block`:** verify each block before
  scan yield; fail closed on mismatch.
- **`sst_fold_into`:** v4/v5 never falls back to full `sst_load` after CRC fail.
- Get path stays unchecked (R11: per-get CRC of ~1 KiB blocks ~4× slower).
- Tests: `sst_scan_test` corrupt-byte case.

### RocksDB path R26 — manifest module + KEYS scan + sst_dump
- **`db/manifest.baga`:** `lsm_manifest_path` / `lsm_parse_manifest` /
  `lsm_load_manifest` / `lsm_write_manifest` (single owner for flat MANIFEST).
- **`lsm_keys`:** streams SST via `sst_fold_into` (block scan) instead of
  full-file `sst_load`.
- **`tools/sst_dump.baga`:** offline MANIFEST + per-gen SST summary (tag,
  restarts, first/last, put/tomb counts, bloom sidecar); optional key list.
- Tests: `tests/manifest_test.baga`.

### RocksDB path R25 — page cache scale + get short-circuit
- **Default page cache:** 2048 × 4 KiB ≈ **8 MiB** (was 256 pages / 1 MiB).
  Large-N random GET was thrashing (n=20k GET_RND ~27% of RocksDB → ~128%).
- **`lsm_get`:** skip `map_has` on empty mem/tomb (SST-only path after flush).
- **Serve defaults:** `flush_at=256`, `compact_at=4`; env `LSM_CACHE_PAGES`
  to override cache capacity.
- Multi-page `pc_read_at`: 8-byte unrolled copy tail.

### RocksDB path R24 — random GET (pc_read + restart)
- **`pc_read_at`:** single-page span fast path via `bytes_slice` (C memcpy)
  instead of baga per-byte `bytes_set`.
- **`block_restart_every`:** 8 (was 16) — smaller SST restart blocks; new
  writes only; older every-16 files still readable.
- Random GET n=2000: ~373k → ~552k ops/s. vs RocksDB n=1000 durable GET_RND
  ~96%; n=5000 batch GET_RND ~112% in this harness.

### RocksDB path R23 — GET block scan + hot span
- **`block_find`:** linear key scan without `str_of_bytes` on misses; materialize
  only on hit (`table/block.baga`).
- **LsmDB hot span:** last restart-block (gen/off/n/data) reused on sequential
  gets — no page re-copy; get-path only (compact/scan unchanged).
- **`lsm_get`:** defer `time_now_ms` until a live value needs TTL; `ttl_unpack`
  magic compare without allocating a string.
- GET profile n=2000: ~213k → ~681k seq / ~373k rnd ops/s. Head-to-head n=5000
  batch: GET_SEQ ~104%, GET_RND ~76% of RocksDB.

### RocksDB path R22 — WAL encode + CRC hot path
- **`crc32c`:** soft CRC for buffers under 4 KiB (table rebuild was slower than
  soft at WAL size); table path kept for large SST bodies. ~4× faster on
  120 B payloads.
- **`wal_record`:** single prealloc buffer (payload written at off=4, CRC over
  slice) — no separate payload alloc + copy.
- **`lsm_put_b`:** skip `map_del` on tomb when tomb map is empty.
- MEM put path n=5000: ~203k → ~546k ops/s (encode-bound).

### RocksDB path R21 — flush ROWS + batch flush_at
- **`sort_strs`:** quicksort + small-slice insertion (was O(n²) insertion over
  the full vec). Flush profile n=2000 had ~56% of `FLUSH_FORCE` in ROWS.
- **`lsm_flush` / merge:** sort `map_keys` in place; no mem-key copy into a
  second vec.
- **Batch bench:** `flush_at=N` + end `flush_force` (was 64 → many tiny SST
  fsyncs). `flush_at` sweep: 64→6.6k, 2000→67k ops/s.
- Probes: `./bench/rocks/run_profile.sh`, `./bench/rocks/run_profile.sh flush`.

### RocksDB path R20 — block scan + `table/block.baga`
- **`table/block.baga`:** shared record encode/decode (`block_rec_at` /
  `block_rec_put` / `block_rec_size`). `sst_build` writes records through
  the builder.
- **Block-level SST scan:** `sst_scan_begin` / `sst_scan_next` walk restart
  blocks via the page cache (one block resident; prior block dropped).
- **Compact:** `sst_fold_into` streams merge inputs — no full-file copy for
  v4/v5; legacy formats still use `sst_load`.
- Tests: `tests/sst_scan_test.baga`; `lsm_test` / `lsm_recover_test` green.

## [0.8.4] — 2026-08-06

**rocksbaga storage path R10–R19**, layered package architecture, engine
perf (get/put, multi-conn RESP, TTL, memory arc), plus post-0.8.0 product
and verify work carried in Unreleased.

### RocksDB path R19 — shared page cache + pin counts
- **`PageCache.fds` / `pins`:** multi-file buffer pool. Eviction writeback
  uses the registered fd for each page's `file_id` (fixes wrong-fd writeback
  when the cache holds dirty pages from more than one SST). Pin count > 0
  blocks clock eviction; all-pinned + full → `rc = -2`.
- **API:** `pc_register_file` / `pc_unregister_file`, `pc_pin` / `pc_unpin`,
  `pc_get_pin`. `pc_read_at` pins each source page while copying.
- **Wiring:** `sst_fd_get`/`drop` and `sst_read_raw` register temporary and
  cached SST fds; compact invalidate + `lsm_close` unregister.
- Tests: `tests/page_cache_test.baga`; `lsm_test` / `lsm_recover_test` green.

### rocksbaga — layered package architecture
- Split flat folder into **`util/`**, **`cache/`**, **`wal/`**, **`table/`**,
  **`db/`**, **`net/`**, **`examples/`**, **`docs/`** with root re-export
  shims (`engine.baga` → `db/engine.baga`, …). Documented in
  `app-product/rocksbaga/ARCHITECTURE.md`. Prefer
  `import "rocksbaga/db/engine.baga"`; short paths still work.
- Further split: **`table/bloom.baga`**, **`db/types.baga`**,
  **`db/compact.baga`** (engine orchestrates flush/recovery only).
- **Architecture doc** updated for the split dependency graph
  (`db/compact` → `table/{sstable,bloom}`).

### rocksbaga — vs RocksDB engine microbench
- **`bench/rocks/`:** head-to-head harness — pure `lsm_put`/`lsm_get`
  (`engine_bench.baga`) vs RocksDB via `rocksdict` (`rocksdb_bench.py`).
  Modes: `durable` (fsync each put) and `batch`. Run:
  `./bench/rocks/run_vs_rocksdb.sh`. Baseline snapshot under
  `bench/rocks/results/`.

### RocksDB path R11 — get-path performance
- **`pc_read_at`:** preallocate + `bytes_set` (was O(n²) `bytes_push`).
- **BloomCache** on `LsmDB`: load `.bloom.<gen>` once; drop on compact.
- **SST fd cache** (`sst_fds`/`sst_sizes`): reuse open descriptors on get;
  closed on compact / `lsm_close`.
- Partial get: skip embedded bloom when sidecar said maybe; skip block CRC
  on get (compact/full-parse still verify body CRC).
- Default page cache cap **256** pages (was 32).
- Bench n=1000 durable: GET ~538 → ~77k–90k ops/s; PUT ~50%→~70% of RocksDB.

### RocksDB path R12 — SST meta cache
- **`SstMeta`** (footer, restart offsets, first/last key) cached per gen on
  `LsmDB.sst_meta`; invalidated on compact. Partial get uses cached range
  + index (no footer/last-block re-read).
- Bench n=1000 durable: GET ~167k ops/s (~31% of RocksDB).

### RocksDB path R13 — restart-key cache
- **`SstMeta.rkeys`:** load every restart key once with the meta; get bsearch
  is pure memory, then a single block read. Bench n=1000 durable: GET_SEQ
  ~200k ops/s (~37% RocksDB), GET_RND ~250k (~48%).

### RocksDB path R14 — put-path encode + WAL buffer
- Faster binary builders: `push_u32_le` one-concat; preallocated WAL record,
  bloom sidecar, SST file layout; single bloom build for SST+sidecar.
- **WAL write buffer** (`LsmDB.wal_buf`, max 64 KiB) — coalesces pwrites until
  sync / buffer full / memtable flush / close.
- Bench n=1000 durable PUT ~695 ops/s (~82% RocksDB).

### RocksDB path R15 — poll multi-conn RESP
- **`lsm_serve`** uses `std/net/poll` event loop: many TCP clients, one
  `LsmDB`, per-fd read buffers. Closes the serial-accept limit (kvbaga K1
  path). `LSM_SERIAL=1` restores one-at-a-time accept. Tests: `tcp2_*`.

### RocksDB path R10 — package rename rocksbaga
- **`app-product/rocksbaga/`:** former `lsmbaga` package (R0–R9 engine
  unchanged). Prefer `import "rocksbaga/…"`. Deprecated **`lsmbaga/`** shims
  re-export modules for old imports. Symbols stay `lsm_*` / env `LSM_*`.
  Not RocksDB feature parity — see `rocksbaga/gaps.md`.

### RocksDB path R9 — standalone bloom sidecar
- **rocksbaga (was lsmbaga):** each new SST also writes `<dir>.bloom.<gen>` (`BAGABLM1` +
  bit array + crc). `sst_get` consults the sidecar first and skips SST open
  on definite miss. Compact unlinks bloom with SST. Embedded bloom stays in
  BAGASST5. Tests: `r9_*` in `tests/lsm_test.baga`.

### RocksDB path R8 — oldest-N compact pick
- **rocksbaga:** `LsmDB.merge_pick` (default 0 = merge all files when a level
  is over). When `>0`, compact merges the **oldest N** SSTs only (min 2);
  with byte targets the pick grows until size coverage. Env `LSM_MERGE_PICK`.
  Tests: `r8_*` in `tests/lsm_test.baga`.

### Advanced plan DoD met
- **Plan close-out:** `docs/superpowers/plans/2026-08-05-advanced-go-rust.md`
  status **plan DoD met**; evidence table in §7; residual horizon §9/§11.
- **Write-up:** `docs/superpowers/plans/2026-08-05-advanced-plan-dod.md`
  (criteria checklist, explicit non-claims, post-plan workstreams, smoke list).

### Phase 2 B1 — ormbaga + jsonrpc L3 results
- **ormbaga:** `OrmExec` / `OrmQuery` / `OrmCount` / `MigrateResult` are L3
  enums (`OrmEOk`/`OrmEErr`, `OrmQOk`/`OrmQErr`, …); helpers `orm_ok`,
  `orm_db_q`, `migrate_is_ok`, … Apps/api + registry + oauth use helpers
  (no `ok:i64` stand-in fields).
- **jsonrpcbaga:** `RpcResult` → `JrpcOk` / `JrpcErr` / `JrpcSkip`.
- Tests: `orm_test`, `api_test`, `jsonrpc_test` green (with migrated DB).

### RocksDB path R7 — byte-size targets + L3
- **lsmbaga:** `LsmDB.target_bytes` (default 0 = file-count only). When set,
  levels compact if file-count ≥ `compact_at` **or** total SST bytes ≥
  level target (L0=T, L1=4T, L2=16T, L3=64T). **L3** promote from L2.
  Helpers `lsm_level_bytes` / `lsm_level_target`. Serve env `LSM_TARGET_BYTES`.
  Tests: `r7_*` in `tests/lsm_test.baga`. Not renamed to rocksbaga yet.

### Phase 5 — io_uring poll backend sketch
- **`tools/iouring/`:** raw x86-64 Linux probe (no liburing):
  `io_uring_setup` detect, NOP CQE, `IORING_OP_POLL_ADD` on a self-pipe.
  Design note maps future `poll_wait_iouring` to today’s `PollResult`;
  production stays `std/net/poll.baga` (`SYS_poll`). Smoke:
  `./tools/iouring/test_sketch.sh`. Baga gap: 3-arg `syscall` + no ring mmap.

### Phase 5 — structural liveness + design notes
- **`examples/verify/liveness_struct.baga`:** `--verify` proves fixed-N
  unanimous 2PC ⇒ commit and matched fan-in ⇒ balanced (counting progress,
  not full temporal liveness). Wired into `scripts/run_verify.sh`.
- **Design notes:** C′ borrow-lite
  (`docs/superpowers/specs/2026-08-05-borrow-lite-design.md`); A4 LLVM L3
  status still C-only (`…/2026-08-05-llvm-l3-status.md`).

### Phase 5 — protoc_baga sketch
- **`tools/protoc_baga/`:** proto3 subset → baga `Msg_encode`/`Msg_decode`
  (string/int64/bytes/bool). Design note + `examples/hello.proto` /
  `registry.proto`. Smoke: `./tools/protoc_baga/test_sketch.sh` (hex goldens
  + baga compile of generated Hello helpers).

### Phase 3 exit — metrics + graceful shutdown
- **fmrbaga:** `fmr_run` uses `poll_wait` + `signal_watch(SIGTERM/SIGINT)`;
  stops accepting on signal; `fmr_shutting_down()` for readiness drain.
- **apps/api + registry:** `GET /metrics` (metbaga), `/ready`/`/readyz` → 503
  while shutting down.
- **Runbook:** `docs/runbooks/product-path.md` (API + registry gRPC + probes).

### Phase 4 B4.4 — latency bench gate
- Recorded `./bench/run_latency.sh` on Ryzen 5 3600 / baga 0.7.0:
  **p50 ≈ 7 ms**, **p99 ≈ 8 ms** per 8e6-iter batch (40 batches).
  Artifacts: `bench/results/latency-2026-08-05.md`, `bench/results/latency-latest.txt`.

### Phase 4 B4.3 — multi-key 2PC concurrent stress
- **txnbaga:** participants hold **multiple concurrent PREPARE**s when locks
  do not conflict; `tpc_txn_id` for disjoint tx ranges. Stress test
  `tests/txn_stress_test.baga` (3 go_bg workers, private multi-key + hot-key
  contention).

### Phase 4 B4.1 — lsm recovery + page-cache stress
- **`tests/lsm_recover_test.baga`:** WAL/SST/tomb reopen (crash-style close),
  double reopen, many-key compact recovery, page cache `cap=2` scan still
  correct. README recovery story (`sync_every=1` fdatasync).

### Phase 4 B4.2 — raft durable log lite
- **raftbaga:** `persist.baga` saves term/vote/commit/log to
  `/tmp/baga_raft_<id>.state`; nodes load + re-apply on start; flush on
  durable changes. `raft_persist_test` covers encode/load/apply and live
  disk recovery after PUT/stop.

### Phase 3 B3.4 — gRPC interop goldens
- **`tests/grpc_goldens_test.baga`:** fixed hex vectors matching protoc wire
  (HelloRequest/Reply, gRPC length-prefix frames, registry GetPackage/Package
  shapes) + google.rpc.Code / HTTP map for 0, 3, 5, 14, 16.

### Phase 3 B3.3 — registry dual protocol (HTTP + gRPC)
- **fmrbaga:** `fmr_is_grpc_request` + `fmr_grpc_handle` hook — same port as JSON
  when `Content-Type: application/grpc` and `/Service/Method` path.
- **apps/registry:** `regbaga.Registry` RPCs `GetPackage` / `ListPackages`
  (hand PB + statusbaga codes); test `tests/registry_grpc_test.baga`.

### Phase 3 B2.4 — OpenAPI from route table
- **fmrbaga:** `fmr_openapi_from_router` emits `paths` from the live `Router`
  (path params, bearer/public heuristics, body/response schema names,
  `x-baga-route-id`). `/openapi.json` uses the app's registered routes.
  `fmr_openapi_doc` keeps a catalog router for pure tests.

### Phase 3 B2.1 — fmr middleware (request-id + otel + log)
- **fmrbaga:** ordered pipeline in `fmr_handle`: request-id → W3C
  `traceparent` (otelbaga child span) → `fmr_before` → dispatch → logbaga
  JSON line (`FMR_LOG=1`) → response headers (`X-Request-Id`, `traceparent`,
  CORS). `FmrCtx` carries `req_id` / `trace_id` / `span_id`.
- **apps/api** + **registry** mains gain `!Time` for the log path.

### RocksDB path R6 — per-block CRC + L2
- **lsmbaga:** new SSTs write **`BAGASST5`**: per-restart-block **crc32c** after
  the restart index; partial get verifies the loaded block. **L2** level:
  L1 ≥ `compact_at` → merge to L2; collapse ≥2 L2 files. v1–v4 readable.
- Not renamed to rocksbaga yet (more polish still open; R7 added targets+L3).

### RocksDB path R5 — partial SST get + L0/L1
- **lsmbaga:** new SSTs write **`BAGASST4`** (core + bloom + fixed footer).
  Get: `fd_size` → footer → bloom pages → restart index → one data block via
  page cache (no full-file materialize on miss). **v1–v3** still readable.
- **L0/L1:** MANIFEST lines `gen level`; flush → L0; L0 ≥ `compact_at` →
  merge to L1; collapse multiple L1. Pure tombs kept on partial merges.
- **std/os:** `lseek` + `fd_size` for SST footer addressing.

### RocksDB path R4 — bloom filter + chain compact
- **lsmbaga:** new SSTs write **`BAGASST3`**: restart index + **bloom filter**
  (~10 bits/key, 4 double-hash probes). Get: CRC → bloom may-contain →
  restart bsearch → block scan. **BAGASST1/2** still readable.
- Compaction **chains** oldest-N merges while `gens >= compact_at`.

### RocksDB path R3 — binary values + better compaction
- **lsmbaga:** memtable `Map<str, bytes>`; WAL/SST values as bytes; `lsm_put_b`
  for NUL-safe puts; `lsm_get` returns `bytes`. Compaction merges the **oldest**
  `compact_at` gens (keeps younger SSTs) and **drops pure tombstones**.
- **kvbaga:** `resp_bulk_b` for binary-safe RESP bulk replies.

### RocksDB path R2 — SST restart index
- **lsmbaga:** new SSTs write **`BAGASST2`**: sorted records + restart index
  (every 16 keys) + crc. `sst_get` uses restart **bsearch** + one-block scan
  (no full row materialize). **`BAGASST1`** still readable. Compaction/KEYS
  still full-parse. Full file IO remains (page-sized blocks = later).

### Positioning + RocksDB path (R1)
- **README / BASE:** Baga is an **educational systems language**; packages are
  ecosystem blocks to **prove the language**, not demos. End goal: **RocksDB-class**
  embedded KV (`lsmbaga` road).
- **lsmbaga R1:** SST lookup uses **binary search** + first/last key filter
  (still full-file parse; block index landed as R2).

### Stabilize language + applications (focus)
- Advanced plan **Phase Stabilize**: pause further Result migrations
  (orm/jsonrpc L3 deferred); keep `main` green for ship.
- B1 landed for **pbbaga** + **pgbaga**; ormbaga keeps stand-in fields +
  accessor helpers (`orm_ok_q`, `orm_db_q`, …).
- Optional light borrow (C′) remains non-mandatory direction only.
- Full `scripts/run_tests.sh` green: registry `pg_conn_of` after L3;
  force `PORT=8090` for registry (no clash with `PORT=8080`);
  jsonrpc live HTTP drains until body; apps/api runbook expanded.

### Plan: optional light borrow checker (direction only)
- `docs/superpowers/plans/2026-08-05-memory-management.md` §7 — light
  **opt-in** borrow later; **not mandatory**, default sharing unchanged.
- Advanced plan Track **C′** mirrors this; does not block product B1–B3.

### B1 pgbaga — PgResult as L3 sum
- `PgResult` → `PgOk(PgRows) | PgErr(PgFail)`; rebind with `pg_conn_of(r)`.
- Accessors sum-aware; ormbaga `orm_*_from` updated. `pg_test` green.

### B1 pbbaga — L3 decode (no ok:i64 stand-ins)
- `GrpcMsg`: `GFrame` / `GBad`; stream: `StreamOk` / `StreamEnd`
- Hello: `HelloOk(HelloBody)` / `HelloBad`; reply: `ReplyOk(str)` / `ReplyBad`
- Unary/client/demo + `pb_test` / grpc_* tests updated.

### A2 Vec/Map of sum enums
- `Vec<Res>` / `Map<K, Res>` allowed (same box path as L4 structs):
  push/get/set/slice/concat and map_set/get; missing map key → zero tag.
- Checker + C codegen; test `tests/std/sum_vec_test.baga`; probe in
  `run_tests.sh`. Docs §11.1 updated.

### A1 qualified sum variants (`Enum::Ok`)
- Syntax: `PgRes::Ok(1)`, match `PgRes::Ok(v)` / bare `Ok(v)` when unique or
  scoped to match scrutinee. Token `::`, AST `NODE_PATH`.
- Cross-enum shared names allowed; bare ambiguous → `нееднозначен`.
- Tests: `tests/std/sum_qualify_test.baga`; probes in `run_tests.sh`.
- Docs: language §11.1 (en/bg); design marked implemented.

### Advanced plan (Go/Rust) + A5 FNS_MAX hard error
- Plan: `docs/superpowers/plans/2026-08-05-advanced-go-rust.md` — Track A
  language unlock, B product migrations, C verify/MEM differentiator.
- A1 design: `docs/superpowers/specs/2026-08-05-sum-variant-qualify-design.md`.
- **A5:** exceeding `FNS_MAX` (1024) for fns/structs/enums/variants is a
  **compile error** (no more silent truncation of symbols).

### L3 sum enums as struct fields + real gRPC client
- **Language (C backend):** structs and sum enums emit in **topological
  order**, so `struct Hold { r: Res }` and nested `enum Box { BoxHas(Wrap) }`
  compile. Docs §11.1 updated (en/bg).
- **pbbaga `grpc_client`:** unary client over HTTP/1.1 binary body —
  `grpc_call_unary` returns L3 `CallOk(GrpcCallOk) | CallErr(Status)`;
  `GrpcClient.last` stores the sum on a field; wires **statusbaga** +
  **mdtbaga** + `grpc-timeout`. Live test: `tests/grpc_client_test.baga`.
- **sumtype_test:** field + nested sum/struct cases.

### gRPC-shaped universal packages (status / metadata / context)
- **statusbaga**: gRPC codes 0–16 (`GRPC_OK`…`GRPC_UNAUTHENTICATED`),
  `Status`, HTTP mapping, trailer helpers, `grpc-timeout` encode/decode.
- **mdtbaga**: metadata multimap (lowercase keys, multi-value, reserved
  header filter) — Go `metadata.MD`.
- **ctxbaga**: deadline / cancel / values + `ctx_from_grpc_timeout`
  (depends on statusbaga).
- **pbbaga**: unary glue uses status codes/messages; `grpc_unary_with_status`,
  `grpc_response_with_md`.
- Tests: `status_test`, `mdt_test`, `ctx_test` + existing grpc unary/bidi.

### Universal foundation packages + file_exists
- **pathbaga**: POSIX-ish path helpers — `path_join`, `path_basename` /
  `dirname`, `path_ext` / `path_stem`, `path_is_abs` (pure).
- **globbaga**: shell-style `*` / `?` match + `glob_filter` for KEYS/routing.
- **uuidbaga**: UUID v4 (RFC 4122) + `uuid_ok` validator (`!Random`).
- **bufbaga**: `StrBuf` string builder (`buf_push` / `buf_str`) — closes
  md/template quadratic-concat gaps (M1).
- **querybaga**: URL query / form parse+encode with `+`→space decode
  (`query_parse` / `query_encode` / `query_from_path`) — G7-style gap.
- **std/os**: `access(2)` extern + `file_exists` / `file_readable` helpers.
- Tests: `path_test`, `glob_test`, `uuid_test`, `buf_test`, `query_test`,
  `os_fs_test` (exists/readable).

### gRPC bidi algebra + OTLP mock-collector integration
- **pbbaga**: `hello_bidi` / `hello_bidi_requests`, `grpc_hello_stream_response`
  (server-stream when unary n>1, else bidi). Live H1 test:
  `tests/grpc_bidi_test.baga`.
- **otelbaga**: end-to-end `otel_export_http` against in-process mock
  collector (`tests/otel_http_test.baga` → POST `/v1/traces`, 200).

### OTLP/JSON export, gRPC streaming frames, MEM-3 mut rebind
- **otelbaga**: `OtelSpan`, `otel_span_to_otlp_json` (base64 ids),
  `otel_export_file` / `otel_export_http` / `otel_export_span_file`.
- **pbbaga**: `grpc_stream_append` / `grpc_stream_next` / `grpc_stream_count`,
  `hello_stream_replies` (server-streaming message layer).
- **MEM-3**: `mut p = arena_alloc(...)` rebinds region; free of old arena
  no longer kills rebound `p` (probes in `run_tests.sh`).

### C8 lite otelbaga + MEM-3 region tags + cloudbaga gRPC
- **otelbaga**: W3C `traceparent` parse/format/new/child/from_header;
  `log_info_trace` / `log_emit_trace` for correlation. No OTLP export.
- **MEM-3 region**: `let p = arena_alloc(a, n)` tags `p` with arena `a`;
  `arena_free(a)` invalidates all such locals (compile error on use).
- **cloudbaga**: POST gRPC method paths → `grpc_hello_response`; logs
  include `trace_id`/`span_id`; echoes `traceparent` on `/hello`.

### HTTP: binary Response body + H2 trailers (gRPC-native)
- `Response` gains `body_bytes`, `trail_ks`/`trail_vs`; helpers
  `http_response_bytes`, `http_set_trailer`, `http_body_len` /
  `http_body_as_bytes`.
- `http_respond_keepalive` and `h2_respond` prefer `body_bytes` (NUL-safe);
  H2 emits trailer HEADERS with `END_STREAM` when trailers are set (gRPC
  `grpc-status`).
- `pbbaga/grpc_unary`: `grpc_hello_response` / `grpc_to_response` for H2.

### MEM-3 arena seatbelt + gRPC unary glue + latency bench
- **MEM-3 (lite):** checker tracks `arena_free` like `drop` on the handle —
  double `arena_free`, `arena_alloc`/`reset` after free, and use of a freed
  handle are compile errors (reuses drop_log join). Runtime null-guards on
  alloc/reset. Full region tagging of arena payloads not claimed. Probes in
  `scripts/run_tests.sh`.
- **gRPC unary glue** (`pbbaga/grpc_unary.baga`): `grpc_hello_handle` +
  `grpc_write_response` (headers as str, body via `tcp_write_bytes` —
  frames start with 0x00 so they cannot live in `Response.body` str).
  Live test: `tests/grpc_unary_test.baga`.
- **Latency bench:** `bench/latency.baga` + `bench/run_latency.sh` —
  batch min/avg/max/p50/p99 via `monotonic_ms`.

### Storage — S8 txnbaga: 2PC coordinator + MVCC
- New package `app-product/txnbaga`: in-process **two-phase commit**
  (2 participants, channels) + **MVCC** i64 store (snapshot reads by
  `read_ts`, versioned commits).
- PREPARE locks + write-set; all-YES → COMMIT publishes versions; any NO /
  timeout → ABORT. Return-updated `tpc_put_ex` / `tpc_txn_ex` (cluster is
  by-value).
- Pure rules under `--verify`: `tpc_decide`, `mvcc_visible`, `lock_conflict`,
  `next_ts` (`examples/verify/tpc_decide.baga`).
- Tests: `tests/txn_test.baga` (MVCC snaps + 2PC multi-version + two-key).
  Spec/notes: `docs/superpowers/specs/2026-08-05-txnbaga-design.md`.
- Closes Track S sequencing step S8 (distributed transactions probe).

### Cloud — C6 relbaga + C7 flagbaga
- **relbaga** (C6): exponential backoff (`rel_backoff_ms`), `rel_retry` over
  `fn(i64)->i64`, circuit breaker (`brk_*` closed/open/half-open), bulkhead
  via channel tokens (`bh_new`/`acquire`/`try`/`release`). Tests:
  `tests/rel_test.baga`.
- **flagbaga** (C7): `--name value` / `--name=value` / bare bool flags,
  positionals; `flags_str`/`i64`/`bool`/`has`; `flags_parse` (process) +
  `flags_parse_vec` (tests). Tests: `tests/flag_test.baga`.

### Cloud — C5 pbbaga: protobuf wire + gRPC framing
- New package `app-product/pbbaga`: Protocol Buffers wire codec (varint,
  fixed32/64, length-delimited strings/bytes), zigzag `sint64`, skip
  unknown fields, hand-built field helpers (no protoc).
- gRPC length-prefixed messages: `[0][BE32 len][payload]` via
  `grpc_encode` / `grpc_decode`; example `HelloRequest`/`HelloReply` +
  pure `hello_rpc`. Full H2 transport remains host glue on httpdbaga.
- Tests: `tests/pb_test.baga` (varint 300 golden, cyrillic, skip,
  fixed, zigzag, neg int64, frame, protoc string golden `testing`).
- Spec: `docs/superpowers/specs/2026-08-05-pbbaga-design.md`.

### Cloud — C1–C4: signals, metrics, logs, cloudbaga demo
- **C1 signals** (builtins): `signal_watch` / `signal_check` / `signal_clear`
  / `signal_wait` / `signal_raise` — process-global slot for graceful
  shutdown (SIGTERM/SIGINT). C backend + `libbaga_par.so` (LLVM). Tests:
  `tests/std/signal_test.baga`.
- **C2 metbaga**: Prometheus text exposition — counters, gauges,
  `met_render` for `GET /metrics`.
- **C3 logbaga**: JSON lines on stderr (`ts`, `level`, `msg`, optional
  `req_id`) via `std/json` escape.
- **C4 cloudbaga**: 12-factor demo service — `/healthz`, `/readyz`,
  `/metrics`, `/`; poll accept loop observes signals and flips readiness.
- Docs: language §19 builtins; cloud direction plan C1–C4 marked shipped.

### Consensus — raftbaga (S7): leader election + log replication
- New package `app-product/raftbaga`: **3-node in-process Raft** over CSP
  (`go` / channels / `chan_recv_timeout`). No shared mutable state between
  nodes; messages are nested `cell2` trees (M17 packing).
- Election (staggered timeouts), heartbeats, single-entry AppendEntries,
  majority commit, apply to per-node `Map<i64,i64>`.
- Client: `raft_start` / `raft_put` / `raft_get` / `raft_stop` — tries nodes
  one-by-one so only the leader appends.
- Pure decision rules in `rules.baga` with specs; `--verify` on
  `examples/verify/raft_term.baga` proves term adoption, log up-to-date,
  majority (and reports honest UNKNOWN on thinner fragments). **Full Raft
  safety is not claimed.**
- Tests: `tests/raft_test.baga` (rules + live cluster put/get/multi).
  Demo: `app-product/raftbaga/demo.baga`.
- Spec/plan: `docs/superpowers/specs/2026-08-05-raftbaga-design.md`,
  `docs/superpowers/plans/2026-08-05-raftbaga.md`.

### Storage — lsmbaga MVP (S5+S6): page cache + WAL → memtable → SSTable
- New package `app-product/lsmbaga`: durable LSM-style KV on **RESP2**
  (reuses `kvbaga/resp.baga` so redis-cli works for the supported set).
- **S5 page cache** (`page.baga`): fixed 4 KiB pages, clock eviction,
  dirty writeback, invalidate-on-unlink; composite key
  `file_id * 1e9 + page_no`.
- **S6 engine** (`engine.baga` / `wal.baga` / `sstable.baga`):
  crc32c WAL records → memtable; flush to sorted `BAGASST1` SSTables +
  MANIFEST; compaction-lite when `gens >= compact_at`; recovery =
  MANIFEST + SST gens + WAL replay. Path **prefix** layout
  (`<dir>.wal`, `<dir>.sst.<gen>`, `<dir>.manifest`).
- RESP: `PING SET GET DEL EXISTS INCR KEYS DBSIZE SAVE QUIT`; honest
  ERR for TTL/EXPIRE/SET EX. Env: `LSMPATH`, `LSM_FLUSH_AT`,
  `LSM_COMPACT_AT`.
- **std/os** helpers for the engine: `mkdir`/`unlink`/`link`,
  `fs_rename` (link+unlink — raw `rename` collides with `stdio.h`),
  binary-safe `fd_write_bytes` / `fd_pwrite_bytes` / `fd_pread_bytes`
  (tcp idiom: explicit length, embedded NUL ok).
- Tests: `tests/std/os_fs_test.baga` (incl. mid-buffer `0x00`),
  `tests/lsm_test.baga` (flush, tombstone, reopen recovery, compact,
  RESP in-process + TCP loopback durability). Demo:
  `app-product/lsmbaga/demo.baga`.
- Spec/plan: `docs/superpowers/specs/2026-08-05-lsmbaga-design.md`,
  `docs/superpowers/plans/2026-08-05-lsmbaga.md`.

### Language — drop + checker-enforced memory discipline (MEM-1/2)
- `drop(x)` frees a let-bound local's heap blocks **now**: deep free for
  `Vec` (element boxes for bytes/struct elems, data buffer, struct), `Map`
  (pv boxes for boxed struct values, entries, buckets, struct), `bytes`
  (data buffer), and `fn`
  (the malloc'd `(code, env)` cell handle — the closure env box stays in
  the arena, shared ownership, documented).
- Checker seatbelt (all compile errors): use after drop
  (`използване на 'x' след drop`), double drop, drop of a parameter,
  drop of a lambda-captured variable (inside or outside the lambda),
  drop inside a loop of a variable declared outside it, drop of
  `str`/scalars, drop of a non-local expression. Branch-join semantics are
  certainties only: a variable is definitely-dropped after an `if` only
  when dropped on ALL arms; maybe-dropped use after the join is allowed.
- Runtime: 16 B-granularity size-classed free list for blocks ≤ 1024 B in
  `baga_alloc` (padded allocations, serialized by the existing pthread
  mutex — `go`-safe). Reclaim proof: a 1M-iteration alloc+drop loop peaks
  at ~6.2 MB maxrss vs ~87.6 MB without `drop`.
- MEM-2 (`--verify`): `HK_DROP` ghost state keyed by source variable,
  registered at `let x = vec_new()/map_new()/bytes_new(...)`; use-after-drop
  or double drop on a live path is **ОБРОЧЕНО (REFUTED)** with a witness
  (`examples/verify/mem_drop.baga`); aliasing and fn-value drop are silent
  no-claim paths; fragment gating as M14.
- Honesty boundary: assignment revival stays an error (`drop(v); v =
  vec_new(); use(v)` — conservative v1); aliasing is the programmer's
  contract (`let y = x; drop(x); use(y)` NOT diagnosed — the checker
  tracks variables, not heap graphs); blocks > 1024 B not reclaimed;
  historical `vec_grow`/`map_rehash` garbage stays; scope-exit leaks are
  NOT diagnosed (no warning severity — MEM-3 territory); bytes/str inner
  buffers of freed boxes stay in the arena; LLVM backend honestly
  `unsupported`.
- Tests: `tests/std/drop_test.baga` (17 checks) + reclaim probe, 10
  probes (8 negative + 2 positive-join) in `scripts/run_tests.sh`,
  `examples/verify/mem_drop.baga`. Spec:
  `docs/superpowers/specs/2026-08-05-mem-drop-design.md`; docs §12.8.

### Storage foundation (S2–S4): bytes mutators, positioned IO, crc32c
- **S2 — `bytes` mutators** (httpdbaga **G9**): `bytes_new(n)` returns a
  fresh zeroed buffer (`n < 0` clamps to 0); `bytes_set(b, i, v)` is a
  bounds-checked write (`baga: bytes_set: индекс N извън границите [0, L)`,
  `v` masked to a byte) that **mutates the shared buffer** — aliases see
  the write (Vec/Map semantics); `bytes_push(b, v)` returns a **new**
  `bytes` of length `len+1` (the source is untouched, O(n) copy per push —
  fine for frame building). C backend only; the LLVM backend honestly
  reports `unsupported`.
- **S3 — positioned IO** (`std/os`): new externs `pread`/`pwrite`/
  `fsync`/`fdatasync`/`fallocate` (!IO, i64/str params) plus wrappers
  `fd_pwrite(fd, data, off) -> 0/-1` (partial-write loop) and
  `fd_pread(fd, n, off) -> str` (heap buffer, `""` at EOF/error) — neither
  moves the fd's file position. `std/net/tcp.baga` now externs
  `pread64`/`pwrite64` (glibc weak aliases of the same symbols): two
  same-named body-less externs can't coexist — both prototypes are emitted
  unmangled and gcc fails with "conflicting types".
- **S4 — CRC-32C** (`std/crypto/crc32c.baga`): Castagnoli (reflected poly
  0x82F63B78) over native `bytes`, masked-i64 u32 — `crc32c_update` /
  `crc32c_final` / `crc32c_b`, incremental chaining; validated against the
  published iSCSI vector set.
- Tests: `tests/std/bytes_mut_test.baga` (8 checks), `tests/std/os_io_test.baga`
  (11 checks), `tests/std/crc32c_test.baga` (10 checks) + an S2 OOB negative
  probe (`bytes_set` out of bounds). Spec: `docs/superpowers/specs/2026-08-05-storage-foundation-design.md`;
  parent plan `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md`
  (Track S step 1).

### Language — sum types (L3): payload enums + full match
- Enums can carry a payload per variant — real sum types:
  `enum Res { Ok(i64), Err(str) }`, constructed as `Ok(42)`; payload-less
  variants stay bare (`None`). The type is nominal (`TYPE_ENUM`, not
  `i64`) — a `Res` no longer passes where an `i64` is expected.
- Checker: variant names of sum enums are globally unique across the
  program and may not collide with function names (`повторена дефиниция на
  вариант`); constructor arity and payload type are checked; `match` on a
  sum enum takes `Variant(binding)` / `Variant` / `_` patterns, must be
  **exhaustive** (the error names the missing variant), and all arms must
  agree in type. Non-enum matches keep first-arm-wins with no
  exhaustiveness.
- C backend: tagged struct + payload `union` + a `static inline`
  constructor per variant. LLVM backend: honest `unsupported` pointing at
  docs §11.
- Honest v1 limits: no `Vec<sum enum>` / `Map<K, sum enum>` (existing
  `неподдържан елементен тип` error), no generics (write a concrete enum
  per use site), exactly one payload type per variant (use a struct for
  more), sum enums can't be struct fields yet (C typedef order), and an
  enum payload must be declared before the enum that uses it.
- Bug fixed alongside: bare-expression match arms in `-> void` functions
  were wrongly checked against the enclosing fn's return type.
- Unblocked gaps: jsonrpcbaga **R1**, tplbaga **P2**, bagadecimal **D4**,
  oauthbaga **O4** (migrations of the stand-in structs optional).
- Tests: `tests/std/sumtype_test.baga` (15 checks) + 8 negative probes in
  `scripts/run_tests.sh`. Spec:
  `docs/superpowers/specs/2026-08-05-l3-sum-types-design.md`.

### Language — function values & closures (L5)
- Functions are first-class values, typed `fn(T, ...) -> R` (effects
  allowed: `fn(i64) -> i64 !IO`), usable in locals, parameters, return
  types, and as `Vec`/`Map` elements — method tables work
  (`Map<str, fn(str)->str>`).
- **Named references** (`let f = add`) and **lambdas with explicit
  by-value captures** (`fn [a, b] (x: i64) -> i64 { ... }`; later
  mutation of the source doesn't propagate, `Vec`/`Map` captures share
  the reference). Closures can be returned from functions (env box lives
  in the arena).
- C backend: a fn value is an i64 `(code, env)` handle over the par
  runtime's `cell2`; every user fn gets a `__clo` wrapper; lambdas
  compile to a synthetic env struct + wrapper, emitted before the fn
  bodies via memstreams (no AST pre-pass). Calls go through a statically
  typed function-pointer cast.
- Checker: structural `type_eq` for fn types; effect contract at wrap
  time (the value's effects must fit the annotation); **fn-typed locals
  may not shadow global functions** (keeps `--verify` sound — calls
  through values are opaque to it, honest skip); clear errors for
  calling non-functions and for ambiguous fn references (L6 rule:
  caller's own module wins).
- LLVM backend: honest `unsupported` for fn values.
- Unblocked gaps: jsonrpcbaga **R2**, tplbaga **P1**, testbaga **T3**
  (migrations optional; the probe tests live in
  `tests/std/fnval_test.baga`, 18 checks + 4 negative probes).
- Docs: §12.6 in both languages; spec in
  `docs/superpowers/specs/2026-08-05-l5-closures-design.md`.

### TLS 1.3 — TLS_AES_256_GCM_SHA384 (0x1302) negotiated end to end
- New `std/crypto/sha512.baga`: SHA-384/SHA-512 (FIPS 180-4) with 64-bit
  words as hi/lo 32-bit halves — no intermediate exceeds 2^33, the same
  signed-i64 discipline as the rest of std/crypto. FIPS vectors incl.
  the 56-byte and 1,000,000-'a' cases.
- `hmac_sha384_b` (block 128, 48-byte MAC) and `hkdf384_extract/expand`
  (RFC 5869; empty-salt → 48 zeros rule). Vectors from python
  hashlib/hmac computed offline (`tests/std/sha512_test.baga`, 13
  checks).
- `std/net/tls.baga` is suite-parameterized: `TlsSchedule.cipher`
  selects the transcript hash, HKDF/HMAC flavor, HashLen (32/48) and key
  length (16/32) everywhere — schedule, flight decrypt, CertificateVerify
  input, Finished HMACs, application secrets. The gate accepts both
  suites; the ClientHello already offered both.
- Live proof: `scripts/run_tests.sh` runs the openssl peer a **third**
  time with `-ciphersuites TLS_AES_256_GCM_SHA384`; the handshake test
  asserts the negotiated suite via `TLSCIPHER` env.
- **L6 correction found by this work**: sha512.baga's `u32` collided
  with sha256.baga's and every *internal* call turned ambiguous. The
  unqualified resolution now prefers the **caller's own module** (the
  main file is just the common case) instead of the main file only.

### wsbaga — fragmented message reassembly (W2 closed)
- New `ws_read_message`: reassembles continuation frames into one message
  (original opcode, full payload, 64 MiB cap kept). Control frames
  (ping/pong/close) are delivered immediately even mid-message (RFC 6455
  §5.4); a lone continuation or a new data frame mid-message is a
  protocol violation (`bad=1`). `WsConn` gains the accumulator state
  (`frag_op`/`frag`); `ws_read_frame` stays the frame-level API.
- `ws_handle_conn` (echo server) now serves fragmented messages instead
  of closing on opcode 0.
- `tests/ws_test.baga`: fragmented echo (UTF-8 split across frames),
  ping interleaved mid-message, lone-continuation close on a fresh
  connection (the accept loop is serial).

### Language — namespaces (L6): module-qualified calls
- Every imported file is a **module** named by its basename
  (`std/net/http_client.baga` → `http_client`); its functions are
  callable qualified: `http_client.http_get(url)`. Same-named functions
  in different modules are now legal — each decl gets a unique internal
  symbol `module.name`, so the gcc "redefinition" wall is gone.
- Unqualified resolution: the **current file's own** definition wins,
  then the single module defining the name, otherwise a compile error
  (`нееднозначно извикване на 'who' — има я в модулите 'alfa' и 'beta';
  уточни с alfa.who или beta.who`). A local variable named like a module
  shadows it (field access unaffected).
- Same-module duplicates are now a **checker** error (`повторна
  дефиниция на функция`, was: gcc redefinition); forward declaration +
  one implementation stays legal (self/compiler.baga relies on it).
- Clear errors for `unknown.module(...)` and `module.missing_fn(...)`.
- Mechanism: `SrcPos.file` — token/node origin survives the textual
  import expansion (this also unlocks G13 error attribution later).
  Fixed a **latent use-after-scope**: imported files were lexed with a
  filename pointing at the `resolved` stack buffer in `collect_tokens`.
- Scope honestly documented: structs/enums and `go` worker references
  stay global; no `import ... as` alias yet.
- Tests: `tests/ns_mods/{alfa,beta}.baga` + `tests/std/ns_test.baga`
  (qualified calls, main precedence, shadowing) + 4 negative probes in
  `scripts/run_tests.sh`. Docs: §18.1 in both languages.

### Language — `Map<K, struct>` (the last piece of L4)
- Map values may now be struct types — in annotations (`Map<str, Sess>`)
  and in fix-on-first-use inference; struct value types compare **by
  name** (`vec_elem_eq` reused; `type_eq`'s Map branch too).
- C backend: one box per entry (`void *pv` on `baga_MapEntry`, stable
  across rehashes); `baga_map_{set,get}_{str,i64}_box` runtime helpers.
  `map_set` copies in, `map_get` copies out — same semantics as
  `Vec<struct>`.
- **Missing key → field-wise zero struct** (new `emit_zero_struct` in
  codegen): `str` fields come out as `""` — not NULL — so printing a
  missing entry is safe; nested structs recurse; `bytes` zeroed.
- Runtime hardening (needed by the zero struct): `vec_len`/`map_len`
  tolerate NULL and return 0.
- Tests: `tests/std/map_struct_test.baga` (25 checks: copy in/out,
  shared reference fields, missing-key zero struct, del, i64 keys,
  inference, 500-entry rehash) + a negative probe (`Map<str,A>` set `B`)
  in `scripts/run_tests.sh`.
- Gaps closed: oauthbaga **O3**, httpdbaga **G14** — the whole
  "no struct values in containers" class is now gone from the language.
- Docs: `docs/language-{en,bg}.md` §12.5 (struct values + zero-struct
  semantics).

### Language — `Vec<struct>` (L4 closed for struct elements)
- `Vec<T>` element kinds now include **struct types** — in annotations
  (`Vec<Line>`, `[Line]`) and in fix-on-first-use inference. Element
  equality compares struct types **by name**: pushing a different struct
  (or a scalar) into a typed vector is a compile-time error
  (`vec_elem_eq`; `type_eq`'s Vec branch fixed to match).
- C backend: struct elements are **boxed copies** — generic
  `baga_vec_{push,get,set,slice,concat}_box` runtime helpers take the
  element size from the call site (`sizeof(b_T)`); push/set wrap the
  rvalue in a statement expression to get an lvalue. `vec_get` copies
  out by value: mutating the result does not touch the vector, while
  reference-typed fields (`Vec`/`Map`) stay shared — the same semantics
  as plain struct assignment.
- LLVM backend: honest `unsupported` diagnostic (same stance as
  `Vec<bytes>`/`Map`). The `--verify` fragment already skips struct
  constructions honestly — no change.
- Real-world proof: `dec_sum_vec(Vec<Decimal>)` in bagadecimal closes
  gap **D7** (invoice-line sums). Gaps updated: tplbaga **P3** closed,
  oauthbaga **O3** half-closed (`Map<str,struct>` remains), httpdbaga
  **G14** narrowed to `Map` struct values, xmlbaga **X2** unblocked.
- Tests: `tests/std/vec_struct_test.baga` (20 checks: push/get/set,
  copy-in/copy-out semantics, shared reference fields, slice/concat,
  annotated + inferred parameters, growth) + two negative probes
  (`Vec<A>` vs `B`, struct vs scalar) in `scripts/run_tests.sh`.
- Docs: `docs/language-{en,bg}.md` §12.4 (element types + box/copy
  semantics).

### App products — xmlbaga (universal XML, apps-roadmap №11)
- New base package `app-product/xmlbaga`: **pull XML parser + writer**,
  quick-xml style — no DOM (L4: no struct elements in vectors; cursor
  state lives in reference-typed fields so `XmlParser` stays copyable).
- Events: OPEN / CLOSE / TEXT / EOF / ERROR. Self-closing tags emit
  OPEN+CLOSE; comments, PIs and the declaration are skipped; CDATA is raw
  TEXT; DOCTYPE skipped leniently. Full well-formedness errors:
  mismatched/unclosed tags, multiple roots, text outside root, duplicate
  attributes, `<` in attribute values, unknown entities, invalid/control
  char refs, unterminated markup (with byte offsets).
- Entities: builtin five + numeric char refs decoded as UTF-8. Raw names,
  no namespace resolution (X1, documented).
- Writer: text/attr escaping, **sorted (deterministic) attribute order**,
  `xml_elem` convenience, declaration helper; writer → parser round-trip
  in tests.
- CLI `demo.baga`: event dump + `ROUNDTRIP=1` re-emit mode.
- `tests/xml_test.baga` — 35 checks (events, all error paths, entities,
  CDATA, writer goldens, round-trip); sandak discovery builds it.
- Format-specific importers (bank camt.053, invoices) are deliberately
  out of scope — they belong to apps on top of this package.
- Honest gaps in gaps.md: X1 namespaces, X2 no DOM (L4), X3 DOCTYPE
  entities, X4 byte-lenient names, X5 per-char concat (G1 lineage).

### bagaDecimal 0.3.0 — rounding modes + scientific parse (P1 shipped)
- **Rounding modes:** one `dec_round_impl` behind five public entries —
  `dec_round_dp` (half-away, unchanged default), `dec_round_bankers_dp`
  (half-even), `dec_trunc_dp`, `dec_floor_dp`, `dec_ceil_dp`. Dropped
  digits tracked as most-significant-dropped + sticky; zero normalizes
  to +0.
- **Scientific notation:** `dec_parse` accepts `[eE][+-]?digits`
  (`1.23e4`, `1E-2`). Exact when representable; scale > 28 rounds
  half-away, mantissa overflow errors; exponents saturate at ±1000.
- **D6 closed with data:** worst-case `dec_div` (bit-by-bit multi-limb
  path) measured at ≈7 ms — fine for money workloads; Knuth divmod only
  if a bulk workload appears.
- `pg_cell_decimal_or_zero` now carries an explicit "display only, never
  post sums computed through it" warning (strict `pg_cell_decimal` is
  the posting path).
- Tests: `tests/decimal_test.baga` 59 → 86 checks (five modes × signs,
  bankers sticky/odd, exponent round-trips incl. exact 96-bit max,
  exponent error paths).
- Package version 0.2.0 → 0.3.0; PLAN/gaps/design-notes updated.

### bagaDecimal — signed-overflow fixes + mul rescue (P0.6)
- **BUG (crash).** `dec_limb_mul` accumulated 32-bit limb products +
  carry up to ~2^64 in signed `i64`; the sum wrapped negative, the
  arithmetic-shift carry followed, and the carry drain walked out of
  bounds. `dec_mul(3100000000, 3100000000)` aborted at runtime — the
  whole upper half of the 96-bit mantissa range was broken.
- **BUG (silent wrong digits).** `dec_limb_div_small` stepped in base
  2^32 (`rem * 2^32 + limb`) and overflowed for single-limb divisors
  > 2^31: `dec_div_scale(1, 3000000000, 28)` returned confidently wrong
  digits with `ok = 1`.
- **Fix:** the wide paths now run on 16-bit half-limbs — every
  intermediate stays below 2^48 (`dec_limb_mul`, `dec_limb_mul_small`,
  `dec_limb_div_small`). Documented invariant in `docs/design-notes.md`:
  no intermediate may reach 2^63. gaps.md D1 verdict revised.
- **`dec_mul` scale rescue (rust-decimal parity):** a product that still
  exceeds 96 bits after the scale-28 rescale now drops fractional digits
  (half-away on the most significant dropped) until it fits; an error is
  returned only when the integer part alone overflows. Was: loud error.
- **BUG (accounting).** `dec_percent_of` pre-rounded the rate to scale 8
  before multiplying — 33.3333333% of 999999999.99 posted 333333330.00
  instead of 333333333.00. Now exact product → one division by 100 → one
  rounding at the posting. `dec_with_percent` taxes the same rounded
  base it posts.
- Tests: `tests/decimal_test.baga` 40 → 59 checks — big-limb mul,
  192-bit rescale (scale 48 → rescued 19), true overflow stays loud,
  big-divisor div, precise percent, parse/round/-0/to_i64 edges.

### Compiler — non-ASCII string literals before a hex digit (C backend)
- **Bugfix.** `emit_c_string` emitted non-ASCII bytes as `\xHH`, and C hex
  escapes are greedy: a Cyrillic (UTF-8) literal directly followed by an
  ASCII hex digit (`"ел0"`, `"елa"`, `"елf9"`) fused into one invalid
  escape (`\xbb0`) — gcc warned "hex escape sequence out of range" and the
  string came out wrong. Bytes now emit as 3-digit octal (`\ooo`), which
  the standard bounds to exactly three digits.
- Regression probe in `scripts/run_tests.sh` (Cyrillic + `0`/`a`/`f9`).
- `app-product/httpdbaga/gaps.md` G14 brought up to date: `Map<K,V>` has
  shipped, the remaining parallel-Vec shape is the struct-element half
  (L4 lineage, tplbaga P3 / oauthbaga O3).

### Language — `Vec<bytes>` (L4 closed for the bytes element kind)
- `Vec<T>` element kinds now include `bytes` alongside `i64`/`str`/`f64` —
  in annotations (`Vec<bytes>`, `[bytes]`) and in fix-on-first-use
  inference (`vec_push`/`vec_set`). Mixing bytes with another element type
  is a compile-time error, same as the other kinds.
- C backend: elements are boxed `baga_bytes` behind a pointer per slot
  (the `Map` bytes-values precedent); `baga_vec_{push,get,set,slice,concat}_bytes`
  emitted in the runtime. Binary-safe: NUL/0xFF round-trips.
- LLVM backend: honest `unsupported` diagnostic for `Vec<bytes>` (same
  stance as `Map`) instead of a silent fall-back to the i64 helpers.
- `Vec<struct>`/`Vec<Decimal>` etc. remain outside the element whitelist —
  that part of L4 stays open (tplbaga P3, bagadecimal D7).
- Tests: `tests/std/vec_test.baga` (27 checks: round-trip, set, slice,
  concat, annotated parameter, unannotated inference, growth) via baga-test
  discovery; a negative `Vec<bytes>` + str push probe in
  `scripts/run_tests.sh` next to the Map mismatch probes.
- Docs: `docs/language-{en,bg}.md` §12.4 + error/builtin tables; the
  `std/net/tls.baga` flight reader keeps its flat u24 shape (comment
  updated — the format predates `Vec<bytes>` and tracks GCM sequence
  numbers by record order anyway).

## [0.8.0] — 2026-08-04

**Language + ecosystem + pure-Baga cryptography.** README opens with the
language story, the in-tree crypto stack (no OpenSSL at runtime), and the
`app-product` application ecosystem.

### jwtbaga — RS256/ES256 verify + hardening (full crypto stack)
- JWT verify now uses TLS crypto: **RS256** (`rsa_pkcs1_sha256_verify`) and
  **ES256** (`ecdsa_p256_verify_sha256_raw`, JWS R‖S format).
- Hardening: `jwt_alg`, reject `alg:none`, `jwt_verify_hs256` requires
  header `alg == HS256`; `jwt_time_ok` / `jwt_accept_hs256` for exp/nbf/iss/aud.
- `ecdsa_p256_verify_sha256_raw` added in `std/crypto/p256.baga` for JWT.
- HS256 sign/encode unchanged (`jwt_encode`). Asymmetric *sign* still out
  (no private-key API) — verify is enough for OIDC resource servers.
- `tests/jwt_test.baga`: golden HS256, none-attack, exp, RS256/ES256 vectors.

### bagaDecimal — decimal + Postgres NUMERIC (accounting)
- New `app-product/bagadecimal` (**bagaDecimal**), inspired by
  [paupino/rust-decimal](https://github.com/paupino/rust-decimal): 96-bit
  mantissa + scale under `src/` (`ops/`, `parse/`, `format/`, `round/`,
  `convert/`, `money/`, `pg/`, `math/`).
- Core: `dec_parse`, `dec_to_string`, `dec_add/sub/mul/div`, `dec_round_dp`,
  cmp/abs/neg, i64 convert → `DecResult`.
- **Accounting:** `dec_money`, `dec_as_money`, `dec_normalize`,
  `dec_percent_of`, `dec_with_percent`, `dec_sum2/3`.
- **Postgres NUMERIC** (text protocol, like rust-decimal db-postgres):
  `dec_to_pg` / `dec_from_pg`, `pg_param_decimal`, `pg_cell_decimal`,
  `pg_col_is_numeric` (OID 1700). Dependency: `pgbaga`.
- Tests: `tests/decimal_test.baga`, live `tests/decimal_pg_test.baga`
  (INSERT/SELECT/SUM numeric), `examples/money.baga` (27.26).

### TLS 1.3 client, T8 — `https://` + openssl mock (no real OAuth account)
- Application traffic secrets and `TlsConn` (`tls_connect`, seal/open,
  read/write) on pure Baga TLS 1.3.
- **Bugfix:** client Finished HMAC must cover the transcript through
  the server Finished. The T5 probe only checked outer record type ≠ 21
  and accepted encrypted `decrypt_error` alerts (outer type 23).
- `std/net/http_client`: `https://` URLs (port 443); same `http_get` /
  `http_post` / `http_request` API. Self-signed peers accepted (empty
  trust anchor). `!Random` on the client API for ephemeral key share.
- `tests/std/https_test.baga` — live GET against `openssl s_server
  -tls1_3 -www` with a fresh self-signed cert (mock; no third-party
  account). Wired into `scripts/run_tests.sh`.
- Closes oauthbaga gap O1 for the **client** half of G6.

### TLS 1.3 client, T7 — ECDSA-P256 CertificateVerify
- `std/crypto/p256.baga`: NIST P-256 field/point arithmetic on bn limbs;
  `ecdsa_p256_verify_sha256(qx, qy, msg, sig_der)` (SEC1, DER signatures).
- `std/crypto/x509.baga`: EC SPKI (prime256v1 uncompressed) and
  ecdsa-with-SHA256 cert signatures alongside RSA.
- `tls_verify_server`: `ecdsa_secp256r1_sha256` (0x0403) in addition to
  RSA-PSS (0x0804).
- `tests/std/p256_test.baga` — python cryptography vectors (~9 s).
- Live openssl peer runs **twice** in `scripts/run_tests.sh`: RSA-2048
  self-signed and ECDSA-P256 self-signed — both full handshakes green.
- AES_256_GCM_SHA384 still deferred (needs SHA-384 HKDF); servers keep
  picking AES_128_GCM_SHA256 from the ClientHello order.

### TLS 1.3 client, T6 — X.509 + RSA-PSS CertificateVerify
- `std/crypto/der.baga`: minimal DER walker (SEQUENCE / INTEGER / BIT
  STRING / OID, definite lengths).
- `std/crypto/rsa.baga`: `rsa_public` (s^e mod n via bn), PKCS#1 v1.5
  SHA-256 verify (X.509 cert signatures) and EMSA-PSS SHA-256 verify
  (sLen=32, MGF1-SHA256 — TLS `rsa_pss_rsae_sha256`).
- `std/crypto/x509.baga`: parse Certificate → TBS + RSA SPKI (n, e) +
  signature; self-signed and trust-anchor checks.
- `std/net/tls.baga`: `TlsFlight` exposes `cv_algo` / `cv_hash`;
  `tls_verify_server(flight, anchor_der)` trusts the leaf and verifies
  CertificateVerify. Empty anchor = accept self-signed (dev peer).
- `tests/std/rsa_pss_test.baga` — python `cryptography` golden vectors
  (PSS accept/reject + self-signed cert). Live openssl path in
  `tls_handshake_test` now asserts cert + CV.
- Honest gaps: no name constraints / time / revocation; ECDSA is T7.

### Toolchain / packaging — Makefile is C-only; tests via sandak + baga-test
- The root `Makefile` no longer embeds the regression suite (~670 lines of
  hand-listed package tests). It builds the C toolchain only (`baga`,
  `sandak`, optional `baga-llvm`, `libbaga_par.so`) and thin targets
  (`test`, `self`, `test-llvm`).
- `make test` → `scripts/run_tests.sh`:
  - **sandak** discovery — every `app-product/*/sandak.toml` and
    `apps/*/sandak.toml` is built (no hand-maintained package list);
  - **baga-test** discovery — every `tests/**/*_test.baga` (specials
    with env/peers: registry, oauth PG, TLS vs openssl);
  - **run_verify.sh** — the static `--verify` oracle (M0–M18).
- Closes the GitHub-linguist skew where Makefile looked like a large
  share of the repo; package work stays in the package system.

### TLS 1.3 client, T4+T5 — record layer, ClientHello, encrypted handshake
- `std/net/tls.baga`: TLS 1.3 client core — record layer
  (`tls_read_record`), ClientHello builder (x25519 key share,
  supported_versions, signature_algorithms), ServerHello parser, the RFC
  8446 §7.1 key schedule (`tls_schedule`), flight decryption
  (`tls_open_handshake`: multi-record, GCM sequence numbers, inner
  content-type stripping, transcript walk, server-Finished HMAC verify)
  and the client Finished builder (`tls_finished_record`).
- `tests/tls_handshake_test.baga` — a full live handshake against
  `openssl s_server -tls1_3` with a fresh self-signed cert, wired into
  make test: ClientHello → ServerHello → decrypt
  EncryptedExtensions/Certificate/CertificateVerify/Finished → verify the
  server Finished → send the client Finished and assert no alert.
- Scars, documented:
  - the "derived" key-schedule step takes the transcript hash of the
    empty message (SHA-256 of "") as context — not an empty context;
    validated against RFC 8448 known answers before the live server
    would decrypt anything;
  - the ServerHello key_share is a single KeyShareEntry (the list-length
    wrapper exists only in the ClientHello);
  - openssl splits the flight into separate records (EE | Cert | CV+Fin)
    and sends a middlebox-compat ChangeCipherSpec; the flight reader
    handles both;
  - `openssl s_server` without `-quiet` resets connections (stdin loop);
    the harness runs it with `-quiet < /dev/null`;
  - `Vec<bytes>` is not a supported element kind yet (L4) — the flight
    reader takes a u24-length-prefixed `bytes` instead.

### TLS 1.3 client, T3 — HKDF + AES-GCM (std/crypto)
- `hkdf.baga`: RFC 5869 HKDF-SHA256 (extract with the empty-salt →
  HashLen-zeros rule, expand); Appendix A cases 1–3 pass.
- `aes.baga`: AES-128/256 forward cipher (FIPS-197 C.1/C.3). The S-box
  is computed at expand time from GF(2^8) inversion + the affine map —
  no 256-byte literal to mistype. Decryption is deliberately absent:
  GCM needs only the forward direction.
- `gcm.baga`: AES-GCM AEAD — GHASH in GF(2^128) (right-shift form,
  R = e1‖0^120), CTR from J0 = nonce‖00000001 (12-byte nonces, the
  TLS 1.3 shape), 16-byte tags verified with ct_eq_b.
- `tests/std/hkdf_test.baga` + `tests/std/aes_gcm_test.baga`: vectors
  from RFC 5869, FIPS-197, and python `cryptography` generated offline
  (AES-256-GCM, 60-byte non-block PT + AAD, AAD-only, tamper/nonce/AAD
  rejection). Both in the make test std loop; ~0.5 s each.
- Scars, documented: the S-box affine rotations were first written as
  rotl 5/6/7 instead of 3/2/1, and ShiftRows filled its buffer in
  push order (a transpose) — both caught by the intermediate-state
  probes against the FIPS walkthrough, not by the end-to-end vector.

### TLS 1.3 client, T2 — std/crypto/x25519.baga (RFC 7748 ECDH)
- X25519 on top of bn.baga: clamped scalars, Montgomery ladder (bits
  254..0) with constant-time conditional swaps, field arithmetic mod
  2^255-19 with the 2^255 ≡ 19 fold, final inversion z^(p-2) over fmul.
  Little-endian encoding per the RFC. A full scalar multiply ≈ 0.1 s.
- `tests/std/x25519_test.baga` — RFC 7748 §5.2 (first iteration, k=u=9)
  and §6.1 (both public keys + shared secret, both directions); in the
  make test std loop. The 1,000-iteration vector is documented but kept
  out of CI.
- Scar, documented: the first fold dropped a carry that escaped limb 9
  (≥ 2^260 must fold by ×608 again) — (p-1)² came out p-607 instead of
  1, exactly one lost 608. Caught by the modexp-free field probes.

### TLS 1.3 client, T1 — std/crypto/bn.baga (fixed-width bignum)
- The milestone plan is `docs/superpowers/plans/2026-08-04-tls-client.md`
  (T1–T8; closes №10 P2 / gap G6 — the last production blocker).
- New `std/crypto/bn.baga`: unsigned bignum on 26-bit limbs in
  `Vec<i64>` — add/sub/cmp, schoolbook mul, **in-place** shift-subtract
  mod, modmul, left-to-right modexp, big-endian byte codec. Signed-i64
  discipline: widest RSA-2048 column stays below 2^59.
- `tests/std/bn_test.baga` — 19 golden-vector checks, oracle = python
  bigints computed offline: byte round-trips, 256-bit mul/mod/exp
  (NIST P-256 prime), and RSA-2048 modexp over the RFC 3526 group prime
  (e=65537 fast path + 512-bit exponent slow path, ~1.5 s measured).
- Design scar, documented: the first `bn_mod` rebuilt the shifted modulus
  per bit — thousands of arena allocations per mod, and the bump arena
  never reclaims, so the slow-path modexp was OOM-killed. In-place
  reduction fixed it (43 s OOM → 2.1 s green).

### std/net — URL percent-encoding (oauthbaga gap O2)
- New `std/net/url.baga`: `url_encode` / `url_decode` (RFC 3986 §2.1) —
  unreserved set passes through, everything else `%XX` per UTF-8 byte;
  decode is byte-exact (rebuilt through `bytes`, since `chr()` would
  UTF-8-encode values ≥ 0x80 and double-encode the stream) and lenient
  (malformed `%XX` and `%00` copy through literally — baga strings are
  C strings).
- `tests/std/url_test.baga` — 20 checks incl. Cyrillic/emoji round-trips;
  in the `make test` std loop.
- First user: oauthbaga percent-encodes `redirect_uri` in the authorize
  redirect and the provider decodes it.

### App products — oauthbaga worker model (O5 closed)
- PG mode is now fully concurrent: every HTTP connection runs on its own
  `go_bg` worker with its own DB connection (fmr legacy idiom); the CSRF
  authorize states moved to an `oauth_states` table (migration
  20260804102), so nothing is shared between threads. Dev (in-memory)
  mode stays serial.
- `oauth_pg_test` proves the cross-thread flow: the `/login` state row is
  written by one connection and consumed by another; the suite is now on
  the default ports (the workers rebuild config from env).
- `demo.baga` migrates once before booting the two nodes (two concurrent
  `migrate_up` would race on the version insert).
- Honest scar, recorded in gaps.md: the first bg handlers responded
  without `tcp_close(fd)` — with Connection: close the client reads to
  EOF, so an unclosed fd hangs clients until their read timeout.

### Compiler — call-site Vec/Map element inference (tplbaga P5 closed)
- **Soundness fix.** An unannotated `vec_new()` / `map_new()` passed to a
  typed parameter (`Vec<str>`, `Map<str,str>`, …) now gets its element
  kind fixed from the callee parameter at the call site — the same
  fix-on-first-use mutation `vec_push` already did. Before, a later
  `vec_get` fell into the historical i64 default and the codegen read
  `str` memory as `i64`: garbage output, no diagnostic, no compiler
  complaint. Found by tplbaga (№7) the way the roadmap intended.
- The reverse order (`vec_get` before the fixing call) is now a compile
  error instead of a silent wrong type.
- Two regressions in `make test` (vec + map call-site inference).
- `type_str`: rotating buffers — two `Vec`/`Map` types in one diagnostic
  no longer overwrite each other's text.

### Compiler — LLVM `ord` decodes UTF-8 (oracle parity)
- The LLVM backend's `baga_ord` returned the first *byte* (208 for "А");
  the C runtime decodes the code point (1040). Now both backends decode
  1–4 byte UTF-8 sequences; `examples/strings.baga` matches byte-for-byte
  and `make test-llvm` is fully green.

### App products — oauthbaga P1 (Postgres persistence, №10 "всичко заедно")
- The pgbaga/ormbaga leg of №10: `oauth_codes` / `oauth_refresh` /
  `oauth_sessions` tables (goose-style migrations, version 20260804101),
  `$N` parameterized queries only; `OAUTH_PG=1` + `PG*` switches the
  backend, in-memory maps remain the dev mode (both live-tested).
- Store ops meet at typed rows (`OaCode`/`OaTok`/`PxSess`); the JSON
  record codec is now contained to the in-memory backend (O3 shrunk).
- One DB connection per HTTP connection (fmr legacy idiom) — no struct
  rebinding through the handler chain, concurrency-safe, and the natural
  path to a worker pool (O5 mostly closed; the CSRF `states` map stays
  per-node for now, O7 notes the per-request SCRAM cost).
- `tests/oauth_pg_test.baga` — live full cycle on ports 18692/18693 plus
  DB-level proofs: the code row is consumed by the exchange, refresh
  rotation keeps exactly one live token, the session row appears on
  login and dies on logout. Wired into `make test` like registry_test
  (`OAUTH_PG=1 PGDATABASE=baga_oauth`).

### App products — oauthbaga (OAuth proxy, apps-roadmap №10 complete)
- New product `app-product/oauthbaga`: the integration exam — std HTTP
  client (№2) + jwtbaga + cookie sessions, pages rendered by tplbaga
  (first cross-product dependency in the series; №7 feeds №10).
- Provider node: `/oauth/authorize` (auto-approve dev profile, one-time
  codes), `/oauth/token` (authorization_code + refresh_token grants with
  rotation), `/api/me` (Bearer JWT guard); RFC 6749-shaped JSON errors.
- Proxy node: `/login` (CSRF state) → `/callback` (real server-to-server
  code exchange over the std client) → `sid` cookie session with
  transparent refresh → `/logout`.
- Two serial nodes (`cell2` ports, kvbaga idiom): the provider never
  calls itself, so no self-accept deadlock; state is `Map<str,str>` of
  JSON records (L4 stand-in); `TokenReply`/`PxTokens` continue the L3
  Result stand-in convention.
- `demo.baga` boots both nodes (`OAUTH_PORT`/`OAUTH_PROVIDER_PORT`/
  `OAUTH_SECRET`); a browser completes the flow on loopback.
- `tests/oauth_test.baga` — live full cycle: authorize → exchange →
  bearer → protected → refresh/rotation → browser flow → logout
  (47 checks); runs via `scripts/baga-test`.
- Probes: O2 no URL percent-coding in std (third hand-rolled query
  parse); O5 go_bg carries i64 only → serial nodes until state moves to
  Postgres (P1); TLS/G6 still the production blocker (O1).

### httpdbaga — 302 reason phrase
- `reason_phrase(302)` now says "Found" (was the generic "Status") —
  the OAuth redirects were the first 3xx on the wire.

### App products — tplbaga (HTML templates, apps-roadmap №7)
- New product `app-product/tplbaga`: mustache-ish subset — `{{ expr }}`
  escaped interpolation, `{{{ expr }}}` raw, `{% if %}` / `{% else %}` /
  `{% endif %}` (nestable, `!` negation), `{# comments #}`, filter chains
  `{{ v | trim | upper }}` (upper/lower/trim/len/default:arg, ASCII case).
- Tokens are prefix-encoded `Vec<str>` + a `Map<i64,i64>` jump table for
  block pairing — one iterative walk, no recursion (L4 stand-in); `TplOut`
  ok/err struct as the L3 stand-in; filters dispatch by name — the
  designated L5 (closures) probe.
- `demo.baga` CLI: template file + `key=value` data file (exit 0/1/2).
- `tests/tpl_test.baga` — 46 checks (escape, jump table, filters, error
  paths, realistic page); runs via `scripts/baga-test`.
- Probes: unannotated `vec_new()` passes the checker but codegen emitted
  i64 element access until annotated (P5, compiler bug candidate); `spec`
  keyword cannot be an identifier and the diagnostic doesn't say so (P6).

### App products — jsonrpcbaga (JSON-RPC 2.0, apps-roadmap №6)
- New product `app-product/jsonrpcbaga`: JSON-RPC 2.0 subset over HTTP —
  single/batch, notifications, standard error codes, methods
  `ping`/`add`/`echo`/`fail` via name switch.
- `RpcResult` struct as L3 Result stand-in; `rpc_handle_body` pure +
  `rpc_serve` accept loop; `tests/jsonrpc_test.baga` (pure + live HTTP).
- Gaps: no sum types (R1/L3), no function-value method table (R2/L5).

### App products — queuebaga (task queue, apps-roadmap №5)
- New product `app-product/queuebaga`: disk-backed jobs, `chan` of job ids,
  `go_bg` workers, reverse-payload demo work, `fail:` retry until max
  attempts, `q_wait` with timeout.
- Flat paths `<prefix>.<id>.{job,status,result,attempts}` (no mkdir).
- `tests/queue_test.baga` + demo. Gaps: i64-only chan/go (Q1), no setenv
  (Q2), write_file truncate races (Q3), no L5 handlers (Q5).

### App products — grebaga (grep-like CLI, apps-roadmap №9)
- New product `app-product/grebaga`: literal + mini-pattern (`.`/`*`/`\`),
  ASCII `-i`, streaming line scan (chunked read; empty line ≠ EOF), CLI
  `demo.baga` (`-n`/`-i`, files or stdin, exit 0/1/2).
- `tests/grep_test.baga` — match unit tests + live file stream.
- Probe: std `read_line` empty/EOF collapse → custom scanner (G1).

### App products — testbaga (test asserts + runner, apps-roadmap №8)
- New product `app-product/testbaga`: fail-fast `assert_true` /
  `assert_eq_i64` / `assert_eq_str` / `assert_ne_str`, plus `Suite`
  (continue-on-fail, `suite_finish` → exit code).
- `scripts/baga-test` — discovers `*_test.baga` and runs each via baga
  (shell driver: no readdir/process spawn in language yet).
- Dogfood: `tests/testbaga_test.baga`; `tests/std/sort_test` migrated off
  local `check`.
- Gaps: T1 process spawn, T2 list_dir, T3 function values (L5).

### App products — mdbaga (Markdown → HTML, apps-roadmap №4)
- New product `app-product/mdbaga`: CommonMark-ish subset — ATX headings,
  paragraphs, emphasis, inline/fenced code, ul/ol, blockquotes, hr, links,
  HTML escape; `md_to_html` / `md_to_document`.
- CLI `demo.baga` reads `arg(0)` via `read_file`, prints HTML (`MDDOC=1` for
  full document shell). Package: `sandak build`.
- `tests/md_test.baga` — escape, blocks, inline, XSS-ish `<` in text/code.
- Probe gaps: nested concat still dominates builders (M1 / G1); no
  file-exists vs empty distinction on `read_file` (M2).

### App products — chatbaga (WebSocket chat, apps-roadmap №3 complete)
- New product `app-product/chatbaga`: multi-room chat on a single-threaded
  `poll(2)` event loop — JSON join/msg over wsbaga text frames, room
  broadcast, leave notifications, error replies.
- Closes **W1 / K1** (serial accept): one poll set watches the listener +
  every client fd; connection state lives in `Map`s keyed by fd.
- Forced **`Map` bytes values** into the language (`Map<i64, bytes>` residual
  buffers) — also the path to close kvbaga K2 for binary store values.
- `demo.baga` standalone server (`CHATPORT`, default 16460); interop with
  `wscat` and raw RFC 6455 clients (UTF-8 text, multi-client broadcast).
- `tests/chat_test.baga` — 18 live checks (two clients, errors, room
  isolation, close/`left`); package via `sandak build` (app-product list).

### std/net — poll(2) event loop primitive
- `std/net/poll.baga`: `poll_wait(fds, timeout_ms)` / `poll_has` over
  SYS_poll (POLLIN|POLLERR|POLLHUP). Same memfd staging pattern as tcp.
- `tests/std/poll_test.baga` in the `make test` std loop.

### Language — Map bytes values (kvbaga K2 path)
- `Map<K,V>` values may be `bytes` (in addition to i64/str/f64). Checker +
  C runtime (`baga_map_*_bytes`); missing key → empty bytes.
- `tests/std/map_test.baga` covers NUL/0xFF round-trip through map values.

### App products — wsbaga (WebSocket, apps-roadmap №3)
- New product `app-product/wsbaga`: RFC 6455 — server handshake
  (`Sec-WebSocket-Accept`), frame codec (FIN/opcode, 7/16/64-bit lengths,
  client masking), text/binary/ping/pong/close handling, buffered
  `ws_read_frame`, echo server `ws_serve(port)`, and a masked client
  (`ws_client_connect` verifies the accept key).
- **Interop-verified**: `wscat` (Node.js) echoes UTF-8 text and 900-byte
  payloads against the Baga server; loopback `tests/ws_test.baga` covers
  all length boundaries (125/126/65535/65536), binary with NUL/0xFF,
  ping→pong, close→EOF (14 checks).
- Honest limits in gaps.md: serial accept closed by chatbaga (W1 = K1 →
  poll); no fragmented-message reassembly (W2) still open.

### std/crypto — SHA-1 (probed into existence by wsbaga)
- `std/crypto/sha1.baga`: RFC 3174, same shape as sha256 (Vec core +
  `bytes` wrappers: sha1/sha1_hex/sha1_b/sha1_b_hex).
- `tests/std/sha1_test.baga`: RFC vectors incl. million-'a' and the
  RFC 6455 accept-key vector; in the `make test` std loop.
- SHA-1 only for protocol mandates (RFC 6455); sha256 stays the default.

### apps/registry — пакетен registry за sandak (apps-roadmap №2, втора половина)
- New app `apps/registry`: JSON/HTTP package index on the fmrbaga/ormbaga/
  pgbaga stack — `GET /v1/packages[?q=]`, `GET /v1/packages/{name}`,
  `POST /v1/packages` (publish = upsert package + unique version; 409/422
  error shapes). Migrations create `reg_packages` / `reg_versions`.
- `sandak search [term]` / `sandak publish --git URL [--rev R] [--subdir S]
  | --path P` — the client is a Baga program (`src/sandak_registry.baga`)
  executed by sandak through the compiler, talking HTTP via the new std
  client. Registry URL from `SANDAK_REGISTRY` (default http://127.0.0.1:8090).
- `baga` CLI gained **program arguments**: `baga prog.baga arg1 arg2…` (and
  an explicit `--` separator) — everything after the input file reaches
  `arg()`/`arg_count()` of the compiled program. Before this, `arg()` had
  no way to receive values through compile-and-run.
- fmrbaga `jbody_parse_str` now rejects malformed bodies with
  `json_strict_valid` before the lenient parse (G13 in a real request path).
- `tests/registry_test.baga` — first full-stack live HTTP test: boots the
  server in a go_bg worker, drives it through std/net/http_client (18
  checks: publish/dup-409/show/index/search/404/400/422). In `make test`.

### std/net — HTTP/1.1 client (apps-roadmap №2, първа половина)
- `std/net/http_client.baga`: `http_request(method, url, headers, body,
  timeout)` + `http_get` / `http_post`. URL parse (http:// only — https
  waits for TLS), DNS hostnames through `tcp_connect_to`, `Map<str,str>`
  request/response headers (lowercased, case-insensitive lookup via
  `http_resp_header`), Content-Length + chunked bodies, read-to-close.
- First product of the map type in std itself: headers are `Map<str,str>`.
- `tests/std/http_client_test.baga` — 17 live loopback checks against an
  httpdbaga worker (GET/POST/UTF-8 bodies, chunked, 418, refused, bad URL);
  wired into `make test`.
- Gap found (L6): no namespaces — the client's `http_header` collided with
  httpdbaga's; renamed to `http_resp_header`. Prefix convention holds until
  module scope exists.

### Language — `main -> i64` exit code (kvbaga K3 closed)
- The C wrapper emitted `b_main(); return 0;`, swallowing the exit code of
  `fn main() -> i64`. Now `return (int)b_main();` for i64/i32 mains; void
  mains unchanged. The baga CLI already propagated `WEXITSTATUS`.
- Regression check in `make test`; kvbaga gaps.md K3 closed.

### App products — kvbaga (Redis-compatible KV server)
- New product `app-product/kvbaga`: a RESP2 KV server built deliberately on
  the new map type — the first "app as language probe" on `Map<K,V>`.
- `resp.baga` (pure RESP2 codec: buffered parse, reply builders, client
  round-trip), `store.baga` (`Map<str,str>` + `Map<str,i64>` deadlines,
  lazy TTL expiry), `server.baga` (serial accept loop for `go_bg`,
  idle `SO_RCVTIMEO` guard).
- Commands: PING, SET [EX s], GET, DEL, EXISTS, INCR, KEYS, EXPIRE, TTL,
  DBSIZE, QUIT — Redis-shaped errors (`-ERR`, nil bulks, arity checks).
- Honest limits logged in gaps.md (K1–K5): serial connections (`go()`
  carries only i64 — the store can't cross threads), text-only values,
  and the swallowed `main` exit code (K3 — repo idiom is `exit(1)`).
- Tests: `tests/kv_test.baga` — 27 live loopback checks; demo boots a
  worker and drives it. Both wired into `make test`.

### Language — `Map<K, V>` (first-class hash table)
- New type `Map<K, V>`: keys `i64`/`str`, values `i64`/`str`/`f64`/`bytes` —
  the same fix-on-first-use rules and annotations as `Vec<T>`; mixing key or
  value types is a compile-time error. (`bytes` values added with chatbaga.)
- Builtins: `map_new`, `map_set`, `map_get` (zero-value when absent),
  `map_has`, `map_del`, `map_len`, `map_keys` (→ `Vec<str>`/`Vec<i64>`).
- Maps are pointers: passing one to a function shares it (mutate-through,
  unlike by-value structs) — the natural store for servers and caches.
- C backend: chained hash table (`baga_Map`, FNV-1a / Murmur-mix hashing,
  grows at load factor 3/4). LLVM backend: honest "unsupported" diagnostic.
- Self-hosting parity unchanged (`make self` fixed point holds); the self
  compiler does not parse `Map` yet (documented limitation).
- Docs: `docs/language-{en,bg}.md` §12.5 + type/builtin tables.
- Tests: `tests/std/map_test.baga` (bytes + rehash growth) + two negative
  type-error checks wired into `make test`.

### std/net — production connects
- **DNS resolution:** `tcp_resolve_ipv4` — hostnames via `getaddrinfo`
  (AF_INET, `mem_read` pointer-walk through the `addrinfo` list); dotted
  IPv4 still short-circuits the resolver.
- **Timeouts:** `tcp_set_timeouts` (SO_RCVTIMEO + SO_SNDTIMEO) — a blocked
  read/write/connect fails instead of hanging forever.
- **Client tuning:** `tcp_set_nodelay` (TCP_NODELAY), `tcp_set_keepalive`
  (SO_KEEPALIVE); `tcp_connect_to(host, port, timeout_s)` wires all of it.
  `tcp_connect` keeps its classic behavior.
- New primitive `mem_read(addr, n)` — copy arbitrary process memory into a
  Baga `str` via memfd (with the offset reset; SYS_write advances it).

### App products — pgbaga (Postgres adapter)
- **Production connect:** `pg_connect_to(host, port, ..., timeout_s)` —
  hostname or IPv4, bounded connect/read/write; `pg_set_timeout` retunes a
  live connection; **`pg_cancel`** sends CancelRequest on a fresh connection
  using the BackendKeyData captured at startup.
- **JSON/JSONB tables end to end:** `pg_param_json` binds (`$N::json[b]`),
  column OID detection (`pg_col_is_json` / `pg_col_is_jsonb`), JSON cell
  accessors (`pg_cell_json` / `pg_cell_json_ok`), and validated literals in
  ormbaga (`sql_json` / `sql_jsonb`).
- `std/json`: new `json_strict_valid` — a strict RFC 8259 validator
  (the existing `json_parse` stays lenient for recovery).
- Typed getters: `pg_cell_bool`, `pg_cell_f64`; transaction wrappers
  `pg_begin` / `pg_commit` / `pg_rollback`; structured error accessors
  `pg_sqlstate` / `pg_err_message`.
- `PgReader` now lives inside `PgConn` — buffered socket state survives
  across queries (gap G9 closed; ground for LISTEN/NOTIFY later).
- Hardening: `pg_read_msg` rejects message lengths outside `[4, 2^30-1]`.
- `tests/pg_test.baga`: live JSON table round-trips + strict harness
  (a FAIL now exits 1 instead of printing "all passed"); 70 checks.

### Packages — sandak (пакетна система)
- New tool `sandak`: `sandak.toml` manifests, path + git dependencies
  (with `subdir` for monorepos), `sandak.lock` with `--locked`, and
  `fetch`/`build`/`run` commands. Zero dependencies (libc + git + gcc).
- Compiler: repeatable `-I <dir>` import search path flag.
- The whole monorepo is packaged: `std`, `app-product/*`, `apps/api` have
  manifests; imports are package-named (`import "fmrbaga/app.baga"`).
- Docker: multi-stage `Dockerfile` + `docker-compose.yml` — point `APP_REPO`
  at a git URL and the container clones toolchain + app + deps and builds.

## [0.7.0] — 2026-08-02

Second tagged release: M14–M18 static verification, soundness fixes, evaluation
and research docs. CLI: `baga --version` / `-V` prints `baga 0.7.0`.

### Static verification — M18: `!Overflow` as an effect (effect system ≡ verifier)
- Arithmetic safety (M15) is now a **type-level effect**. `!Overflow` is a
  permission (like `!IO`), not a claim: the M15 kind-4 obligations are the
  *effect inference* for `!Overflow`, and the one-way effect check is the
  *discharge*. The effect system and the verifier become one judgement.
- A function **without** `!Overflow` claims overflow-safety; `--verify`
  proves it (`ефект !Overflow: безопасна — типът е точен`), refutes it with a
  concrete witness when it overflows (undeclared overflow ⇒ nonzero exit), or
  honestly reports НЕ МОГА ДА РЕША.
- A function **with** `!Overflow` is discharged: the overflow is still printed
  as evidence, but it is no longer a contract violation and does not fail
  verification (`ensures` verdicts are idealized-ℤ-only). Over-declaring
  `!Overflow` on a provably-safe function is allowed (noted as redundant).
- `!Overflow` propagates through calls via the generic effect merge — a caller
  must declare or catch it ("необработен ефект !Overflow"); no checker change
  was needed.
- The fragment gate now admits `{Par, Overflow}` (`ret_has_unverifiable_effects`);
  functions with other effects still skip honestly and make no overflow claim.
- The M15 exit-flag rule is gated: a REFUTED arithmetic obligation fails
  verification only when the function does not declare `!Overflow`. No
  existing example declares `!Overflow`, so all prior exit codes are unchanged.
- `--verify --json` adds an `overflow_effect` field
  (`{analyzed, declared, safe, result, witness}`); `--proofs` emits a
  `theorem <fn>_overflow_safe`.
- Examples: `examples/verify/ovf_eff_{safe,refuted,declared,unknown,redundant,skip,propagate,propagate_ok}.baga`.
- Notes: `docs/thesis-m18-overflow-effect.md` (the culmination),
  `docs/thesis-open-problems.md` (liveness / full BV / rich polynomials),
  `docs/thesis.md` (binding research monograph).
- Doc seriousness pass: research monograph/notes without degree theatre;
  proof sketches vs LA certificates; CLI/`--verify` recursion claim;
  self-host LOC (~2660); STLC SN not claimed for full Baga; theory placement
  among tools instead of curriculum comparisons.

### Static verification — M17: pair abstraction (`cell2` + channel pair APIs)
- `cell2(a,b)` / `cell2_0(p)` / `cell2_1(p)` are exact rewrites in the
  verifier (`cell2_0(cell2(a,b)) = a`) — allowed anywhere, including inside
  conditions (`if cell2_0(r) == 1`).
- The pair-returning channel APIs are now in the fragment with ranges for
  the status component and M16 content axioms for the value component:
  - `chan_recv2` (ok ∈ [0,1]), `chan_try_recv` / `chan_recv_timeout`
    (status ∈ [0,2]), `chan_select2*` (which ∈ [0,3]; value gets only the
    axioms BOTH channels share).
  - `select2_wait`'s which ∈ {0,1,3} is modeled as the interval [0,3]
    (over-approx; the abstract status keeps refutations honest).
- `go(worker, cell2(a, b))`: packed arguments work; a worker's
  `requires cell2_1(p) >= 1` is discharged at spawn where the pair's
  components are visible. Inside the worker, packed params stay honestly
  opaque.
- Examples: `examples/verify/pair_{recv2,select,go}.baga`.
- Note: `docs/thesis-m17-pairs.md`.

### Static verification — M16: channel content invariants (rely–guarantee)
- New statement-level annotation `invariant <expr>` (contextual keyword):
  - `invariant c[*] >= 1` — "every payload sent on channel `c` satisfies the
    predicate", anchored on the channel's resolved symbolic var (aliases work).
  - scalar form (no `[*]`) acts as `assume` — the path gains the constraint.
  - `chan_send` discharges the predicate (else the axiom is dropped, M3
    rule); `chan_recv` instantiates it on the result.
- Cross-thread: a worker's `requires c[*] ...` is discharged against the
  caller's axioms at `go` spawn (kind-2 obligation, provable); a worker
  without matching requires drops them at spawn — honest, never unsound.
  The same discharge/drop rules apply at plain M5 calls.
- `go` workers may now declare `Par` effects (channel-using workers were
  previously outside the fragment; non-`Par` effects still skip).
- Examples: `examples/verify/chan_inv{,_bad,_par,_escape}.baga`.
- Note: `docs/thesis-m16-channel-invariants.md`.

### Static verification — M15: arithmetic safety (the ℤ-vs-i64 bridge)
- New kind-4 obligations: every `+ - * -x / % <<` in verified code gets a
  verdict — ДОКАЗАНО (cannot overflow on this path), ОБРОЧЕНО with a concrete
  large-magnitude witness (e.g. `abs(INT64_MIN)`, `n + 1` at `n = INT64_MAX`,
  `n / m` at `m = 0`), or honestly НЕ МОГА ДА РЕША.
- Exact bound search over the FM core (binary search on feasibility);
  products use tightest provable |factor| bounds, compared in `__int128`.
- When all arith obligations of a function are proven, the idealized-ℤ model
  and the i64 runtime coincide — the output says so; otherwise it marks the
  ensures verdicts as idealized-model-only. JSON: `"arith": [...]`.
- The extreme window (2^62, 2^63) reports UNKNOWN, never a false proof.

### Soundness fixes (found by M15)
- **M1 loop havoc**: variables assigned/let-bound in a `while` body are now
  havoced before the invariant is assumed (head + post-loop states). Before,
  the post-loop state kept stale pre-loop values, making invariants vacuous —
  a loop returning `-n` was falsely ДОКАЗАНО for `output >= 0`. Now honestly
  UNKNOWN unless the invariant really covers the variable
  (`examples/verify/loop_havoc.baga`).
- **Rational core**: `rat_add/rat_mul/rat_mk/v_gcd/rat_neg` are now
  INT64_MIN-safe (`__int128` intermediates); `fm_sat` bails out conservatively
  (SAT = "cannot decide") on overflowed constraints.

### Static verification — M14: `!Par` enters `--verify`
- Functions whose only effect is `Par` are now verifiable (other effects
  still skip honestly).
- **Fork–join determinism:** for a pure verifiable worker `f`,
  `join(go(f, x)) ≡ f(x)` — the worker spec applies via M5 assume–guarantee
  (requires discharged at spawn, ensures assumed for the join result).
- **Handle protocols:** ghost state per symbolic handle —
  `spawn → join | detach`; join/detach after consume is REFUTED with a
  counterexample (join-after-detach is fatal at runtime). Channels track
  open/closed; `send` on a known-closed channel is provably `-1`.
- New JSON field `"protocol"` for kind-3 obligations.
- Boundary (honest skips): pair-returning builtins (`chan_recv2`,
  `chan_try_recv`, `chan_select2*`), mutexes, `pool_map`, effectful workers.
- Examples: `examples/verify/par_{join,join_bad,detach_bad,chan}.baga`.
- Note: `docs/thesis-m14-par-fragment.md`.

### Proof extraction
- `--proofs` now prints the verifier's established facts, not just heuristics:
  - `_terminates` uses the real verdict — recursion with a proven `decreases`
    measure is reported as full correctness; otherwise honestly partial.
  - while-loop invariants appear as `lemma <fn>_invariant_<k>` with their
    Hoare status (init + preservation proven, or honestly unproven → UNKNOWN).

## [0.2.0] — 2026-08-02

First tagged release after the static-verification arc and theory write-up.

### Static verification (`--verify`)
- **M0–M7** — linear i64 paths, while invariants, bounds, element axioms,
  assume–guarantee recursion, `decreases` termination, integer tightening
- **M8–M12** — product symbols, sign table, const/var div–mod, floor mul,
  complete square, AM-GM identity, conclusiveness gate (no false alarms)
- **M13** — products inside `if`/`while` guards; sound bitwise envelope
  (`| & ^` neutrals, `n&1∈{0,1}`, `<<`/`>>` special cases)

### Concurrency & backends
- `!Par`: `go` / `join` / channels / select wait–timeout
- LLVM `!Par` parity via `libbaga_par.so`

### Docs
- `docs/theory-{en,bg}.md` — Fourier–Motzkin, Farkas, ℤ-tightening, M0–M13
- `docs/thesis-m13-nonlinear-fragment.md` — research note

### CLI
- `baga --version` / `-V` prints `baga 0.2.0`

## [0.1.0] — unreleased baseline

Bootstrap compiler, self-hosting, effects, specs runtime, std library, playground.
