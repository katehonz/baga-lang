# Canonical product stack (base)

**Locked:** 2026-08-03 · **Updated package list:** 2026-08-15

**Language role:** Baga is an **educational systems language**. Packages here
are **ecosystem building blocks to prove the language** — not throwaway demos.
Long-horizon storage goal: a **RocksDB-like** engine (`rocksbaga`; was `lsmbaga`).

```
        apps/*   (each product is independent — own routes, models, schema)
           │
      ┌────▼────┐
      │ fmrbaga │   universal framework (router, JSON, auth, config, serve)
      └────┬────┘
 ┌─────────┼─────────┐
 ▼         ▼         ▼
httpdbaga  jwtbaga  ormbaga (+ pool)
                    (table ORM + migrations)
                         │
              ┌──────────┴──────────┐
              ▼                     ▼
           pgbaga              boilabaga
              │               (boila adapter)
              ▼                     │
           Postgres                 ▼
                              pgbaga → boilaDB :6575
```

| Layer | Path | Role |
|-------|------|------|
| **App** | `apps/*` | product code (actions, models, schema, routes) — **universal pattern** |
| Framework | `app-product/fmrbaga` | universal router/jsonx/deps/workers (no domain) |
| HTTP | `httpdbaga` | request/response |
| Auth | `jwtbaga` | HS256 |
| ORM | `ormbaga` | migrations, CRUD, pool, prepare |
| Driver | `pgbaga` | SCRAM, Simple + Extended Query |
| Adapter | `boilabaga` | connect + dialect defaults for boilaDB PG wire |

**Rules**

1. **Packages stay universal** — no product domain inside `app-product/*`.
2. **Apps stay independent** — each app owns routes, models, and migrations.
3. Prefer worker pool in production (`FMR_WORKERS` default 4; often 8).
4. Prefer parameterized ORM (`$1`) for user input.
5. Do not fork a second web framework without a strong reason.
6. Every package carries a `sandak.toml` (name == directory name) and builds
   with `sandak build`; imports use package names, never `../../` paths.

---

## Full package list (`app-product/`)

**44+ packages** (directories with `sandak.toml`; plus **boilaDB** server). Alphabetical.

### Web / API stack

| Package | Role |
|---------|------|
| **fmrbaga** | Baga web framework — router, JSON (`jsonx`), middleware, OpenAPI, config, serve, workers; **git submodule** → [bagalang/fmrbaga](https://github.com/bagalang/fmrbaga) |
| **httpdbaga** | HTTP/1.1 + HTTP/2 (h2c, HPACK) server library; **git submodule** → [bagalang/httpdbaga](https://github.com/bagalang/httpdbaga) |
| **jwtbaga** | JWT/JWS — HS256 sign/verify; RS256/ES256 verify; **git submodule** → [bagalang/jwtbaga](https://github.com/bagalang/jwtbaga) |
| **oauthbaga** | OAuth2 / OIDC-style flows, proxy, session cookie demo; **git submodule** → [bagalang/oauthbaga](https://github.com/bagalang/oauthbaga) |
| **ormbaga** | Universal table ORM + versioned migrations + pool (no app domain); **git submodule** → [bagalang/ormbaga](https://github.com/bagalang/ormbaga) |
| **pgbaga** | Native PostgreSQL wire client (SCRAM-SHA-256, Simple + Extended Query); **git submodule** → [bagalang/pgbaga](https://github.com/bagalang/pgbaga) |
| **boilaDB** | Multimodal SQL server — BoilaSQL, PG wire `:6575`, HTTP `:6570` ([docs](boilaDB/docs/README.md)); **git submodule** → [bagalang/boilaDB](https://github.com/bagalang/boilaDB) |
| **boilabaga** | Client adapter to boilaDB over PG wire (defaults :6575 + BoilaSQL dialect); **git submodule** → [bagalang/boilabaga](https://github.com/bagalang/boilabaga) |
| **querybaga** | URL query / form parse and encode; **git submodule** → [bagalang/querybaga](https://github.com/bagalang/querybaga) |
| **wsbaga** | WebSocket RFC 6455 — handshake, frames, echo server/client; **git submodule** → [bagalang/wsbaga](https://github.com/bagalang/wsbaga) |
| **chatbaga** | Multi-room WebSocket chat product (on wsbaga + poll); **git submodule** → [bagalang/chatbaga](https://github.com/bagalang/chatbaga) |

### RPC / protocols

| Package | Role |
|---------|------|
| **jsonrpcbaga** | JSON-RPC 2.0 over HTTP (single + batch, name dispatch); **git submodule** → [bagalang/jsonrpcbaga](https://github.com/bagalang/jsonrpcbaga) |
| **pbbaga** | Protocol Buffers wire codec + gRPC message framing; **git submodule** → [bagalang/pbbaga](https://github.com/bagalang/pbbaga) |
| **statusbaga** | gRPC status codes + Status (Go `codes`/`status` style); **git submodule** → [bagalang/statusbaga](https://github.com/bagalang/statusbaga) |
| **ctxbaga** | Context lite — deadlines, cancel, string values (Go `context`); **git submodule** → [bagalang/ctxbaga](https://github.com/bagalang/ctxbaga) |

### Storage / data plane

| Package | Role |
|---------|------|
| **rocksbaga** | Durable LSM-style KV (RocksDB-class path); flagship storage; **git submodule** → [bagalang/rocksbaga](https://github.com/bagalang/rocksbaga) |
| **lsmbaga** | **Deprecated** → re-exports / points at `rocksbaga`; **git submodule** → [bagalang/lsmbaga](https://github.com/bagalang/lsmbaga) |
| **kvbaga** | Redis-compatible RESP2 KV server (`Map` store, TTL); **git submodule** → [bagalang/kvbaga](https://github.com/bagalang/kvbaga) |
| **queuebaga** | Background job queue — disk payloads, worker pool over `chan`; **git submodule** → [bagalang/queuebaga](https://github.com/bagalang/queuebaga) |
| **raftbaga** | Raft fragment — election + single-entry log (3 in-process nodes); **git submodule** → [bagalang/raftbaga](https://github.com/bagalang/raftbaga) |
| **txnbaga** | 2PC coordinator + MVCC store (distributed transactions probe); **git submodule** → [bagalang/txnbaga](https://github.com/bagalang/txnbaga) |

### Cloud / ops

| Package | Role |
|---------|------|
| **cloudbaga** | 12-factor cloud demo — healthz/readyz/metrics, graceful shutdown; **git submodule** → [bagalang/cloudbaga](https://github.com/bagalang/cloudbaga) |
| **flagbaga** | Typed CLI flags over `arg()`; **git submodule** → [bagalang/flagbaga](https://github.com/bagalang/flagbaga) |
| **logbaga** | Structured JSON lines on stderr; **git submodule** → [bagalang/logbaga](https://github.com/bagalang/logbaga) |
| **metbaga** | Prometheus text metrics; **git submodule** → [bagalang/metbaga](https://github.com/bagalang/metbaga) |
| **otelbaga** | W3C Trace Context + OTLP/JSON export lite; **git submodule** → [bagalang/otelbaga](https://github.com/bagalang/otelbaga) |
| **relbaga** | Resilience — backoff, retry, circuit breaker, bulkhead |

### Text / documents / images / reports

| Package | Role |
|---------|------|
| **bufbaga** | String builder (push chunks, join once); **git submodule** → [bagalang/bufbaga](https://github.com/bagalang/bufbaga) |
| **csvbaga** | CSV parse/stringify (RFC 4180-ish); **git submodule** → [bagalang/csvbaga](https://github.com/bagalang/csvbaga) |
| **imgbaga** | Raster images — PNG/JPEG/GIF/QOI/ICO/TIFF/WebP (VP8+VP8L)/BMP/PNM (`image` crate); **git submodule** → [bagalang/imgbaga](https://github.com/bagalang/imgbaga) |
| **mdbaga** | Markdown parser / renderer; **git submodule** → [bagalang/mdbaga](https://github.com/bagalang/mdbaga) |
| **mdtbaga** | gRPC metadata (`metadata.MD` multimap); **git submodule** → [bagalang/mdtbaga](https://github.com/bagalang/mdtbaga) |
| **officebaga** | Office docs — DOCX/XLSX/ODT/ODS (+ legacy DOC/XLS probe); **git submodule** → [bagalang/officebaga](https://github.com/bagalang/officebaga) |
| **pdfbaga** | PDF writer with UTF-8/Cyrillic via embedded TTF; **git submodule** → [bagalang/pdfbaga](https://github.com/bagalang/pdfbaga) |
| **reportbaga** | Accounting reports — data/HTML → Excel · CSV · PDF · HTML; **git submodule** → [bagalang/reportbaga](https://github.com/bagalang/reportbaga) |
| **tplbaga** | HTML template engine (Mustache-ish: if, filters, escape); **git submodule** → [bagalang/tplbaga](https://github.com/bagalang/tplbaga) |
| **xmlbaga** | XML pull parser + writer (no DOM); **git submodule** → [bagalang/xmlbaga](https://github.com/bagalang/xmlbaga) |
| **zipbaga** | ZIP + DEFLATE/inflate + CRC-32 (used by officebaga); **git submodule** → [bagalang/zipbaga](https://github.com/bagalang/zipbaga) |

### Language / tooling utilities

| Package | Role |
|---------|------|
| **bagadecimal** | Fixed-precision decimal (money, VAT, rates; Postgres NUMERIC bridge); **git submodule** → [bagalang/bagadecimal](https://github.com/bagalang/bagadecimal) |
| **globbaga** | Simple glob matching (`*`, `?`); **git submodule** → [bagalang/globbaga](https://github.com/bagalang/globbaga) |
| **grebaga** | Grep-like line scanner (literal + mini patterns); **git submodule** → [bagalang/grebaga](https://github.com/bagalang/grebaga) |
| **pathbaga** | Path helpers (join, basename, dirname, ext, stem); **git submodule** → [bagalang/pathbaga](https://github.com/bagalang/pathbaga) |
| **testbaga** | Minimal test assertions (`assert_true`, `assert_eq_*`); **git submodule** → [bagalang/testbaga](https://github.com/bagalang/testbaga) |
| **uuidbaga** | UUID v4 (RFC 4122); **git submodule** → [bagalang/uuidbaga](https://github.com/bagalang/uuidbaga) |

### Runtime / plugins

| Package | Role |
|---------|------|
| **wasmtimebaga** | Wasmtime host embedding (C API + shim; wasmtime-go model) — run wasm in-process; **git submodule** → [bagalang/wasmtimebaga](https://github.com/bagalang/wasmtimebaga) |

### Flat inventory (names only)

```
bagadecimal  boilabaga   bufbaga     chatbaga    cloudbaga   csvbaga
ctxbaga      flagbaga    fmrbaga     globbaga    grebaga     httpdbaga
imgbaga      jsonrpcbaga jwtbaga     kvbaga      logbaga     lsmbaga†
mdbaga       mdtbaga     metbaga     oauthbaga   officebaga  ormbaga
otelbaga     pathbaga    pbbaga      pdfbaga     pgbaga      querybaga
queuebaga    raftbaga    relbaga     reportbaga  rocksbaga   statusbaga
testbaga     tplbaga     txnbaga     uuidbaga    wasmtimebaga wsbaga
xmlbaga      zipbaga
```

† `lsmbaga` — deprecated alias for `rocksbaga`. Also: **boilaDB** (server).
