# Toolchain only — the C bootstrap of baga/sandak/LLVM/!Par.
# Baga packages (std, app-product/*, apps/*) build with sandak.
# Regression suite: scripts/run_tests.sh (discovery via sandak + baga-test).

CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm -pthread

# import search path for monorepo packages (sandak computes this for packages;
# manual ./baga invocations in tests: repo root for std/, app-product/ for *baga)
BAGAIFLAGS := -I . -I app-product

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c src/verify.c
OBJS := $(SRCS:.c=.o)
BIN  := baga

.PHONY: all clean test test-llvm test-llvm-rc llvm self par-rt sandak docker

all: $(BIN)

sandak: src/sandak.c
	$(CC) $(CFLAGS) -o $@ $<

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

test-llvm-rc: $(BIN) $(LLVM_BIN) $(PAR_SO)
	@./tests/llvm_rc.sh

# Self-hosting bootstrap: fixed point — the self compiler reproduces itself
# (baga2 == baga3 as compilers). baga (C bootstrap) and baga2 (self) are
# different compilers with different codegen, so we compare baga2's output
# (baga_self3.c) with baga3's output (baga_self4.c).
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

# Full regression: language examples, sandak packages, baga-test discovery, --verify.
# Not a package build system — that is sandak.
test: $(BIN) sandak
	@bash scripts/run_tests.sh
