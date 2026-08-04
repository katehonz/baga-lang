CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm -pthread

# import search path за пакетите в монорепото (sandak го изчислява автоматично;
# за ръчни ./baga извиквания в тестовете: repo root за std/, app-product/ за *baga)
BAGAIFLAGS := -I . -I app-product

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c src/verify.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test test-llvm llvm self par-rt

all: $(BIN)

sandak: src/sandak.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: docker
docker: $(BIN) sandak
	@bash tests/sandak/docker_smoke.sh

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c include/baga.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Shared !Par runtime for LLVM/lli (go/join/chan)
PAR_SO := lib/libbaga_par.so
par-rt: $(PAR_SO)
$(PAR_SO): src/baga_par_rt.c
	@mkdir -p lib
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -pthread

# LLVM build (optional)
LLVM_CONFIG ?= llvm-config-14
LLVM_CFLAGS := $(shell $(LLVM_CONFIG) --cflags 2>/dev/null) -DBAGA_LLVM
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis target 2>/dev/null) $(LDFLAGS)
LLVM_SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c src/verify.c src/codegen_llvm.c
LLVM_OBJS := $(LLVM_SRCS:.c=.llvm.o)
LLVM_BIN := baga-llvm

llvm: $(LLVM_BIN) $(PAR_SO)

$(LLVM_BIN): $(LLVM_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LLVM_LDFLAGS)

src/%.llvm.o: src/%.c include/baga.h
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN) $(LLVM_OBJS) $(LLVM_BIN) $(PAR_SO)

test-llvm: $(BIN) $(LLVM_BIN) $(PAR_SO)
	@./tests/llvm_oracle.sh

# Self-hosting bootstrap: инвариантът е fixed point — self компилаторът
# възпроизвежда себе си (baga2 == baga3 като компилатори). baga (C bootstrap)
# и baga2 (self) са различни компилатора с различен codegen, затова сравняваме
# изхода на baga2 (baga_self3.c) с изхода на baga3 (baga_self4.c).
self: $(BIN)
	@echo "=== self-hosting bootstrap ==="
	@./$(BIN) $(BAGAIFLAGS) --emit-c self/compiler.baga > /tmp/baga_self2.c
	@gcc $(CFLAGS) -o /tmp/baga2 /tmp/baga_self2.c $(LDFLAGS)
	@/tmp/baga2 self/compiler.baga > /tmp/baga_self3.c
	@gcc $(CFLAGS) -o /tmp/baga3 /tmp/baga_self3.c $(LDFLAGS)
	@/tmp/baga3 self/compiler.baga > /tmp/baga_self4.c
	@if diff -q /tmp/baga_self3.c /tmp/baga_self4.c > /dev/null; then \
		echo "OK: baga2 == baga3 (fixed point — self компилаторът се възпроизвежда) ⚔️"; \
	else \
		echo "FAIL: baga2 != baga3"; diff /tmp/baga_self3.c /tmp/baga_self4.c | head -20; exit 1; \
	fi

test: $(BIN) sandak
	@echo "=== здравей ==="
	./$(BIN) $(BAGAIFLAGS) examples/zdravei.baga
	@echo "=== факториел ==="
	./$(BIN) $(BAGAIFLAGS) examples/faktorial.baga
	@echo "=== фибоначи ==="
	./$(BIN) $(BAGAIFLAGS) examples/fib.baga
	@echo "=== типове ==="
	./$(BIN) $(BAGAIFLAGS) examples/types.baga
	@echo "=== struct ==="
	./$(BIN) $(BAGAIFLAGS) examples/tochka.baga
	@echo "=== match ==="
	./$(BIN) $(BAGAIFLAGS) examples/match.baga
	@echo "=== effects ==="
	./$(BIN) $(BAGAIFLAGS) examples/effects.baga
	@echo "=== spec ==="
	./$(BIN) $(BAGAIFLAGS) examples/spec.baga
	@./$(BIN) $(BAGAIFLAGS) examples/spec_ensures.baga > /dev/null
	@echo "=== spec_ensures_fail (очакваме runtime грешка) ==="
	@./$(BIN) $(BAGAIFLAGS) examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: ensures гаранцията е хваната" \
		|| { echo "FAIL: ensures не е хваната"; exit 1; }
	@echo "=== spec_requires_fail (очакваме runtime грешка) ==="
	@./$(BIN) $(BAGAIFLAGS) examples/spec_requires_fail.baga 2>&1 | grep -q "requires #1 нарушено" \
		&& echo "OK: requires предусловието е хванато" \
		|| { echo "FAIL: requires не е хванато"; exit 1; }
	@echo "=== vec_typed (очакваме compile грешка) ==="
	@./$(BIN) $(BAGAIFLAGS) examples/vec_typed.baga 2>&1 | grep -q "елемент от тип str, но векторът е Vec<i64>" \
		&& echo "OK: Vec<T> хвана смесването" \
		|| { echo "FAIL: Vec<T> не хвана смесването"; exit 1; }
	@echo "=== arg_type_bad (очакваме compile грешка) ==="
	@./$(BIN) $(BAGAIFLAGS) examples/arg_type_bad.baga 2>&1 | grep -q "аргумент #1 е от тип str, но параметърът е i64" \
		&& echo "OK: проверката на аргументите хвана грешния тип" \
		|| { echo "FAIL: проверката на аргументите не хвана грешния тип"; exit 1; }
	@echo "=== vec_ann (Vec<T> анотации) ==="
	./$(BIN) $(BAGAIFLAGS) examples/vec_ann.baga
	@echo "=== emit-c cleanup (регресия: double-free при += desugar) ==="
	@./$(BIN) $(BAGAIFLAGS) --emit-c examples/vec_ann.baga > /dev/null \
		&& echo "OK: --emit-c не гърми върху += desugar" \
		|| { echo "FAIL: --emit-c"; exit 1; }
	@echo "=== bitwise ==="
	@./$(BIN) $(BAGAIFLAGS) examples/bitwise.baga > /tmp/baga_bitwise_out.txt
	@printf "2\n7\n5\n16\n16\n24\n9\n4\n16777215\n" | diff - /tmp/baga_bitwise_out.txt > /dev/null \
		&& echo "OK: побитови оператори" \
		|| { echo "FAIL: побитови оператори"; exit 1; }
	@echo "=== import ==="
	@./$(BIN) $(BAGAIFLAGS) tests/import_main.baga > /tmp/baga_import_out.txt
	@printf "49\n21\n" | diff - /tmp/baga_import_out.txt > /dev/null \
		&& echo "OK: import + include guard" \
		|| { echo "FAIL: import"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/import_cycle_a.baga 2>&1 | grep -q "цикличен import" \
		&& echo "OK: import цикълът е хванат" \
		|| { echo "FAIL: import цикълът не е хванат"; exit 1; }
	@echo "=== -I include path ==="
	@cp tests/i_flag/main.baga /tmp/baga_i_flag_main.baga
	@./$(BIN) $(BAGAIFLAGS) -I tests/i_flag /tmp/baga_i_flag_main.baga > /tmp/baga_i_flag_out.txt \
		&& test "$$(cat /tmp/baga_i_flag_out.txt)" = "42" \
		&& echo "OK: -I include path резолюция" \
		|| { echo "FAIL: -I include path"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) -Itests/i_flag /tmp/baga_i_flag_main.baga > /dev/null \
		&& echo "OK: -I<dir> слепен вариант" \
		|| { echo "FAIL: -I<dir> слепен вариант"; exit 1; }
	@echo "=== sandak (пакетен мениджър) ==="
	@SANDAK=$(CURDIR)/sandak BAGA=$(CURDIR)/baga bash tests/sandak/run_tests.sh
	@echo "=== sandak build на repo пакетите ==="
	@for p in httpdbaga jwtbaga pgbaga ormbaga fmrbaga kvbaga wsbaga chatbaga mdbaga testbaga grebaga queuebaga jsonrpcbaga tplbaga oauthbaga; do \
		(cd app-product/$$p && BAGA=$(CURDIR)/$(BIN) $(CURDIR)/sandak build > /dev/null) \
			&& echo "OK: sandak build $$p" \
			|| { echo "FAIL: sandak build $$p"; exit 1; }; \
	done
	@(cd apps/api && BAGA=$(CURDIR)/$(BIN) $(CURDIR)/sandak build > /dev/null) && test -x apps/api/target/api \
		&& echo "OK: sandak build apps/api (bin)" \
		|| { echo "FAIL: sandak build apps/api"; exit 1; }
	@echo "=== string interpolation ==="
	@./$(BIN) $(BAGAIFLAGS) examples/interp.baga > /tmp/baga_interp_out.txt
	@printf 'name=baga n=42 ok=true expr=84\ndollar=$$ braces={ } neg=-7\n' | diff - /tmp/baga_interp_out.txt > /dev/null \
		&& echo 'OK: $${expr} интерполация (str/i64/bool/call)' \
		|| { echo "FAIL: интерполация"; cat /tmp/baga_interp_out.txt; exit 1; }
	@echo "=== bytes type ==="
	@./$(BIN) $(BAGAIFLAGS) examples/bytes.baga > /tmp/baga_bytes_out.txt
	@printf 'len=4\nat0=222\nhex=deadbeef\nroundtrip=hi\ndec_hex=cafe\ncat_hex=deadbeef00ff\nslice_hex=adbe\n' | diff - /tmp/baga_bytes_out.txt > /dev/null \
		&& echo "OK: bytes тип (hex литерал, len/at/slice/concat, str/hex конверсии)" \
		|| { echo "FAIL: bytes"; cat /tmp/baga_bytes_out.txt; exit 1; }
	@echo "=== extern fn (FFI) ==="
	@rm -f /tmp/baga_extern_write.txt
	@./$(BIN) $(BAGAIFLAGS) examples/extern_write.baga | grep -q "written"
	@test "$$(cat /tmp/baga_extern_write.txt)" = "baga ffi works" \
		&& echo "OK: extern fn записва файл" \
		|| { echo "FAIL: extern fn"; exit 1; }
	@printf 'extern fn bad(v: Vec<i64>) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern.baga
	@./$(BIN) $(BAGAIFLAGS) /tmp/baga_bad_extern.baga 2>&1 | grep -q "неподдържан тип на параметър" \
		&& echo "OK: extern fn типовото ограничение е хванато" \
		|| { echo "FAIL: extern fn типовото ограничение"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --emit-c examples/extern_write.baga | grep -v "static void baga_write" | grep -q "baga_write" \
		&& { echo "FAIL: extern write в statement позиция отива към builtin"; exit 1; } \
		|| echo "OK: extern write в statement позиция вика libc"
	@printf 'extern fn bad(v: void) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern_void.baga
	@./$(BIN) $(BAGAIFLAGS) /tmp/baga_bad_extern_void.baga 2>&1 | grep -q "неподдържан тип на параметър" \
		&& echo "OK: void параметър на extern fn е отхвърлен" \
		|| { echo "FAIL: void параметър на extern fn не е отхвърлен"; exit 1; }
	@echo "=== arena ==="
	@./$(BIN) $(BAGAIFLAGS) examples/arena.baga > /tmp/baga_arena_out.txt
	@printf "true\ntrue\narena ok\n" | diff - /tmp/baga_arena_out.txt > /dev/null \
		&& echo "OK: arena алокатор" \
		|| { echo "FAIL: arena"; exit 1; }
	@echo "=== --test-specs (property-based) ==="
	@./$(BIN) $(BAGAIFLAGS) --test-specs examples/spec_ensures.baga
	@./$(BIN) $(BAGAIFLAGS) --test-specs examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: --test-specs намери контрапример" \
		|| { echo "FAIL: --test-specs не намери контрапример"; exit 1; }
	@echo "=== --check / --lib (без main, G2) ==="
	@./$(BIN) $(BAGAIFLAGS) --check app-product/httpdbaga/http.baga | grep -q "ok:" \
		&& echo "OK: --check на библиотека без main" \
		|| { echo "FAIL: --check http.baga"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --lib app-product/jwtbaga/jwt.baga | grep -q "ok:" \
		&& echo "OK: --lib на jwt.baga" \
		|| { echo "FAIL: --lib jwt.baga"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --emit-c app-product/httpdbaga/http.baga 2>/dev/null | grep -q "b_main" \
		&& { echo "FAIL: --emit-c на lib не трябва да емитва b_main"; exit 1; } \
		|| echo "OK: --emit-c на библиотека (без main wrapper)"
	@./$(BIN) $(BAGAIFLAGS) app-product/httpdbaga/http.baga 2>&1 | grep -q "липсва функция 'main'" \
		&& echo "OK: run без main все още изисква main" \
		|| { echo "FAIL: run без main трябва да гърми"; exit 1; }
	@printf 'fn main() -> i64 {\n    return 7\n}\n' > /tmp/baga_exitcode.baga
	@./$(BIN) $(BAGAIFLAGS) /tmp/baga_exitcode.baga > /dev/null 2>&1; \
		test $$? -eq 7 \
		&& echo "OK: main -> i64 връща exit кода на процеса (kvbaga K3)" \
		|| { echo "FAIL: exit кодът на main се губи"; exit 1; }
	@echo "=== http (app-product/httpdbaga) ==="
	@./$(BIN) $(BAGAIFLAGS) tests/http_test.baga > /tmp/baga_http_out.txt
	@grep -q "http_test: all passed" /tmp/baga_http_out.txt \
		&& echo "OK: HTTP parser + responder (loopback)" \
		|| { echo "FAIL: http_test"; cat /tmp/baga_http_out.txt; exit 1; }
	@echo "=== hpack (app-product/httpdbaga, RFC 7541) ==="
	@./$(BIN) $(BAGAIFLAGS) tests/hpack_test.baga > /tmp/baga_hpack_out.txt
	@grep -q "hpack_test: all passed" /tmp/baga_hpack_out.txt \
		&& echo "OK: HPACK known answers (RFC 7541 C.1–C.4) + encoder round-trip" \
		|| { echo "FAIL: hpack_test"; cat /tmp/baga_hpack_out.txt; exit 1; }
	@echo "=== h2 (app-product/httpdbaga, HTTP/2 loopback) ==="
	@./$(BIN) $(BAGAIFLAGS) tests/h2_test.baga > /tmp/baga_h2_out.txt
	@grep -q "h2_test: all passed" /tmp/baga_h2_out.txt \
		&& echo "OK: HTTP/2 framing + streams (in-process client vs h2_serve)" \
		|| { echo "FAIL: h2_test"; cat /tmp/baga_h2_out.txt; exit 1; }
	@echo "=== jwt (app-product/jwtbaga) ==="
	@./$(BIN) $(BAGAIFLAGS) tests/jwt_test.baga > /tmp/baga_jwt_out.txt
	@grep -q "jwt_test: all passed" /tmp/baga_jwt_out.txt \
		&& echo "OK: JWT HS256 sign/verify (golden vector)" \
		|| { echo "FAIL: jwt_test"; cat /tmp/baga_jwt_out.txt; exit 1; }
	@echo "=== pg (app-product/pgbaga, live Postgres) ==="
	@./$(BIN) $(BAGAIFLAGS) --lib app-product/pgbaga/pg.baga | grep -q "ok:" \
		&& echo "OK: --lib pg.baga" \
		|| { echo "FAIL: --lib pg.baga"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/pg_test.baga > /tmp/baga_pg_out.txt
	@grep -q "pg_test: all passed" /tmp/baga_pg_out.txt \
		&& echo "OK: PostgreSQL SCRAM + Simple Query (live)" \
		|| { echo "FAIL: pg_test (need PG on 127.0.0.1 + bagatest role?)"; cat /tmp/baga_pg_out.txt; exit 1; }
	@echo "=== orm (app-product/ormbaga, ActiveRecord + goose) ==="
	@./$(BIN) $(BAGAIFLAGS) --lib app-product/ormbaga/orm.baga | grep -q "ok:" \
		&& echo "OK: --lib orm.baga" \
		|| { echo "FAIL: --lib orm.baga"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/orm_test.baga > /tmp/baga_orm_out.txt
	@grep -q "orm_test: all passed" /tmp/baga_orm_out.txt \
		&& echo "OK: ORM migrations + CRUD (live baga_orm)" \
		|| { echo "FAIL: orm_test (need DB baga_orm + bagatest?)"; cat /tmp/baga_orm_out.txt; exit 1; }
	@echo "=== fmr (app-product/fmrbaga, FastAPI-style) ==="
	@./$(BIN) $(BAGAIFLAGS) --lib app-product/fmrbaga/handlers.baga | grep -q "ok:" \
		&& echo "OK: --lib fmrbaga handlers" \
		|| { echo "FAIL: --lib fmrbaga"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/fmr_test.baga > /tmp/baga_fmr_out.txt
	@grep -q "fmr_test: all passed" /tmp/baga_fmr_out.txt \
		&& echo "OK: fmrbaga jsonx + router + validation" \
		|| { echo "FAIL: fmr_test"; cat /tmp/baga_fmr_out.txt; exit 1; }
	@echo "=== kv (app-product/kvbaga, RESP KV сървър върху Map) ==="
	@./$(BIN) $(BAGAIFLAGS) --lib app-product/kvbaga/server.baga | grep -q "ok:" \
		&& echo "OK: --lib kvbaga server" \
		|| { echo "FAIL: --lib kvbaga"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/kv_test.baga > /tmp/baga_kv_out.txt
	@grep -q "kv_test: all passed" /tmp/baga_kv_out.txt \
		&& echo "OK: KV RESP сървър (loopback, Map<str,str> + TTL)" \
		|| { echo "FAIL: kv_test"; cat /tmp/baga_kv_out.txt; exit 1; }
	@echo "=== apps/api (Lucky-style product) ==="
	@./$(BIN) $(BAGAIFLAGS) --check apps/api/start.baga | grep -q "ok:" \
		&& echo "OK: --check apps/api/start.baga" \
		|| { echo "FAIL: apps/api"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/api_test.baga > /tmp/baga_api_out.txt
	@grep -q "api_test: all passed" /tmp/baga_api_out.txt \
		&& echo "OK: apps/api models + openapi posts" \
		|| { echo "FAIL: api_test"; cat /tmp/baga_api_out.txt; exit 1; }
	@echo "=== registry (apps/registry + std HTTP клиент, live) ==="
	@./$(BIN) $(BAGAIFLAGS) --check apps/registry/start.baga | grep -q "ok:" \
		&& echo "OK: --check apps/registry/start.baga" \
		|| { echo "FAIL: --check apps/registry"; exit 1; }
	@PORT=8090 PGDATABASE=baga_registry ./$(BIN) $(BAGAIFLAGS) tests/registry_test.baga > /tmp/baga_reg_out.txt
	@grep -q "registry_test: all passed" /tmp/baga_reg_out.txt \
		&& echo "OK: registry publish/search/show + std HTTP клиент (live)" \
		|| { echo "FAIL: registry_test (нужен е live Postgres)"; cat /tmp/baga_reg_out.txt; exit 1; }
	@echo "=== oauth (app-product/oauthbaga, Postgres persistence) ==="
	@OAUTH_PG=1 PGDATABASE=baga_oauth ./$(BIN) $(BAGAIFLAGS) tests/oauth_pg_test.baga > /tmp/baga_oauth_out.txt
	@grep -q "oauth_pg_test: all passed" /tmp/baga_oauth_out.txt \
		&& echo "OK: OAuth пълен цикъл с Postgres codes/refresh/sessions (live)" \
		|| { echo "FAIL: oauth_pg_test (нужен е live Postgres)"; cat /tmp/baga_oauth_out.txt; exit 1; }
	@echo "=== ws (app-product/wsbaga, RFC 6455 loopback) ==="
	@./$(BIN) $(BAGAIFLAGS) --lib app-product/wsbaga/ws.baga | grep -q "ok:" \
		&& echo "OK: --lib wsbaga ws.baga" \
		|| { echo "FAIL: --lib wsbaga"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/ws_test.baga > /tmp/baga_ws_out.txt
	@grep -q "ws_test: all passed" /tmp/baga_ws_out.txt \
		&& echo "OK: WebSocket handshake + frames (loopback, SHA-1)" \
		|| { echo "FAIL: ws_test"; cat /tmp/baga_ws_out.txt; exit 1; }
	@echo "=== par (go/join/chan, !Par) ==="
	@./$(BIN) $(BAGAIFLAGS) examples/par.baga > /tmp/baga_par_out.txt
	@printf "49\n81\n42\n" | diff - /tmp/baga_par_out.txt > /dev/null \
		&& echo "OK: go/join fan-out" \
		|| { echo "FAIL: par"; cat /tmp/baga_par_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) examples/par_chan.baga > /tmp/baga_par_chan_out.txt
	@printf "240\n" | diff - /tmp/baga_par_chan_out.txt > /dev/null \
		&& echo "OK: chan fan-in" \
		|| { echo "FAIL: par_chan"; cat /tmp/baga_par_chan_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) examples/par_pool.baga > /tmp/baga_par_pool_out.txt
	@printf "385\n" | diff - /tmp/baga_par_pool_out.txt > /dev/null \
		&& echo "OK: pool_map bounded workers" \
		|| { echo "FAIL: par_pool"; cat /tmp/baga_par_pool_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/probe_alloc_race.baga > /tmp/baga_race_out.txt
	@grep -q "total=128032" /tmp/baga_race_out.txt \
		&& echo "OK: конкурентни алокации през global arena (G11 регресия)" \
		|| { echo "FAIL: alloc race"; cat /tmp/baga_race_out.txt; exit 1; }
	@echo "=== std библиотеката (str/bytes/sort/json/crypto/os/time/random/io/net/par) ==="
	@for t in bytes hmac http_client io json map os poll random sha1 sha256 sort str tcp tcp_bytes time par; do \
		./$(BIN) $(BAGAIFLAGS) tests/std/$${t}_test.baga > /tmp/baga_std_out.txt 2>&1 \
			&& grep -q "all passed" /tmp/baga_std_out.txt \
			&& echo "OK: std/$$t" \
			|| { echo "FAIL: std/$$t"; cat /tmp/baga_std_out.txt; exit 1; }; \
	done
	@printf 'fn main() {\n    let m: Map<str, i64> = map_new()\n    map_set(m, "a", "текст")\n}\n' > /tmp/baga_map_bad.baga
	@./$(BIN) $(BAGAIFLAGS) /tmp/baga_map_bad.baga 2>&1 | grep -q "стойност от тип str, но картата е Map<str, i64>" \
		&& echo "OK: Map<K,V> стойностен mismatch е отхвърлен" \
		|| { echo "FAIL: map value mismatch трябва да гърми"; exit 1; }
	@printf 'fn main() {\n    let m = map_new()\n    map_set(m, "k", 1)\n    map_set(m, 2, 3)\n}\n' > /tmp/baga_map_bad2.baga
	@./$(BIN) $(BAGAIFLAGS) /tmp/baga_map_bad2.baga 2>&1 | grep -q "ключ от тип i64, но картата е" \
		&& echo "OK: смесен ключов тип е отхвърлен" \
		|| { echo "FAIL: map key mismatch трябва да гърми"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/probe_binary_io.baga > /tmp/baga_bio_out.txt 2>&1 \
		&& grep -q "all passed" /tmp/baga_bio_out.txt \
		&& echo "OK: binary I/O през сокети (NUL/0xFF, chr() UTF-8 капан — G8)" \
		|| { echo "FAIL: probe_binary_io"; cat /tmp/baga_bio_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) tests/std/sha_big_probe.baga > /tmp/baga_std_probe.txt \
		&& printf '1310720\ndf0be9d175a152159d1a9c73747a686186eb63b56466d5eed6ad6f540d133aff\n' | diff - /tmp/baga_std_probe.txt > /dev/null \
		&& echo "OK: std/sha256 върху 1.25 MB вход (oracle: hashlib)" \
		|| { echo "FAIL: sha_big_probe"; cat /tmp/baga_std_probe.txt; exit 1; }
	@echo "=== verify (статична верификация, M0+M1) ==="
	@for f in abs_val max2 clamp sum; do \
		./$(BIN) $(BAGAIFLAGS) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f доказано (completeness)" \
			|| { echo "FAIL: $$f — очаквах ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/bad_abs.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
		&& echo "OK: bad_abs оброчено с контрапример (soundness)" \
		|| { echo "FAIL: bad_abs — очаквах ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/nonlinear.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
		&& echo "OK: nonlinear — x*y>=0 е оброчено с реализируем контрапример (M8b)" \
		|| { echo "FAIL: nonlinear"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/bad_loop.baga > /tmp/baga_verify_out.txt; \
	grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: bad_loop — грешен инвариант не води до фалшиво доказателство (soundness)" \
		|| { echo "FAIL: bad_loop"; cat /tmp/baga_verify_out.txt; exit 1; }
	@for f in vec_safe vec_guard vec_param; do \
		./$(BIN) $(BAGAIFLAGS) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "граница.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — достъпът до вектор е доказано в границите (M2)" \
			|| { echo "FAIL: $$f — очаквах граница ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/vec_oob.baga > /tmp/baga_verify_out.txt; \
	grep -q "граница.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: vec_oob — извън-границите достъп е оброчен (M2 soundness)" \
		|| { echo "FAIL: vec_oob — очаквах граница ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/vec_param_unsafe.baga > /tmp/baga_verify_out.txt; \
	grep -q "граница.*НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "граница.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: vec_param_unsafe — неохраняван достъп не се доказва (M2 soundness)" \
		|| { echo "FAIL: vec_param_unsafe"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/recursive.baga > /tmp/baga_verify_out.txt; \
	grep -q "ПРОПУСНАТО" /tmp/baga_verify_out.txt \
		&& echo "OK: recursive — честно пропуснато" \
		|| { echo "FAIL: recursive"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/sum_rec.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: sum_rec — рекурсия доказана с индукционна хипотеза (M5)" \
		|| { echo "FAIL: sum_rec — очаквах ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/sum_rec.baga | grep -q "частична коректност" \
		&& echo "OK: sum_rec — бележка за частична коректност (M5 честност)" \
		|| { echo "FAIL: sum_rec — липсва бележка за частична коректност"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/bad_rec.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
		&& echo "OK: bad_rec — грешен ensures на рекурсия е оброчен (M5 soundness)" \
		|| { echo "FAIL: bad_rec — очаквах ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/call_req_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "извикване.*ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
		&& echo "OK: call_req_bad — requires при извикване е оброчен с контрапример (M5 soundness)" \
		|| { echo "FAIL: call_req_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/term_dec.baga > /tmp/baga_verify_out.txt; \
	grep -q "терминация: доказана" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША|частична коректност)" /tmp/baga_verify_out.txt \
		&& echo "OK: term_dec — decreases доказва терминация, пълна коректност (M6)" \
		|| { echo "FAIL: term_dec"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/term_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "терминация.*намалява.*ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "частична коректност" /tmp/baga_verify_out.txt \
		&& echo "OK: term_bad — ненамаляваща мярка е оброчена, ensures остава частична коректност (M6 soundness)" \
		|| { echo "FAIL: term_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/int_exact.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: int_exact — n > 0 ⇒ n >= 1 чрез integer tightening (M7)" \
		|| { echo "FAIL: int_exact"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/int_exact_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 1" /tmp/baga_verify_out.txt \
		&& echo "OK: int_exact_bad — n > 0 ⇏ n >= 2 е оброчен с n = 1 (M7 soundness)" \
		|| { echo "FAIL: int_exact_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/spurious.baga > /tmp/baga_verify_out.txt; \
	grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: spurious — няма фалшиво оборване през абстрактни стойности (M8 soundness)" \
		|| { echo "FAIL: spurious — очаквах UNKNOWN без ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/fact_full.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && grep -q "терминация: доказана" /tmp/baga_verify_out.txt \
		&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: fact_full — факториел напълно доказан през продуктова аксиома (M8b)" \
		|| { echo "FAIL: fact_full"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/square.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: square — x * x >= 0 без предусловия (M8b)" \
		|| { echo "FAIL: square"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/fact_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
		&& echo "OK: fact_bad — грешно твърдение за продукт е оброчено conclusively (M8b soundness)" \
		|| { echo "FAIL: fact_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/sign_prod.baga > /tmp/baga_verify_out.txt; \
	grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^4$$' \
		&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: sign_prod — пълна знакова таблица за продукти (M9)" \
		|| { echo "FAIL: sign_prod"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/sum_sq.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: sum_sq — x*x + y*y >= 0 (M9)" \
		|| { echo "FAIL: sum_sq"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/div_const.baga > /tmp/baga_verify_out.txt; \
	grep -q "half_nonneg" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "half_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: div_const — n/k знак + soundness (M9)" \
		|| { echo "FAIL: div_const"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/mod_const.baga > /tmp/baga_verify_out.txt; \
	grep -q "mod3" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "mod_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: mod_const — n%%k bounds + soundness (M9b)" \
		|| { echo "FAIL: mod_const"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/poly_depth.baga > /tmp/baga_verify_out.txt; \
	grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -qE '^[4-9]$$|^[1-9][0-9]' \
		&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: poly_depth — square dominance + product mono (M10)" \
		|| { echo "FAIL: poly_depth"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/div_mod_id.baga > /tmp/baga_verify_out.txt; \
	grep -q "rebuild:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "rebuild_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: div_mod_id — n = q*k + r (M10)" \
		|| { echo "FAIL: div_mod_id"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/floor_mul.baga > /tmp/baga_verify_out.txt; \
	grep -q "floor4" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "floor_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: floor_mul — k*(n/k)<=n (M11)" \
		|| { echo "FAIL: floor_mul"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/complete_sq.baga > /tmp/baga_verify_out.txt; \
	grep -q "complete_m1" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "too_strong" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: complete_sq — (x±1)^2 (M11)" \
		|| { echo "FAIL: complete_sq"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/var_div.baga > /tmp/baga_verify_out.txt; \
	grep -q "rebuild_var" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "unsafe_div" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: var_div — n/m, n%m, n=qm+r (M12)" \
		|| { echo "FAIL: var_div"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/amgm.baga > /tmp/baga_verify_out.txt; \
	grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^[2-9]' \
		&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: amgm — (x-y)^2 >= 0 (M12)" \
		|| { echo "FAIL: amgm"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/nonlinear_if.baga > /tmp/baga_verify_out.txt; \
	grep -q "sq_guard:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "sq_pos_branch" /tmp/baga_verify_out.txt \
		&& grep -q "sq_guard_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: nonlinear_if — products in if-guards (M13)" \
		|| { echo "FAIL: nonlinear_if"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/bitwise_laws.baga > /tmp/baga_verify_out.txt; \
	grep -q "or_zero" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "bit_lsb" /tmp/baga_verify_out.txt \
		&& grep -q "bit_lsb_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: bitwise_laws — BV identities + n&1 (M13)" \
		|| { echo "FAIL: bitwise_laws"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/par_join.baga > /tmp/baga_verify_out.txt; \
	grep -q "par_double:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: par_join — fork-join детерминизъм: spec на worker през go/join (M14)" \
		|| { echo "FAIL: par_join"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/par_join_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
		&& echo "OK: par_join_bad — грешен ensures през join е оброчен (M14 soundness)" \
		|| { echo "FAIL: par_join_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/par_detach_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "протокол (join след detach.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: par_detach_bad — join след detach е статично оброчен (M14 протокол)" \
		|| { echo "FAIL: par_detach_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/par_chan.baga > /tmp/baga_verify_out.txt; \
	grep -q "send_after_close:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "recv_claim:" /tmp/baga_verify_out.txt && grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: par_chan — send след close ⇒ -1 доказано; recv payload честно UNKNOWN (M14)" \
		|| { echo "FAIL: par_chan"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_add.baga > /tmp/baga_verify_out.txt; \
	grep -q "inc_bounded:" /tmp/baga_verify_out.txt && grep -q "1/1 операции доказано безопасни" /tmp/baga_verify_out.txt \
		&& grep -q "контрапример: n = 9223372036854775807" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_add — ограниченото събиране е доказано; неограниченото е оброчено при INT64_MAX (M15)" \
		|| { echo "FAIL: ovf_add"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_mul.baga > /tmp/baga_verify_out.txt; \
	grep -q "mul_bounded:" /tmp/baga_verify_out.txt && grep -q "1/1 операции доказано безопасни" /tmp/baga_verify_out.txt \
		&& grep -q "аритметика (преливане: (a \* b)): ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_mul — FM граници доказват продукт; неограничен продукт е оброчен (M15)" \
		|| { echo "FAIL: ovf_mul"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/div_zero.baga > /tmp/baga_verify_out.txt; \
	grep -q "div_safe:" /tmp/baga_verify_out.txt && grep -q "1/1 операции доказано безопасни" /tmp/baga_verify_out.txt \
		&& grep -q "контрапример: n = 0 m = 0" /tmp/baga_verify_out.txt \
		&& echo "OK: div_zero — m >= 1 доказва безопасно деление; без нея m = 0 е оброчен (M15)" \
		|| { echo "FAIL: div_zero"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/loop_havoc.baga > /tmp/baga_verify_out.txt; \
	grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ensures.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: loop_havoc — няма фалшиво ДОКАЗАНО през стойности от преди цикъла (M15 soundness)" \
		|| { echo "FAIL: loop_havoc"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/abs_val.baga > /tmp/baga_verify_out.txt; \
	grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "аритметика.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& grep -q "x = -9223372036854775808" /tmp/baga_verify_out.txt \
		&& echo "OK: abs_val — ensures доказан, но abs(INT64_MIN) преливане е хванато (M15)" \
		|| { echo "FAIL: abs_val arith"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/chan_inv.baga > /tmp/baga_verify_out.txt; \
	grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: chan_inv — съдържателен инвариант: send discharge + recv instantiate (M16)" \
		|| { echo "FAIL: chan_inv"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/chan_inv_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ensures.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: chan_inv_bad — недоказуем payload изпуска аксиомата, recv е честно UNKNOWN (M16)" \
		|| { echo "FAIL: chan_inv_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/chan_inv_par.baga > /tmp/baga_verify_out.txt; \
	grep -q "boss:" /tmp/baga_verify_out.txt && grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "канален инвариант на 'worker' при извикване.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: chan_inv_par — cross-thread инвариант с discharge при spawn (M16 rely–guarantee)" \
		|| { echo "FAIL: chan_inv_par"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/chan_inv_escape.baga > /tmp/baga_verify_out.txt; \
	grep -q "boss2:" /tmp/baga_verify_out.txt && grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& ! grep -q "boss2:" -A1 /tmp/baga_verify_out.txt | grep -q "ДОКАЗАНО" \
		&& echo "OK: chan_inv_escape — worker без requires изпуска аксиомата при spawn (M16 drop rule)" \
		|| { echo "FAIL: chan_inv_escape"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/pair_recv2.baga > /tmp/baga_verify_out.txt; \
	grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: pair_recv2 — ok-flag + content инвариант през cell2 проекции (M17)" \
		|| { echo "FAIL: pair_recv2"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/pair_select.baga > /tmp/baga_verify_out.txt; \
	grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt && grep -q "ensures #2.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "which_too_high:" /tmp/baga_verify_out.txt && grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: pair_select — which ∈ [0,3] доказано; ≤ 2 честно UNKNOWN (M17)" \
		|| { echo "FAIL: pair_select"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/pair_go.baga > /tmp/baga_verify_out.txt; \
	grep -q "boss:" /tmp/baga_verify_out.txt && grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "requires на 'worker' при извикване.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: pair_go — packed аргумент, requires върху компоненти discharged при spawn (M17)" \
		|| { echo "FAIL: pair_go"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_eff_safe.baga > /tmp/baga_verify_out.txt \
		&& grep -q "ефект !Overflow: безопасна — типът е точен" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_eff_safe — доказано безопасна, без !Overflow ⇒ типът е точен (M18)" \
		|| { echo "FAIL: ovf_eff_safe"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_eff_refuted.baga > /tmp/baga_verify_out.txt; \
	test $$? -ne 0 \
		&& grep -q "прелива при n = 9223372036854775807, а !Overflow не е деклариран" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_eff_refuted — недекларирано преливане е нарушение, ненулев exit (M18)" \
		|| { echo "FAIL: ovf_eff_refuted"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_eff_declared.baga > /tmp/baga_verify_out.txt \
		&& grep -q "ефект !Overflow: деклариран — прелива при" /tmp/baga_verify_out.txt \
		&& ! grep -q "не е деклариран" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_eff_declared — декларираното !Overflow discharge-ва преливането, exit 0 (M18)" \
		|| { echo "FAIL: ovf_eff_declared"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_eff_unknown.baga > /tmp/baga_verify_out.txt \
		&& grep -q "ефект !Overflow: безопасността не е доказуема — декларирай !Overflow" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_eff_unknown — недоказуема безопасност иска декларация, без провал (M18)" \
		|| { echo "FAIL: ovf_eff_unknown"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_eff_redundant.baga > /tmp/baga_verify_out.txt \
		&& grep -q "деклариран, но аритметиката е доказано безопасна" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_eff_redundant — излишно, но честно !Overflow върху безопасна функция (M18)" \
		|| { echo "FAIL: ovf_eff_redundant"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/ovf_eff_skip.baga > /tmp/baga_verify_out.txt; \
	grep -q "ПРОПУСНАТО" /tmp/baga_verify_out.txt && ! grep -q "ефект !Overflow" /tmp/baga_verify_out.txt \
		&& echo "OK: ovf_eff_skip — пропусната функция няма ефект !Overflow ред (M18 честност)" \
		|| { echo "FAIL: ovf_eff_skip"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) examples/verify/ovf_eff_propagate.baga 2>&1 | grep -q "необработен ефект !Overflow" \
		&& echo "OK: ovf_eff_propagate — !Overflow се разпространява и checker-ът го изисква (M18)" \
		|| { echo "FAIL: ovf_eff_propagate"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --check examples/verify/ovf_eff_propagate_ok.baga | grep -q "ok:" \
		&& echo "OK: ovf_eff_propagate_ok — декларираното !Overflow обработва ефекта (M18)" \
		|| { echo "FAIL: ovf_eff_propagate_ok"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify --json examples/verify/ovf_eff_refuted.baga | grep -q '"overflow_effect"' \
		&& ./$(BIN) $(BAGAIFLAGS) --verify --json examples/verify/ovf_eff_refuted.baga | grep -q '"result": "refuted"' \
		&& echo "OK: ovf_eff JSON — overflow_effect полето е машинно четимо (M18)" \
		|| { echo "FAIL: ovf_eff JSON"; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --proofs examples/verify/ovf_eff_safe.baga > /tmp/baga_proofs_out.txt; \
	grep -q "theorem inc_bounded_overflow_safe" /tmp/baga_proofs_out.txt \
		&& grep -q "ДОКАЗАНО (M18" /tmp/baga_proofs_out.txt \
		&& echo "OK: proofs — overflow_safe теорема в --proofs (M18)" \
		|| { echo "FAIL: proofs overflow_safe"; cat /tmp/baga_proofs_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --proofs examples/verify/sum.baga > /tmp/baga_proofs_out.txt; \
	grep -q "lemma add_repeated_invariant_1" /tmp/baga_proofs_out.txt \
		&& grep -q "invariant: (s >= 0)" /tmp/baga_proofs_out.txt \
		&& grep -q "ДОКАЗАНО (init + preservation, Hoare)" /tmp/baga_proofs_out.txt \
		&& echo "OK: proofs — верифицирани while инварианти в --proofs" \
		|| { echo "FAIL: proofs invariants"; cat /tmp/baga_proofs_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --proofs examples/verify/fact_full.baga > /tmp/baga_proofs_out.txt; \
	grep -q "decreases measure — proven statically (full correctness)" /tmp/baga_proofs_out.txt \
		&& echo "OK: proofs — реална терминация чрез decreases в --proofs" \
		|| { echo "FAIL: proofs termination"; cat /tmp/baga_proofs_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --proofs examples/verify/bad_loop.baga > /tmp/baga_proofs_out.txt; \
	grep -q "НЕ Е ДОКАЗАНА" /tmp/baga_proofs_out.txt \
		&& echo "OK: proofs — недоказан инвариант е отбелязан честно" \
		|| { echo "FAIL: proofs unproven invariant"; cat /tmp/baga_proofs_out.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) examples/par_select.baga > /tmp/baga_par_sel_out.txt; \
	printf "30\n2\n" | diff - /tmp/baga_par_sel_out.txt > /dev/null \
		&& echo "OK: chan_select2_wait/timeout" \
		|| { echo "FAIL: par_select"; cat /tmp/baga_par_sel_out.txt; exit 1; }
	@for f in elem_param elem_push elem_set elem_slice elem_concat; do \
		./$(BIN) $(BAGAIFLAGS) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — елементен инвариант доказан (M3)" \
			|| { echo "FAIL: $$f — очаквах ensures ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@for f in elem_bad elem_set_bad; do \
		./$(BIN) $(BAGAIFLAGS) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ensures #1.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — нарушен елементен инвариант е оброчен (M3 soundness)" \
			|| { echo "FAIL: $$f — очаквах ensures ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/elem_slice_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "граница.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: elem_slice_bad — out-of-bounds достъп е оброчен (M3 bounds)" \
		|| { echo "FAIL: elem_slice_bad — очаквах граница ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@for f in sorted_param sorted_push; do \
		./$(BIN) $(BAGAIFLAGS) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — sorted + element axiom (relational M3+)" \
			|| { echo "FAIL: $$f — очаквах ensures ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) $(BAGAIFLAGS) --verify examples/verify/sorted_not_le.baga > /tmp/baga_verify_out.txt; true; \
	grep -qE "ensures #1.*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt && ! grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: sorted_not_le — sorted ≠ v[*]<=0 (soundness)" \
		|| { echo "FAIL: sorted_not_le — sorted не трябва да доказва output<=0"; cat /tmp/baga_verify_out.txt; exit 1; }
	@for f in abs_val max2 clamp; do \
		./$(BIN) $(BAGAIFLAGS) --test-specs examples/verify/$$f.baga > /dev/null 2>&1 \
			&& echo "OK: $$f — оракулът (--test-specs) съгласен с ДОКАЗАНО" \
			|| { echo "FAIL: $$f — оракулът не е съгласен"; exit 1; }; \
	done
	@echo "=== --verify --json (машинен изход) ==="
	@./$(BIN) $(BAGAIFLAGS) --verify --json examples/verify/max2.baga > /tmp/baga_verify_json.txt
	@python3 -c "import json; d=json.load(open('/tmp/baga_verify_json.txt')); f=d['functions'][0]; assert f['name']=='max2' and f['ensures'][0]['result']=='proven' and f['arith']==[]" \
		&& echo "OK: --verify --json валиден JSON, proven" \
		|| { echo "FAIL: --verify --json"; cat /tmp/baga_verify_json.txt; exit 1; }
	@./$(BIN) $(BAGAIFLAGS) --verify --json examples/verify/bad_abs.baga > /tmp/baga_verify_json_bad.txt; test $$? -eq 1 \
		&& python3 -c "import json; d=json.load(open('/tmp/baga_verify_json_bad.txt')); e=d['functions'][0]['ensures'][0]; assert e['result']=='refuted' and e['counterexample']" \
		&& echo "OK: --verify --json refuted + контрапример, exit=1" \
		|| { echo "FAIL: --verify --json refuted"; cat /tmp/baga_verify_json_bad.txt; exit 1; }
	@echo "=== LLVM оракул (C vs lli-14) ==="
	@if [ -f ./$(LLVM_BIN) ]; then $(MAKE) -s test-llvm; else echo "(baga-llvm липсва — пропускам LLVM оракула)"; fi
	@echo ""
	@echo "Всички тестове минаха. ⚔️"
