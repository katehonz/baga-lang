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
| **jwtbaga** | JWT/JWS — HS256 sign/verify; RS256/ES256 verify |
| **oauthbaga** | OAuth2 / OIDC-style flows, proxy, session cookie demo |
| **ormbaga** | Universal table ORM + versioned migrations + pool (no app domain) |
| **pgbaga** | Native PostgreSQL wire client (SCRAM-SHA-256, Simple + Extended Query) |
| **boilaDB** | Multimodal SQL server — BoilaSQL, PG wire `:6575`, HTTP `:6570` ([docs](boilaDB/docs/README.md)); **git submodule** → [bagalang/boilaDB](https://github.com/bagalang/boilaDB) |
| **boilabaga** | Client adapter to boilaDB over PG wire (defaults :6575 + BoilaSQL dialect); **git submodule** → [bagalang/boilabaga](https://github.com/bagalang/boilabaga) |
| **querybaga** | URL query / form parse and encode |
| **wsbaga** | WebSocket RFC 6455 — handshake, frames, echo server/client |
| **chatbaga** | Multi-room WebSocket chat product (on wsbaga + poll); **git submodule** → [bagalang/chatbaga](https://github.com/bagalang/chatbaga) |

### RPC / protocols

| Package | Role |
|---------|------|
| **jsonrpcbaga** | JSON-RPC 2.0 over HTTP (single + batch, name dispatch) |
| **pbbaga** | Protocol Buffers wire codec + gRPC message framing |
| **statusbaga** | gRPC status codes + Status (Go `codes`/`status` style) |
| **ctxbaga** | Context lite — deadlines, cancel, string values (Go `context`); **git submodule** → [bagalang/ctxbaga](https://github.com/bagalang/ctxbaga) |

### Storage / data plane

| Package | Role |
|---------|------|
| **rocksbaga** | Durable LSM-style KV (RocksDB-class path); flagship storage; **git submodule** → [bagalang/rocksbaga](https://github.com/bagalang/rocksbaga) |
| **lsmbaga** | **Deprecated** → re-exports / points at `rocksbaga` |
| **kvbaga** | Redis-compatible RESP2 KV server (`Map` store, TTL) |
| **queuebaga** | Background job queue — disk payloads, worker pool over `chan` |
| **raftbaga** | Raft fragment — election + single-entry log (3 in-process nodes) |
| **txnbaga** | 2PC coordinator + MVCC store (distributed transactions probe) |

### Cloud / ops

| Package | Role |
|---------|------|
| **cloudbaga** | 12-factor cloud demo — healthz/readyz/metrics, graceful shutdown; **git submodule** → [bagalang/cloudbaga](https://github.com/bagalang/cloudbaga) |
| **flagbaga** | Typed CLI flags over `arg()`; **git submodule** → [bagalang/flagbaga](https://github.com/bagalang/flagbaga) |
| **logbaga** | Structured JSON lines on stderr |
| **metbaga** | Prometheus text metrics |
| **otelbaga** | W3C Trace Context + OTLP/JSON export lite |
| **relbaga** | Resilience — backoff, retry, circuit breaker, bulkhead |

### Text / documents / images / reports

| Package | Role |
|---------|------|
| **bufbaga** | String builder (push chunks, join once); **git submodule** → [bagalang/bufbaga](https://github.com/bagalang/bufbaga) |
| **csvbaga** | CSV parse/stringify (RFC 4180-ish); **git submodule** → [bagalang/csvbaga](https://github.com/bagalang/csvbaga) |
| **imgbaga** | Raster images — PNG/JPEG/GIF/QOI/ICO/TIFF/WebP (VP8+VP8L)/BMP/PNM (`image` crate); **git submodule** → [bagalang/imgbaga](https://github.com/bagalang/imgbaga) |
| **mdbaga** | Markdown parser / renderer |
| **mdtbaga** | Markdown table helpers |
| **officebaga** | Office docs — DOCX/XLSX/ODT/ODS (+ legacy DOC/XLS probe) |
| **pdfbaga** | PDF writer with UTF-8/Cyrillic via embedded TTF |
| **reportbaga** | Accounting reports — data/HTML → Excel · CSV · PDF · HTML |
| **tplbaga** | HTML template engine (Mustache-ish: if, filters, escape) |
| **xmlbaga** | XML pull parser + writer (no DOM) |
| **zipbaga** | ZIP + DEFLATE/inflate + CRC-32 (used by officebaga) |

### Language / tooling utilities

| Package | Role |
|---------|------|
| **bagadecimal** | Fixed-precision decimal (money, VAT, rates; Postgres NUMERIC bridge); **git submodule** → [bagalang/bagadecimal](https://github.com/bagalang/bagadecimal) |
| **globbaga** | Simple glob matching (`*`, `?`); **git submodule** → [bagalang/globbaga](https://github.com/bagalang/globbaga) |
| **grebaga** | Grep-like line scanner (literal + mini patterns); **git submodule** → [bagalang/grebaga](https://github.com/bagalang/grebaga) |
| **pathbaga** | Path helpers (join, basename, dirname, ext, stem) |
| **testbaga** | Minimal test assertions (`assert_true`, `assert_eq_*`) |
| **uuidbaga** | UUID v4 (RFC 4122) |

### Runtime / plugins

| Package | Role |
|---------|------|
| **wasmtimebaga** | Wasmtime host embedding (C API + shim; wasmtime-go model) — run wasm in-process |

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
