CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test test-llvm llvm cranelift test-cranelift self

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c include/baga.h
	$(CC) $(CFLAGS) -c -o $@ $<

# LLVM build (optional)
LLVM_CONFIG ?= llvm-config-14
LLVM_CFLAGS := $(shell $(LLVM_CONFIG) --cflags 2>/dev/null) -DBAGA_LLVM
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis target 2>/dev/null) $(LDFLAGS)
LLVM_SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c src/codegen_llvm.c
LLVM_OBJS := $(LLVM_SRCS:.c=.llvm.o)
LLVM_BIN := baga-llvm

llvm: $(LLVM_BIN)

$(LLVM_BIN): $(LLVM_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LLVM_LDFLAGS)

src/%.llvm.o: src/%.c include/baga.h
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c -o $@ $<

# Cranelift build (optional): Rust staticlib (cargo) + C emitter, линкнати заедно.
CRANELIFT_DIR := cranelift
CRANELIFT_LIB := $(CRANELIFT_DIR)/target/release/libbaga_cranelift.a
CRANELIFT_BIN := baga-cranelift

cranelift: $(CRANELIFT_BIN)

$(CRANELIFT_LIB): $(CRANELIFT_DIR)/src/lib.rs $(CRANELIFT_DIR)/Cargo.toml
	cargo build --release --manifest-path $(CRANELIFT_DIR)/Cargo.toml

$(CRANELIFT_BIN): $(CRANELIFT_LIB) include/baga.h src/codegen_cranelift.c $(CRANELIFT_DIR)/baga_clif_rt.h
	$(CC) $(CFLAGS) -DBAGA_CRANELIFT -I$(CRANELIFT_DIR) -o $@ \
	    src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c \
	    src/proofs.c src/codegen_cranelift.c $(CRANELIFT_LIB) -lpthread -ldl -lm

test-cranelift: $(BIN) $(CRANELIFT_BIN)
	@./tests/cranelift_oracle.sh

clean:
	rm -f $(OBJS) $(BIN) $(LLVM_OBJS) $(LLVM_BIN) $(CRANELIFT_BIN)

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
	@echo "=== --test-specs (property-based) ==="
	@./$(BIN) --test-specs examples/spec_ensures.baga
	@./$(BIN) --test-specs examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: --test-specs намери контрапример" \
		|| { echo "FAIL: --test-specs не намери контрапример"; exit 1; }
	@echo "=== LLVM оракул (C vs lli-14) ==="
	@if [ -f ./$(LLVM_BIN) ]; then $(MAKE) -s test-llvm; else echo "(baga-llvm липсва — пропускам LLVM оракула)"; fi
	@echo "=== Cranelift оракул (C vs in-process JIT) ==="
	@if [ -f ./$(CRANELIFT_BIN) ]; then $(MAKE) -s test-cranelift; else echo "(baga-cranelift липсва — пропускам Cranelift оракула)"; fi
	@echo ""
	@echo "Всички тестове минаха. ⚔️"
