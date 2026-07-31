CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test llvm

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
	rm -f $(OBJS) $(BIN)

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
	@echo "=== --test-specs (property-based) ==="
	@./$(BIN) --test-specs examples/spec_ensures.baga
	@./$(BIN) --test-specs examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: --test-specs намери контрапример" \
		|| { echo "FAIL: --test-specs не намери контрапример"; exit 1; }
	@echo ""
	@echo "Всички тестове минаха. ⚔️"
