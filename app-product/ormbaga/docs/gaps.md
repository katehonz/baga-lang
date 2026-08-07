# ormbaga — gaps

## G1 — No `Vec<struct>` for migration registry

**Symptom.** `MigrationSet` is four parallel vectors (`versions`, `names`, `ups`, `downs`).

**Workaround.** `migrate_add` pushes to all four.

**Verdict.** Language limit; same pattern as httpdbaga headers.

## G2 — By-value `OrmDb` threading

**Symptom.** Every call returns updated `db`; forget rebind → stale connection state.

**Workaround.** `db = orm_db_q(r)` / `orm_db_exec(r)` / `migrate_db(r)` —
always rebind (documented).

**Verdict.** Same as pgbaga G2.

## G2b — OrmQuery / OrmExec L3 — **shipped (B1)**

**Shipped.** `OrmExec` / `OrmQuery` / `OrmCount` / `MigrateResult` are sum
enums; helpers `orm_ok`, `orm_ok_q`, `orm_nrows`, `migrate_is_ok`, …
Apps and tests use helpers (no `.ok` fields on the enum).

## G3 — ~~No parameterized queries~~ — **CLOSED**

**Was.** ORM built SQL strings only.

**Now.** `orm_find_*` / `orm_where_eq_*` / `orm_insert_*` / update / delete use
pgbaga Extended Query (`$1`). Legacy `*_lit` paths remain for trusted SQL.

**Verdict.** Closed.

## G4 — No reflection / model macros

**Symptom.** Cannot map `struct User { … }` ↔ row automatically.

**Workaround.** Dynamic cells + `orm_cell_by(name)`.

**Verdict.** Acceptable for P0; codegen tool could come later (Prisma-like) as external binary.

## G5 — Migrations only embedded

**Symptom.** No `migrations/*.sql` directory walker in std.

**Workaround.** `schema.baga` registry; can `read_file` fixed paths manually.

**Verdict.** Add file-based loader when `std/os` has readdir (or list paths in set).

## G6 — Multi-line string SQL awkward

**Symptom.** Baga string literals are single-line (use `\n` or one long line).

**Workaround.** Single-line DDL in registry.

**Severity.** Low.

## G7 — ~~`RETURNING` not used for insert→id~~ — **CLOSED**

**Now.** `orm_insert_returning_strs` / `orm_insert_returning_lits` +
`sql_insert_returning_p`. Covered in `tests/orm_test.baga`.

## Closed / fine

- goose history table + transactional up/down verified in `tests/orm_test.baga`.
- Quote escaping for `O'Hara` / `a'b` verified.
- ON DELETE CASCADE through ORM delete verified.

## Closed recently (2026-08-07)

- `orm_all_page` / `orm_where_eq_*_page` + `sql_page` / `sql_offset`.
- `orm_ping` for readiness checks.
- Package layered under `sql/`, `session/`, `migrate/`, `pool/`.
