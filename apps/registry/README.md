# apps/registry — пакетен registry за sandak

JSON/HTTP registry за Baga пакети (apps-roadmap №2, втора половина):
индекс на пакети + версии, който `sandak search` / `sandak publish` говорят.
Стекът е същият като apps/api: **fmrbaga** (router/JSON) + **ormbaga**
(Postgres, migrations) + **pgbaga**.

## API

### HTTP JSON (fmrbaga)

| Метод | Път | Резултат |
|-------|-----|----------|
| GET | `/health` | `{status, service}` |
| GET | `/ready` `/readyz` | DB ping; **503** while shutting down (SIGTERM) |
| GET | `/metrics` | Prometheus text (`process_up`, `fmr_ready`, `registry_info`) |
| GET | `/v1/packages?q=term` | `{items:[…], count}` — ILIKE в name/description |
| GET | `/v1/packages/{name}` | пакет + `versions[]` |
| POST | `/v1/packages` | publish: upsert пакет + нов version; 409 при дубъл версия, 422 при липсващи полета |

Publish body: `{name*, version*, description, source_kind, source_url, rev, subdir}`.
MVP без auth (локален/dev registry) — токени са P1 (виж PLAN-а на езика).

### gRPC (B3 dual protocol — same port)

`Content-Type: application/grpc` + POST method path:

| RPC | Path | Notes |
|-----|------|--------|
| GetPackage | `/regbaga.Registry/GetPackage` | `name` → Package (NOT_FOUND=5) |
| ListPackages | `/regbaga.Registry/ListPackages` | optional `q` ILIKE → PackageList |

PB fields (hand-encoded, no protoc): Package `{name, description, latest_version, source_kind, source_url}`; List `{repeated Package items, count}`.

```baga
let frame = grpc_encode(reg_pb_get_req_encode("lsmbaga"))
let r = grpc_call_unary("localhost", 8090, "/regbaga.Registry/GetPackage", frame, 5, mdt_new(), 0)?
```

Test: `tests/registry_grpc_test.baga`.

## Пускане

```bash
# миграции + сървър (по подразбиране :8080, DB baga_orm)
PORT=8090 PGDATABASE=baga_registry ./baga apps/registry/start.baga

# или билднат бинарник
(cd apps/registry && sandak build)
PORT=8090 PGDATABASE=baga_registry apps/registry/target/registry
```

Тестът `tests/registry_test.baga` сам създава `baga_registry` DB, пуска
сървъра в `go_bg` worker и го кара през **std HTTP клиента** — първият
end-to-end продуктов тест на `http_client` (в `make test`).

## Клиентът (sandak)

```bash
sandak search [term]                                   # SANDAK_REGISTRY, default :8090
sandak publish --git URL [--rev R] [--subdir S]        # чете sandak.toml в cwd
sandak publish --path P
```

`src/sandak_registry.baga` е Baga програма (std HTTP клиент + std/json);
`src/sandak.c` я exec-ва през `baga` с аргументите на командата.

## Честни граници

- Няма auth (MVP); няма tarball качване — registry-то е **индекс** къде живее
  пакетът (git url/rev/subdir или path), не огледало на съдържание.
- `sandak` още не резолвира `registry = …` зависимости от манифест —
  следващата стъпка преди истинско `sandak add name@version`.
- Портовете в тестовете са фиксирани (8090/8091).
