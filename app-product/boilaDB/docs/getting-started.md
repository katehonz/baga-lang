# Getting started

boilaDB is a single-process multimodal database. Persistence is
[rocksbaga](../../rocksbaga/README.md) (LSM, WAL, SST). The public
surface is **BoilaSQL** — a documented PostgreSQL subset — over HTTP
(`:6570`) and PostgreSQL wire protocol v3 (`:6575`).

Encoding is **UTF-8 only**. Geo/GPS is out of scope.

## Prerequisites

From the Baga repo root:

```bash
make                          # C bootstrap compiler → ./baga
# optional:
make sandak                   # package manager (sandak.toml)
```

The server is Baga source. You do not need a separate install: compile
and run the entry points under `app-product/boilaDB/tools/`.

## Start the HTTP server

```bash
BOILA_PATH=/tmp/baga_boila BOILA_PORT=6570 BOILA_WORKERS=4 \
  ./baga -I . -I app-product app-product/boilaDB/tools/serve.baga
```

Defaults if env is unset: path `/tmp/baga_boila`, port **6570**, 4
shards, 4 workers, 64 max connections. The first open creates the
default database `boila` (same role as `postgres` in PostgreSQL).

```bash
curl -s localhost:6570/health
# {"status":"ok","open_databases":1,"max_db":64,"live_conn":0,
#  "mode":"mt-pool","workers":4,"version":"0.1.0"}

curl -s -X POST localhost:6570/sql \
  --data "SELECT 1 AS n, current_database()"
```

## Start the PostgreSQL wire server

```bash
BOILA_PATH=/tmp/baga_boila BOILA_PGPORT=6575 BOILA_WORKERS=4 \
  ./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga
```

Use any PostgreSQL client. The port is **6575** on purpose so a local
Postgres on 5432 is never inherited.

```bash
psql "host=127.0.0.1 port=6575 user=boila dbname=boila"
# empty password = trust, until you create users or set BOILA_TOKEN
```

From Baga:

```baga
import "boilabaga/adapter.baga"

let c = boila_connect_env()?     // 127.0.0.1:6575 / boila / boila
```

See [pgwire.md](pgwire.md) and [`boilabaga`](../../boilabaga/README.md).

HTTP and PG can share the same `BOILA_PATH`. Run them as two processes
only if you accept two independent server objects; for one process, pick
one listener (or start both only in separate data dirs).

## First table

Every table needs a `PRIMARY KEY` (LSM-friendly point/range path).

```sql
CREATE TABLE users (
  id    BIGINT,
  name  TEXT,
  email TEXT,
  PRIMARY KEY (id)
);

CREATE INDEX users_email ON users (email);

INSERT INTO users (id, name, email) VALUES
  (1, 'Ана', 'ana@example.com'),
  (2, 'Борис', 'boris@example.com')
RETURNING id, name;

SELECT id, name FROM users WHERE email = 'ana@example.com';
```

Identifiers are **bare** (`users`, not `"users"`). String literals are
UTF-8; Cyrillic is first-class.

## Interactive shell

No network — opens the store and runs SQL in-process:

```bash
BOILA_PATH=/tmp/baga_boila \
  ./baga -I . -I app-product app-product/boilaDB/tools/shell.baga
```

```
boila[boila]> CREATE TABLE t (id BIGINT, PRIMARY KEY (id));
OK (table id 1)
boila[boila]> INSERT INTO t (id) VALUES (1), (2);
OK (засегнати редове: 2)
boila[boila]> SELECT * FROM t;
cols: id
  …
(2 ред(а))
boila[boila]> \q
```

Empty line or `\q` exits. `USE otherdb` changes the prompt.

## sandak

```toml
# app-product/boilaDB/sandak.toml
[package]
name = "boilaDB"
version = "0.1.0"
entry = "tools/serve.baga"

[dependencies]
std = { path = "../../std" }
rocksbaga = { path = "../rocksbaga" }
httpdbaga = { path = "../httpdbaga" }
metbaga = { path = "../metbaga" }
```

```bash
cd app-product/boilaDB
sandak build    # typecheck / compile the HTTP entry
```

## Tests

```bash
./scripts/baga-test                       # discovers tests/boila_*_test.baga
bash app-product/boilaDB/scripts/filesize.sh
bash app-product/boilaDB/scripts/deps.sh
```

ORM against a live `serve_pg`: `tests/orm_boila_test.baga` (36 checks).

## Next

- [Configuration](configuration.md) — shards, workers, budget, TTL sweep
- [BoilaSQL](sql.md) — the dialect, not “almost Postgres”
- [Modalities](modalities.md) — FTS, vectors, time-series, graph
