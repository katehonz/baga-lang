CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c src/verify.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test test-llvm llvm self

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c include/baga.h
	$(CC) $(CFLAGS) -c -o $@ $<

# LLVM build (optional)
LLVM_CONFIG ?= llvm-config-14
LLVM_CFLAGS := $(shell $(LLVM_CONFIG) --cflags 2>/dev/null) -DBAGA_LLVM
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis target 2>/dev/null) $(LDFLAGS)
LLVM_SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c src/verify.c src/codegen_llvm.c
LLVM_OBJS := $(LLVM_SRCS:.c=.llvm.o)
LLVM_BIN := baga-llvm

llvm: $(LLVM_BIN)

$(LLVM_BIN): $(LLVM_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LLVM_LDFLAGS)

src/%.llvm.o: src/%.c include/baga.h
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN) $(LLVM_OBJS) $(LLVM_BIN)

test-llvm: $(BIN) $(LLVM_BIN)
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
	grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: nonlinear — честно НЕ МОГА ДА РЕША" \
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
