CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm -pthread

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c src/verify.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test test-llvm llvm self par-rt

all: $(BIN)

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
	@./$(BIN) --emit-c self/compiler.baga > /tmp/baga_self2.c
	@gcc $(CFLAGS) -o /tmp/baga2 /tmp/baga_self2.c $(LDFLAGS)
	@/tmp/baga2 self/compiler.baga > /tmp/baga_self3.c
	@gcc $(CFLAGS) -o /tmp/baga3 /tmp/baga_self3.c $(LDFLAGS)
	@/tmp/baga3 self/compiler.baga > /tmp/baga_self4.c
	@if diff -q /tmp/baga_self3.c /tmp/baga_self4.c > /dev/null; then \
		echo "OK: baga2 == baga3 (fixed point — self компилаторът се възпроизвежда) ⚔️"; \
	else \
		echo "FAIL: baga2 != baga3"; diff /tmp/baga_self3.c /tmp/baga_self4.c | head -20; exit 1; \
	fi

test: $(BIN)
	@echo "=== здравей ==="
	./$(BIN) examples/zdravei.baga
	@echo "=== факториел ==="
	./$(BIN) examples/faktorial.baga
	@echo "=== фибоначи ==="
	./$(BIN) examples/fib.baga
	@echo "=== типове ==="
	./$(BIN) examples/types.baga
	@echo "=== struct ==="
	./$(BIN) examples/tochka.baga
	@echo "=== match ==="
	./$(BIN) examples/match.baga
	@echo "=== effects ==="
	./$(BIN) examples/effects.baga
	@echo "=== spec ==="
	./$(BIN) examples/spec.baga
	@./$(BIN) examples/spec_ensures.baga > /dev/null
	@echo "=== spec_ensures_fail (очакваме runtime грешка) ==="
	@./$(BIN) examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: ensures гаранцията е хваната" \
		|| { echo "FAIL: ensures не е хваната"; exit 1; }
	@echo "=== spec_requires_fail (очакваме runtime грешка) ==="
	@./$(BIN) examples/spec_requires_fail.baga 2>&1 | grep -q "requires #1 нарушено" \
		&& echo "OK: requires предусловието е хванато" \
		|| { echo "FAIL: requires не е хванато"; exit 1; }
	@echo "=== vec_typed (очакваме compile грешка) ==="
	@./$(BIN) examples/vec_typed.baga 2>&1 | grep -q "елемент от тип str, но векторът е Vec<i64>" \
		&& echo "OK: Vec<T> хвана смесването" \
		|| { echo "FAIL: Vec<T> не хвана смесването"; exit 1; }
	@echo "=== arg_type_bad (очакваме compile грешка) ==="
	@./$(BIN) examples/arg_type_bad.baga 2>&1 | grep -q "аргумент #1 е от тип str, но параметърът е i64" \
		&& echo "OK: проверката на аргументите хвана грешния тип" \
		|| { echo "FAIL: проверката на аргументите не хвана грешния тип"; exit 1; }
	@echo "=== vec_ann (Vec<T> анотации) ==="
	./$(BIN) examples/vec_ann.baga
	@echo "=== emit-c cleanup (регресия: double-free при += desugar) ==="
	@./$(BIN) --emit-c examples/vec_ann.baga > /dev/null \
		&& echo "OK: --emit-c не гърми върху += desugar" \
		|| { echo "FAIL: --emit-c"; exit 1; }
	@echo "=== bitwise ==="
	@./$(BIN) examples/bitwise.baga > /tmp/baga_bitwise_out.txt
	@printf "2\n7\n5\n16\n16\n24\n9\n4\n16777215\n" | diff - /tmp/baga_bitwise_out.txt > /dev/null \
		&& echo "OK: побитови оператори" \
		|| { echo "FAIL: побитови оператори"; exit 1; }
	@echo "=== import ==="
	@./$(BIN) tests/import_main.baga > /tmp/baga_import_out.txt
	@printf "49\n21\n" | diff - /tmp/baga_import_out.txt > /dev/null \
		&& echo "OK: import + include guard" \
		|| { echo "FAIL: import"; exit 1; }
	@./$(BIN) tests/import_cycle_a.baga 2>&1 | grep -q "цикличен import" \
		&& echo "OK: import цикълът е хванат" \
		|| { echo "FAIL: import цикълът не е хванат"; exit 1; }
	@echo "=== string interpolation ==="
	@./$(BIN) examples/interp.baga > /tmp/baga_interp_out.txt
	@printf 'name=baga n=42 ok=true expr=84\ndollar=$$ braces={ } neg=-7\n' | diff - /tmp/baga_interp_out.txt > /dev/null \
		&& echo 'OK: $${expr} интерполация (str/i64/bool/call)' \
		|| { echo "FAIL: интерполация"; cat /tmp/baga_interp_out.txt; exit 1; }
	@echo "=== bytes type ==="
	@./$(BIN) examples/bytes.baga > /tmp/baga_bytes_out.txt
	@printf 'len=4\nat0=222\nhex=deadbeef\nroundtrip=hi\ndec_hex=cafe\ncat_hex=deadbeef00ff\nslice_hex=adbe\n' | diff - /tmp/baga_bytes_out.txt > /dev/null \
		&& echo "OK: bytes тип (hex литерал, len/at/slice/concat, str/hex конверсии)" \
		|| { echo "FAIL: bytes"; cat /tmp/baga_bytes_out.txt; exit 1; }
	@echo "=== extern fn (FFI) ==="
	@rm -f /tmp/baga_extern_write.txt
	@./$(BIN) examples/extern_write.baga | grep -q "written"
	@test "$$(cat /tmp/baga_extern_write.txt)" = "baga ffi works" \
		&& echo "OK: extern fn записва файл" \
		|| { echo "FAIL: extern fn"; exit 1; }
	@printf 'extern fn bad(v: Vec<i64>) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern.baga
	@./$(BIN) /tmp/baga_bad_extern.baga 2>&1 | grep -q "неподдържан тип на параметър" \
		&& echo "OK: extern fn типовото ограничение е хванато" \
		|| { echo "FAIL: extern fn типовото ограничение"; exit 1; }
	@./$(BIN) --emit-c examples/extern_write.baga | grep -v "static void baga_write" | grep -q "baga_write" \
		&& { echo "FAIL: extern write в statement позиция отива към builtin"; exit 1; } \
		|| echo "OK: extern write в statement позиция вика libc"
	@printf 'extern fn bad(v: void) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern_void.baga
	@./$(BIN) /tmp/baga_bad_extern_void.baga 2>&1 | grep -q "неподдържан тип на параметър" \
		&& echo "OK: void параметър на extern fn е отхвърлен" \
		|| { echo "FAIL: void параметър на extern fn не е отхвърлен"; exit 1; }
	@echo "=== arena ==="
	@./$(BIN) examples/arena.baga > /tmp/baga_arena_out.txt
	@printf "true\ntrue\narena ok\n" | diff - /tmp/baga_arena_out.txt > /dev/null \
		&& echo "OK: arena алокатор" \
		|| { echo "FAIL: arena"; exit 1; }
	@echo "=== --test-specs (property-based) ==="
	@./$(BIN) --test-specs examples/spec_ensures.baga
	@./$(BIN) --test-specs examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: --test-specs намери контрапример" \
		|| { echo "FAIL: --test-specs не намери контрапример"; exit 1; }
	@echo "=== --check / --lib (без main, G2) ==="
	@./$(BIN) --check app-product/httpdbaga/http.baga | grep -q "ok:" \
		&& echo "OK: --check на библиотека без main" \
		|| { echo "FAIL: --check http.baga"; exit 1; }
	@./$(BIN) --lib app-product/jwtbaga/jwt.baga | grep -q "ok:" \
		&& echo "OK: --lib на jwt.baga" \
		|| { echo "FAIL: --lib jwt.baga"; exit 1; }
	@./$(BIN) --emit-c app-product/httpdbaga/http.baga 2>/dev/null | grep -q "b_main" \
		&& { echo "FAIL: --emit-c на lib не трябва да емитва b_main"; exit 1; } \
		|| echo "OK: --emit-c на библиотека (без main wrapper)"
	@./$(BIN) app-product/httpdbaga/http.baga 2>&1 | grep -q "липсва функция 'main'" \
		&& echo "OK: run без main все още изисква main" \
		|| { echo "FAIL: run без main трябва да гърми"; exit 1; }
	@echo "=== http (app-product/httpdbaga) ==="
	@./$(BIN) tests/http_test.baga > /tmp/baga_http_out.txt
	@grep -q "http_test: all passed" /tmp/baga_http_out.txt \
		&& echo "OK: HTTP parser + responder (loopback)" \
		|| { echo "FAIL: http_test"; cat /tmp/baga_http_out.txt; exit 1; }
	@echo "=== jwt (app-product/jwtbaga) ==="
	@./$(BIN) tests/jwt_test.baga > /tmp/baga_jwt_out.txt
	@grep -q "jwt_test: all passed" /tmp/baga_jwt_out.txt \
		&& echo "OK: JWT HS256 sign/verify (golden vector)" \
		|| { echo "FAIL: jwt_test"; cat /tmp/baga_jwt_out.txt; exit 1; }
	@echo "=== par (go/join/chan, !Par) ==="
	@./$(BIN) examples/par.baga > /tmp/baga_par_out.txt
	@printf "49\n81\n42\n" | diff - /tmp/baga_par_out.txt > /dev/null \
		&& echo "OK: go/join fan-out" \
		|| { echo "FAIL: par"; cat /tmp/baga_par_out.txt; exit 1; }
	@./$(BIN) examples/par_chan.baga > /tmp/baga_par_chan_out.txt
	@printf "240\n" | diff - /tmp/baga_par_chan_out.txt > /dev/null \
		&& echo "OK: chan fan-in" \
		|| { echo "FAIL: par_chan"; cat /tmp/baga_par_chan_out.txt; exit 1; }
	@./$(BIN) examples/par_pool.baga > /tmp/baga_par_pool_out.txt
	@printf "385\n" | diff - /tmp/baga_par_pool_out.txt > /dev/null \
		&& echo "OK: pool_map bounded workers" \
		|| { echo "FAIL: par_pool"; cat /tmp/baga_par_pool_out.txt; exit 1; }
	@echo "=== std библиотеката (str/bytes/sort/json/crypto/os/time/random/io/net/par) ==="
	@for t in bytes hmac io json os random sha256 sort str tcp time par; do \
		./$(BIN) tests/std/$${t}_test.baga > /tmp/baga_std_out.txt 2>&1 \
			&& grep -q "all passed" /tmp/baga_std_out.txt \
			&& echo "OK: std/$$t" \
			|| { echo "FAIL: std/$$t"; cat /tmp/baga_std_out.txt; exit 1; }; \
	done
	@./$(BIN) tests/std/sha_big_probe.baga > /tmp/baga_std_probe.txt \
		&& printf '1310720\ndf0be9d175a152159d1a9c73747a686186eb63b56466d5eed6ad6f540d133aff\n' | diff - /tmp/baga_std_probe.txt > /dev/null \
		&& echo "OK: std/sha256 върху 1.25 MB вход (oracle: hashlib)" \
		|| { echo "FAIL: sha_big_probe"; cat /tmp/baga_std_probe.txt; exit 1; }
	@echo "=== verify (статична верификация, M0+M1) ==="
	@for f in abs_val max2 clamp sum; do \
		./$(BIN) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f доказано (completeness)" \
			|| { echo "FAIL: $$f — очаквах ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) --verify examples/verify/bad_abs.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
		&& echo "OK: bad_abs оброчено с контрапример (soundness)" \
		|| { echo "FAIL: bad_abs — очаквах ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/nonlinear.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
		&& echo "OK: nonlinear — x*y>=0 е оброчено с реализируем контрапример (M8b)" \
		|| { echo "FAIL: nonlinear"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/bad_loop.baga > /tmp/baga_verify_out.txt; \
	grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: bad_loop — грешен инвариант не води до фалшиво доказателство (soundness)" \
		|| { echo "FAIL: bad_loop"; cat /tmp/baga_verify_out.txt; exit 1; }
	@for f in vec_safe vec_guard vec_param; do \
		./$(BIN) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "граница.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — достъпът до вектор е доказано в границите (M2)" \
			|| { echo "FAIL: $$f — очаквах граница ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) --verify examples/verify/vec_oob.baga > /tmp/baga_verify_out.txt; \
	grep -q "граница.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: vec_oob — извън-границите достъп е оброчен (M2 soundness)" \
		|| { echo "FAIL: vec_oob — очаквах граница ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/vec_param_unsafe.baga > /tmp/baga_verify_out.txt; \
	grep -q "граница.*НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "граница.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: vec_param_unsafe — неохраняван достъп не се доказва (M2 soundness)" \
		|| { echo "FAIL: vec_param_unsafe"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/recursive.baga > /tmp/baga_verify_out.txt; \
	grep -q "ПРОПУСНАТО" /tmp/baga_verify_out.txt \
		&& echo "OK: recursive — честно пропуснато" \
		|| { echo "FAIL: recursive"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/sum_rec.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: sum_rec — рекурсия доказана с индукционна хипотеза (M5)" \
		|| { echo "FAIL: sum_rec — очаквах ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/sum_rec.baga | grep -q "частична коректност" \
		&& echo "OK: sum_rec — бележка за частична коректност (M5 честност)" \
		|| { echo "FAIL: sum_rec — липсва бележка за частична коректност"; exit 1; }
	@./$(BIN) --verify examples/verify/bad_rec.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
		&& echo "OK: bad_rec — грешен ensures на рекурсия е оброчен (M5 soundness)" \
		|| { echo "FAIL: bad_rec — очаквах ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/call_req_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "извикване.*ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
		&& echo "OK: call_req_bad — requires при извикване е оброчен с контрапример (M5 soundness)" \
		|| { echo "FAIL: call_req_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/term_dec.baga > /tmp/baga_verify_out.txt; \
	grep -q "терминация: доказана" /tmp/baga_verify_out.txt && ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША|частична коректност" /tmp/baga_verify_out.txt \
		&& echo "OK: term_dec — decreases доказва терминация, пълна коректност (M6)" \
		|| { echo "FAIL: term_dec"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/term_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "терминация.*намалява.*ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "частична коректност" /tmp/baga_verify_out.txt \
		&& echo "OK: term_bad — ненамаляваща мярка е оброчена, ensures остава частична коректност (M6 soundness)" \
		|| { echo "FAIL: term_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/int_exact.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: int_exact — n > 0 ⇒ n >= 1 чрез integer tightening (M7)" \
		|| { echo "FAIL: int_exact"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/int_exact_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 1" /tmp/baga_verify_out.txt \
		&& echo "OK: int_exact_bad — n > 0 ⇏ n >= 2 е оброчен с n = 1 (M7 soundness)" \
		|| { echo "FAIL: int_exact_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/spurious.baga > /tmp/baga_verify_out.txt; \
	grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: spurious — няма фалшиво оборване през абстрактни стойности (M8 soundness)" \
		|| { echo "FAIL: spurious — очаквах UNKNOWN без ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/fact_full.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && grep -q "терминация: доказана" /tmp/baga_verify_out.txt \
		&& ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: fact_full — факториел напълно доказан през продуктова аксиома (M8b)" \
		|| { echo "FAIL: fact_full"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/square.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: square — x * x >= 0 без предусловия (M8b)" \
		|| { echo "FAIL: square"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/fact_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
		&& echo "OK: fact_bad — грешно твърдение за продукт е оброчено conclusively (M8b soundness)" \
		|| { echo "FAIL: fact_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/sign_prod.baga > /tmp/baga_verify_out.txt; \
	grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^4$$' \
		&& ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: sign_prod — пълна знакова таблица за продукти (M9)" \
		|| { echo "FAIL: sign_prod"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/sum_sq.baga > /tmp/baga_verify_out.txt; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: sum_sq — x*x + y*y >= 0 (M9)" \
		|| { echo "FAIL: sum_sq"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/div_const.baga > /tmp/baga_verify_out.txt; \
	grep -q "half_nonneg" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "half_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: div_const — n/k знак + soundness (M9)" \
		|| { echo "FAIL: div_const"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/mod_const.baga > /tmp/baga_verify_out.txt; \
	grep -q "mod3" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "mod_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: mod_const — n%%k bounds + soundness (M9b)" \
		|| { echo "FAIL: mod_const"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/poly_depth.baga > /tmp/baga_verify_out.txt; \
	grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -qE '^[4-9]$$|^[1-9][0-9]' \
		&& ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: poly_depth — square dominance + product mono (M10)" \
		|| { echo "FAIL: poly_depth"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/div_mod_id.baga > /tmp/baga_verify_out.txt; \
	grep -q "rebuild:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "rebuild_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: div_mod_id — n = q*k + r (M10)" \
		|| { echo "FAIL: div_mod_id"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/floor_mul.baga > /tmp/baga_verify_out.txt; \
	grep -q "floor4" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "floor_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: floor_mul — k*(n/k)<=n (M11)" \
		|| { echo "FAIL: floor_mul"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/complete_sq.baga > /tmp/baga_verify_out.txt; \
	grep -q "complete_m1" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "too_strong" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: complete_sq — (x±1)^2 (M11)" \
		|| { echo "FAIL: complete_sq"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/var_div.baga > /tmp/baga_verify_out.txt; \
	grep -q "rebuild_var" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "unsafe_div" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: var_div — n/m, n%m, n=qm+r (M12)" \
		|| { echo "FAIL: var_div"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/amgm.baga > /tmp/baga_verify_out.txt; \
	grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^[2-9]' \
		&& ! grep -qE "ОБРОЧЕНО|НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
		&& echo "OK: amgm — (x-y)^2 >= 0 (M12)" \
		|| { echo "FAIL: amgm"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/nonlinear_if.baga > /tmp/baga_verify_out.txt; \
	grep -q "sq_guard:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "sq_pos_branch" /tmp/baga_verify_out.txt \
		&& grep -q "sq_guard_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: nonlinear_if — products in if-guards (M13)" \
		|| { echo "FAIL: nonlinear_if"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) --verify examples/verify/bitwise_laws.baga > /tmp/baga_verify_out.txt; \
	grep -q "or_zero" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& grep -q "bit_lsb" /tmp/baga_verify_out.txt \
		&& grep -q "bit_lsb_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: bitwise_laws — BV identities + n&1 (M13)" \
		|| { echo "FAIL: bitwise_laws"; cat /tmp/baga_verify_out.txt; exit 1; }
	@./$(BIN) examples/par_select.baga > /tmp/baga_par_sel_out.txt; \
	printf "30\n2\n" | diff - /tmp/baga_par_sel_out.txt > /dev/null \
		&& echo "OK: chan_select2_wait/timeout" \
		|| { echo "FAIL: par_select"; cat /tmp/baga_par_sel_out.txt; exit 1; }
	@for f in elem_param elem_push elem_set elem_slice elem_concat; do \
		./$(BIN) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — елементен инвариант доказан (M3)" \
			|| { echo "FAIL: $$f — очаквах ensures ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@for f in elem_bad elem_set_bad; do \
		./$(BIN) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ensures #1.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — нарушен елементен инвариант е оброчен (M3 soundness)" \
			|| { echo "FAIL: $$f — очаквах ensures ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) --verify examples/verify/elem_slice_bad.baga > /tmp/baga_verify_out.txt; \
	grep -q "граница.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: elem_slice_bad — out-of-bounds достъп е оброчен (M3 bounds)" \
		|| { echo "FAIL: elem_slice_bad — очаквах граница ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
	@for f in sorted_param sorted_push; do \
		./$(BIN) --verify examples/verify/$$f.baga > /tmp/baga_verify_out.txt; \
		grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
			&& echo "OK: $$f — sorted + element axiom (relational M3+)" \
			|| { echo "FAIL: $$f — очаквах ensures ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
	done
	@./$(BIN) --verify examples/verify/sorted_not_le.baga > /tmp/baga_verify_out.txt; true; \
	grep -qE "ensures #1.*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt && ! grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: sorted_not_le — sorted ≠ v[*]<=0 (soundness)" \
		|| { echo "FAIL: sorted_not_le — sorted не трябва да доказва output<=0"; cat /tmp/baga_verify_out.txt; exit 1; }
	@for f in abs_val max2 clamp; do \
		./$(BIN) --test-specs examples/verify/$$f.baga > /dev/null 2>&1 \
			&& echo "OK: $$f — оракулът (--test-specs) съгласен с ДОКАЗАНО" \
			|| { echo "FAIL: $$f — оракулът не е съгласен"; exit 1; }; \
	done
	@echo "=== --verify --json (машинен изход) ==="
	@./$(BIN) --verify --json examples/verify/abs_val.baga > /tmp/baga_verify_json.txt
	@python3 -c "import json; d=json.load(open('/tmp/baga_verify_json.txt')); f=d['functions'][0]; assert f['name']=='abs_val' and f['ensures'][0]['result']=='proven'" \
		&& echo "OK: --verify --json валиден JSON, proven" \
		|| { echo "FAIL: --verify --json"; cat /tmp/baga_verify_json.txt; exit 1; }
	@./$(BIN) --verify --json examples/verify/bad_abs.baga > /tmp/baga_verify_json_bad.txt; test $$? -eq 1 \
		&& python3 -c "import json; d=json.load(open('/tmp/baga_verify_json_bad.txt')); e=d['functions'][0]['ensures'][0]; assert e['result']=='refuted' and e['counterexample']" \
		&& echo "OK: --verify --json refuted + контрапример, exit=1" \
		|| { echo "FAIL: --verify --json refuted"; cat /tmp/baga_verify_json_bad.txt; exit 1; }
	@echo "=== LLVM оракул (C vs lli-14) ==="
	@if [ -f ./$(LLVM_BIN) ]; then $(MAKE) -s test-llvm; else echo "(baga-llvm липсва — пропускам LLVM оракула)"; fi
	@echo ""
	@echo "Всички тестове минаха. ⚔️"
