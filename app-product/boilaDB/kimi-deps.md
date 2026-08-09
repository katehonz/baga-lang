# kimi-deps — препоръки за възстановяване на слоевата дисциплина

Дата: 2026-08-09. Повод: преглед на реалните `import`-и спрямо ARCHITECTURE §3
(„еднопосочни зависимости, без нагорни import-и"). Изводът: монолитът е наред
(един бинарник, 89 файла, нито един > 400 реда, тестовете минават), но
„модулната" част е компрометирана — има нагорни import-и и кръгови
зависимости. Принцип 4 („всеки модул може да се изтръгне в отделен пакет без
пренаписване") в момента не важи за storage, txn, catalog и index.

## Намерени нарушения (с доказателства)

Целевата йерархия по §3: `core < storage < txn < catalog/index < модали < sql < api`.

1. **storage → sql, storage → txn (2 слоя нагоре). — FIXED (D2)**
   `BoilaServer` + multi-DB registry са в `server/databases.baga`;
   `boila_server_exec*` в `server/exec_server.baga`. storage/ държи
   само `shards.baga`. Grandfather `storage:sql`/`storage:txn` махнати.
2. **txn → ts (нагоре към модал). — FIXED (D3)**
   `BoilaTxnWrite.ttl_sec` се попълва от caller-а (`boila_txn_put_ex` в
   exec_dml/exec_modify с `t.ttl_sec`); commit само подава на
   `boila_put_ex` (преместен в storage/shards). Grandfather `txn:ts` махнат.
3. **catalog → sql (нагоре). — FIXED (D4)**
   `BoilaColDef` / `BoilaCreate` / `BoilaAlter` (+ helpers) са в
   `catalog/ddl_types.baga`; sql/ast и parse_alter ги импортват оттам.
   Grandfather `catalog:sql` махнат.
4. **catalog → модалите (нагоре). — FIXED (D5)**
   Modal wipe APIs: `fts_wipe.baga`, `hnsw_wipe.baga`, `graph/*_wipe*`.
   Orchestration in `sql/exec_drop.baga` + `sql/exec_alter.baga`.
   catalog drop/truncate/alter = core schema/data/index only.
5. **index → catalog и index → fts/vector. — FIXED (D5)**
   `ix_drop` = secondary only; FTS/HNSW drop in modal wipe modules;
   cascade in `exec_drop`. Residual: `index:catalog` (schema rewrite on
   secondary drop — legitimate catalog use) kept in grandfather until
   secondary defs move fully under catalog.

Допустими (надолу по диаграмата, не се пипат): модали → catalog/storage/txn,
graph → catalog/drop, api → sql/catalog, всички → core.

## Препоръки (подредени по цена/риск)

### D1. Гейт `scripts/deps.sh` — първо, преди всяка поправка
Скрипт по модела на `filesize.sh` (§9 т.5): whitelist на разрешените
междумодулни `import "../…"` ребра; fail при ново несписъчно ребро. Днес
компилаторът приема цикли мълчаливо — дисциплината трябва да се охранява
механично, иначе всяка следваща фаза ще добавя нови нагорни import-и.
Пуска се с тестовете. ~30 реда, нулев риск.

### D2. Извади `BoilaServer` от storage
`BoilaServer` (meta store + отворени бази + session_txn + plan cache + guc)
е сървърно/сесийно състояние, не физически layout. Възможности:
- (препоръчително) мести се в `api/` (или нов `server/` между sql и api);
  storage пази само `BoilaStore`/`databases` (meta + per-db stores).
- План кешът и txn-ът остават собственост на сървърния слой, който вече
  импортва и sql, и txn — надолу, законно.
Премахва ребрата storage→txn и storage→sql. Среден размер, нискорисков
(чисто местене на struct + конструктор).

### D3. TTL резолюцията излез от txn commit-а
`txn/mvcc.baga` не трябва да знае за `ts/retention`. Извършителят
(`sql/exec_dml*`) вече импорта catalog → познава ttl_sec на таблицата от
schema row-а. Варианти:
- (препоръчително) txn write записите носят `ttl_sec` като поле, попълнено
  от caller-а; commit flush само го подава на `lsm_put_ex`. TTL кешът
  (ttl_ids/ttl_secs, ред 217-243) се мести в exec_dml.
- Алтернатива: hook/callback, регистриран от ts при open — по-индиректно,
  без реална нужда в baga.
Премахва txn→ts. Малък, локален.

### D4. Споделените типове слезат от sql/ast в catalog/core
`BoilaAlter`, колоновите дескриптори и type code-ове, които каталогът
консумира, се дефинират в `catalog/` (или `core/`), а `sql/ast.baga` и
`parse_alter.baga` ги импортват оттам — надолу по йерархията. sql/ е над
catalog, така че това е естествената посока. Премахва catalog→sql.
Механичен.

### D5. Cascade wipe се вдига в sql/ executor-а (най-голямото)
Сега `catalog/drop.baga`, `catalog/truncate.baga`, `catalog/alter.baga` и
`index/ix_drop.baga` всеки сам викат fts/vec/graph/ts wipe-ове. Цел:
- catalog оперира само върху своите sys записи (name/schema/ttl) +
  data/index CF wipe през storage — нищо над себе си.
- Всяка модал изнася една `*_wipe_table(store, txn, tid)` /
  `*_wipe_col(...)` функция без да знае за останалите.
- `sql/exec_drop.baga` (който вече съществува и е над всички) става
  единственият оркестратор: вика последователно catalog metadata rewrite +
  fts wipe + vec wipe + graph wipe + ts/ttl cleanup.
- `index/ix_drop.baga` губи fts/vector/catalog-drop ребрата по същия начин.
Това връща §3 в сила („композицията — в executor-а") и прави добавянето на
бъдеща модал = един ред в оркестратора, не import-и в 4 чужди модула.
Най-много работа, но тук е истинската тежест на проблема.

### D6. Опресни ARCHITECTURE §10 — DONE
§3 диаграма + server/ слой; §10 карта = реалните директории; TTL metadata
в `catalog/ttl.baga` (ts/retention само sweep). deps.sh: 0 grandfather.

### D7. Превантивно делене (§9 т.1) — NOTED
`sql/exec_dual_parse.baga` е 397/400. Pratt primary↔expr↔CASE са взаимно
рекурсивни; baga textual import не позволява чисто разцепване без цикъл.
Дели се при следваща dual feature (или след forward-decl в езика). Gate-ът
остава ≤400.

## Какво НЕ се препоръчва

- Не се разцепва монолита на процеси/пакети — проблемът не е в „един
  бинарник", а в посоката на ребрата.
- Не се въвежда event bus / DI рамка заради cascade-ите — baga-идиомата е
  директни извиквания; оркестрация нагоре (D5) е достатъчна.
- Не се чака „голям рефакторинг наведнъж" — D1..D5 са независими и всяка
  оставя repo-то компилируемо и тестовете зелени.

## Приемен ред — COMPLETE (2026-08-10)

D1 ✓ → D2 ✓ → D3 ✓ → D4 ✓ → D5 ✓ → D6 ✓ · D7 noted (397/400, no clean split).
`scripts/deps.sh`: **0 grandfathered upward edges**.
