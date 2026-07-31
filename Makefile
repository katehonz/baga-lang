CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test test-llvm llvm

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

clean:
	rm -f $(OBJS) $(BIN) $(LLVM_OBJS) $(LLVM_BIN)

test-llvm: $(BIN) $(LLVM_BIN)
	@./tests/llvm_oracle.sh

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
	@echo "=== --test-specs (property-based) ==="
	@./$(BIN) --test-specs examples/spec_ensures.baga
	@./$(BIN) --test-specs examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: --test-specs намери контрапример" \
		|| { echo "FAIL: --test-specs не намери контрапример"; exit 1; }
	@echo "=== LLVM оракул (C vs lli-14) ==="
	@if [ -f ./$(LLVM_BIN) ]; then $(MAKE) -s test-llvm; else echo "(baga-llvm липсва — пропускам LLVM оракула)"; fi
	@echo ""
	@echo "Всички тестове минаха. ⚔️"
