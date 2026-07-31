CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c include/baga.h
	$(CC) $(CFLAGS) -c -o $@ $<

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
	@echo ""
	@echo "Всички тестове минаха. ⚔️"
