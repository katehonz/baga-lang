# Sandak — пакетна система за Baga (дизайн)

**Дата:** 2026-08-03 · **Статус:** одобрен дизайн · **Подход:** A (Cargo-минимален)

## Проблем

Днес зависимостите в Baga са текстови `import` с твърдо кодирани относителни
пътища (`../../app-product/fmrbaga/app.baga`). Няма манифест, версии, lock,
централен build — и няма начин един контейнер да изтегли приложение и
зависимостите му от GitHub и да го компилира сам. Работата по продукта
(`apps/api` + `app-product/*`) не може да продължи без пакетна система.

## Цел

Cargo-подобен UX (manifest, lock, fetch, build) с **минимална промяна в
компилатора** (само `-I` search path). Крайният сценарий: потребителят
редактира два параметъра в `docker-compose.yml` и контейнерът сам тегли
toolchain + приложение + зависимости от GitHub и билдва.

Изрично **извън обхвата** (YAGNI за v1): мрежов регистър (crates.io стил),
семантично версиониране с резолвър на версии, модулна система с namespaces
(ортогонална, може после), публикуване на пакети, `sandak new`.

## Терминология и имена

- **`sandak`** — инструментът (сандъкът държи crates). Отделен C бинарник,
  `src/sandak.c`, сглобява се от същия коренски Makefile. Без външни
  зависимости (само libc + `git` + `gcc` като външни процеси).
- **`sandak.toml`** — манифест на пакет.
- **`sandak.lock`** — pinned резолюция, комитва се в git.
- **`.sandak/cache/`** — локален кеш на git зависимости (в .gitignore).

## Пакетен модел

Всеки пакет е директория със `sandak.toml` в корена. Пакети в монорепото:

| Пакет | Път | Вид |
|-------|-----|-----|
| `std` | `std/` | библиотека (подпапките са част от пакета) |
| `httpdbaga`, `jwtbaga`, `pgbaga`, `ormbaga`, `fmrbaga` | `app-product/*` | библиотеки |
| `api` | `apps/api` | приложение (entry с main) |

Библиотечен пакет има `entry` библиотечен корен и се проверява с `baga --lib`.
Приложение има `entry` с `main` и се компилира до бинарник.

## Манифест: `sandak.toml`

```toml
[package]
name = "fmrbaga"
version = "0.1.0"
entry = "handlers.baga"        # за приложение: "start.baga"

[dependencies]
httpdbaga = { path = "../httpdbaga" }
ormbaga   = { path = "../ormbaga" }
jwtbaga   = { git = "https://github.com/user/jwtbaga", rev = "a1b2c3d" }
std       = { path = "../../std" }
```

- Зависимост: `{ path = "..." }` или `{ git = "...", rev|tag|branch = "..." }`.
- Поле `subdir = "app-product/fmrbaga"` в git зависимост — пакетът е поддиректория
  на клонираното repo (monorepo случай; задължително за Docker сценария v1).
- Парсер: мини-TOML (~150 реда C) — само `[table]`, `key = "value"`, inline
  tables `{ k = v, ... }`, коментари `#`. Не е общ TOML парсер.

## Резолюция на импорти

Синтаксисът на `import "..."` не се променя. Към компилатора се добавя
повтаряем флаг `-I <dir>` (и съответно `baga` приема множество `-I`).

Към search path се добавя **родителската директория на корена на всяка
зависимост** — така импортът винаги включва името на пакета
(`import "fmrbaga/app.baga"`, `import "std/str/str.baga"`), което елиминира
колизии между пакети. Изискване: името на директорията на пакета съвпада с
`name` от манифеста (проверява се при fetch; за git + `subdir` — името на
subdir-а).

Ред на търсене за `import "fmrbaga/app.baga"`:

1. Релативно на директорията на текущия файл (сегашно поведение —
   intra-package импортите работят без промяна).
2. В родителската директория на корена на всяка зависимост, по реда от
   манифеста (подадени като `-I`).
3. Грешка „не мога да намеря import", ако никъде го няма.

Първото съвпадение печели (редът е детерминистичен). Благодарение на
изискването импортът да включва името на пакета, двусмислие между пакети е
възможно само при дублирано име на пакет в графа — това е грешка при
резолюция, още преди компилация.

`std` не е special-cased: обикновена зависимост, чийто корен е `std/`, затова
импортите изглеждат `import "std/str/str.baga"` — същият вид като днес, само
че без `../../`.

## Команди на sandak (v1)

- `sandak fetch` — рекурсивна резолюция на зависимостите (DFS с cycle
  detection), клониране на липсващите git deps в `.sandak/cache/<name>-<rev>/`
  (shallow clone + checkout на pinned rev), запис/обновяване на `sandak.lock`.
- `sandak build` — fetch + компилация:
  - приложение: `baga -I dep1 -I dep2 ... --emit-c entry.baga` →
    `gcc -O2 -o target/<name> ... -lm -pthread`
  - библиотека: `baga -I ... --lib entry.baga` (проверка без main)
- `sandak run` — build + стартиране на бинарника.
- `--locked` — грешка вместо re-resolve, ако `sandak.toml` и `sandak.lock`
  се разминават (за CI/Docker).

## Lock файл: `sandak.lock`

Същият мини-TOML; един блок на пакет:

```toml
[[package]]
name = "jwtbaga"
version = "0.1.0"
source = "git+https://github.com/user/jwtbaga"
rev = "a1b2c3d..."

[[package]]
name = "fmrbaga"
version = "0.1.0"
source = "path+../fmrbaga"   # path deps се записват за completeness
```

Съдържа пълния транзитивен граф, сортиран по име за стабилни diff-ове.

## Makefile

- Нова цел `sandak` → бинарник `sandak` (gcc, същите CFLAGS).
- `make test` продължава да работи: импортите в `app-product/`, `apps/` и
  `tests/` се мигрират от `../../...` към пакетни (`import "fmrbaga/app.baga"`),
  а тестовите извиквания минават през `sandak build`/`baga -I ...`.
- Коренският Makefile си остава за компилатора; пакетите се билдват от sandak.

## Docker сценарий (крайната цел)

В repo-то: `Dockerfile` (multi-stage) + `docker-compose.yml` с параметри.

```dockerfile
# Stage 1: toolchain
FROM debian:bookworm-slim AS toolchain
RUN apt-get update && apt-get install -y gcc make git && rm -rf /var/lib/apt/lists/*
ARG BAGA_REPO=https://github.com/user/baga
ARG BAGA_REF=main
RUN git clone --depth 1 --branch $BAGA_REF $BAGA_REPO /baga \
 && make -C /baga sandak \
 && cp /baga/baga /baga/sandak /usr/local/bin/

# Stage 2: app build
FROM toolchain AS build
ARG APP_REPO
ARG APP_REF=main
RUN git clone --depth 1 --branch $APP_REF $APP_REPO /app
WORKDIR /app
RUN sandak build --locked

# Stage 3: slim runtime
FROM debian:bookworm-slim
COPY --from=build /app/target/api /usr/local/bin/api
CMD ["api"]
```

```yaml
# docker-compose.yml
services:
  api:
    build:
      context: .
      args:
        APP_REPO: https://github.com/user/my-baga-app
        APP_REF: v0.1.0
    ports: ["8080:8080"]
```

Потребителят редактира само `APP_REPO`/`APP_REF` → `docker compose up --build`.

## Обработка на грешки

Всички съобщения на български, в стила на компилатора:

- липсващ манифест / липсващ `entry` файл;
- цикъл в зависимостите (изброен пътят на цикъла);
- git грешка (мрежа, несъществуващ rev) — предадена човешки;
- `--locked` разминаване — кое поле се различава;
- дублирано име на пакет в графа на зависимостите — изброени са двата източника.

## Тестване

1. **Unit:** shell тестове в `tests/sandak/` — парсер на манифест (валиден/
   невалиден), резолвър (path deps, цикъл, липсващ пакет), lock round-trip.
2. **Интеграция:** `sandak build` в `apps/api` + съществуващият
   `tests/api_test.baga` минава; `sandak build` на всяка `app-product/*`
   библиотека (`--lib`).
3. **Регресия:** целият `make test` остава зелен след миграцията на импортите.
4. **Docker smoke:** локален `docker build` (без push), проверка че бинарникът
   в runtime образа тръгва (`--help` или healthcheck).

## Известни ограничения (съзнателни)

- Глобалното namespace остава — колизия на имена между пакети е възможна;
  решава се с naming конвенция (префикси `fmr_`, `orm_`, както досега).
- Една версия на пакет в графа — няма резолвър на версии.
- Git deps изискват `git` в PATH; няма fallback към tarball.
