# app-product × светове — карта на стека

Този документ е практическата карта „къде световете (и новата версия на
езика) помагат реално“ за пакетите в `app-product/`. Всички те са git
submodule-и; промяна в пакет се commit-ва в неговия repo и после root-ът
вдига submodule pointer-а.

## Готово

| Пакет | Промяна | Защо |
|---|---|---|
| `boilaDB` | SQL/HTTP/PG stats + live conn в замразен свят (`wcache_add`) | горещият път не взима `gmu` за статистика/admission |
| `boilaDB` | plan/catalog cache (`m/sm/tm`) под per-db mutex (P46) | shared data lock позволяваше едновременни `map_set` от worker-и |
| `fmrbaga` | runtime gauges в замразен свят (`wcache_add`) | премахнат незаключен `Map` race в `/metrics` и drain gauges |
| `metbaga` | нов `WMet`/`wmet_*`: frozen world + `wcache` | shared registry за многонишкови сървъри; `met_*` остава request-local |
| `cloudbaga` | `CloudState.metrics` → `WMet` | `/metrics` и `http_requests_total` вече са безопасни при threaded serve |
| `std`/LLVM | `byte_chr`, `bytes_from_vec`, `vec_from_bytes`, късосъединени `&&`/`||` | отключва pure codec стека за LLVM и поправя guard сегфолти |

## Къде светове НЕ са нужни

| Пакет | Причина |
|---|---|
| `rocksbaga` | per-shard `LsmDB` е под собствен mutex; `dbs` entries са preinserted/boxed, `exps` е под `emu`; page cache е per-LsmDB |
| `raftbaga` | state-ът е локален за node loop-а; линеаризуемите четения минават през канала до leader |
| `txnbaga` | participant state е локален; координацията е през chan |
| `queuebaga` | дискът е source of truth; in-memory cache би сменил restart семантиката |
| `ormbaga` | pool слотовете са channel-owned; parallel Vec-овете се пишат на различни индекси |
| `oauthbaga` | PG режимът е stateless; in-memory store е serial dev режим |
| `chatbaga`, `kvbaga`, `wsbaga` | един poll/accept thread притежава state-а |
| pure codecs (`json`, `xml`, `zip`, `pdf`, `pb`, `img`, `jwt`, `glob`, `tpl`, `smtp`, …) | няма shared mutable state; ползата е LLVM, не светове |

## Оставащи кандидати (по приоритет)

1. **Raft/txn snapshot читатели** — frozen world на committed KV store за
   наблюдатели/read-only заявки. Изисква copy-on-commit или snapshot
   publish; не е drop-in и не е направено, за да не се компрометира
   линеаризуемостта.
2. **queuebaga status cache** — `wcache_set_str` за финални статуси/резултати
   с read-through към диска. Само ако latency-ът на `q_status` е проблем;
   кешът е process-local и не бива да е source of truth.
3. **rocksbaga frozen SST/bloom snapshot** — само ако shard mutex-ът стане
   bottleneck и се махне; голяма промяна, не сега.

## Правила, научени от стека

- **Frozen world + `wcache_add`** е правилният инструмент за броячи и gauges:
  атомарност без `gmu`, без `!Par` по handlers.
- **`wcache` не е за агрегати**: планове, каталози и `LsmDB` съдържат
  контейнери, не scalar-и. За тях или mutex (P46), или бъдещ `wrec`/`wmap`
  модел.
- **`met_*` (Vec) е request-local**, `wmet_*` е за shared registry. Не
  създавай свят на всеки `/metrics` request — `met_*` е по-евтин.
- **LLVM gate**: `tests/llvm_app_pure.sh` (в `make test-llvm`) пази pure
  app-product стека; `tests/llvm_worlds.sh` пази световете. Ако submodule-ите
  липсват, скриптовете skip-ват с ясно съобщение.

## Проверки

```bash
make test-llvm LLI=lli-19          # oracle + worlds + pure app stack
make test-llvm-rc LLI=lli-19       # RC вариант
./baga -I app-product tests/wmet_test.baga
```

За boilaDB: `boila_p5_test`, `boila_pool_test`, `boila_pgwire_test`,
`boila_chaos_test`, `boila_http_mux_test`; pool тестът е чист и под TSan.
