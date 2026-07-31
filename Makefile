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
	@echo "=== Тест: здравей ==="
	./$(BIN) examples/zdravei.baga
	@echo ""
	@echo "=== Тест: факториел ==="
	./$(BIN) examples/faktorial.baga
	@echo ""
	@echo "=== Тест: генериран C код ==="
	./$(BIN) --emit-c examples/zdravei.baga
