#!/usr/bin/env bash
# run_tests.sh — full baga regression suite.
#
# Layout (no duplication with the package system):
#   Makefile          → C toolchain only (baga, sandak, llvm, par-rt)
#   sandak            → build every package that has sandak.toml
#   scripts/baga-test → discover and run tests/**/*_test.baga
#   scripts/run_verify.sh → --verify oracle (M0–M18)
#
# `make test` only builds the toolchain and delegates here.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BAGA:-$ROOT/baga}"
export BAGA="$BIN"
BAGAIFLAGS="-I . -I app-product"
SANDAK="${SANDAK:-$ROOT/sandak}"

if [[ ! -x "$BIN" ]]; then
	echo "run_tests.sh: missing $BIN (run make)" >&2
	exit 127
fi
if [[ ! -x "$SANDAK" ]]; then
	echo "run_tests.sh: missing $SANDAK (run make sandak)" >&2
	exit 127
fi

run() { "$BIN" $BAGAIFLAGS "$@"; }

# ── 1. Language smoke (examples that just run) ───────────────────────────
echo "=== здравей ==="
run examples/zdravei.baga
echo "=== факториел ==="
run examples/faktorial.baga
echo "=== фибоначи ==="
run examples/fib.baga
echo "=== типове ==="
run examples/types.baga
echo "=== struct ==="
run examples/tochka.baga
echo "=== match ==="
run examples/match.baga
echo "=== effects ==="
run examples/effects.baga
echo "=== spec ==="
run examples/spec.baga
run examples/spec_ensures.baga > /dev/null
echo "=== vec_ann (Vec<T> анотации) ==="
run examples/vec_ann.baga

# ── 2. Negative / oracle compiler checks ─────────────────────────────────
echo "=== spec_ensures_fail (очакваме runtime грешка) ==="
run examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
	&& echo "OK: ensures гаранцията е хваната" \
	|| { echo "FAIL: ensures не е хваната"; exit 1; }
echo "=== spec_requires_fail (очакваме runtime грешка) ==="
run examples/spec_requires_fail.baga 2>&1 | grep -q "requires #1 нарушено" \
	&& echo "OK: requires предусловието е хванато" \
	|| { echo "FAIL: requires не е хванато"; exit 1; }
echo "=== vec_typed (очакваме compile грешка) ==="
run examples/vec_typed.baga 2>&1 | grep -q "елемент от тип str, но векторът е Vec<i64>" \
	&& echo "OK: Vec<T> хвана смесването" \
	|| { echo "FAIL: Vec<T> не хвана смесването"; exit 1; }
echo "=== arg_type_bad (очакваме compile грешка) ==="
run examples/arg_type_bad.baga 2>&1 | grep -q "аргумент #1 е от тип str, но параметърът е i64" \
	&& echo "OK: проверката на аргументите хвана грешния тип" \
	|| { echo "FAIL: проверката на аргументите не хвана грешния тип"; exit 1; }
echo "=== emit-c cleanup (регресия: double-free при += desugar) ==="
run --emit-c examples/vec_ann.baga > /dev/null \
	&& echo "OK: --emit-c не гърми върху += desugar" \
	|| { echo "FAIL: --emit-c"; exit 1; }
echo "=== bitwise ==="
run examples/bitwise.baga > /tmp/baga_bitwise_out.txt
printf "2\n7\n5\n16\n16\n24\n9\n4\n16777215\n" | diff - /tmp/baga_bitwise_out.txt > /dev/null \
	&& echo "OK: побитови оператори" \
	|| { echo "FAIL: побитови оператори"; exit 1; }
echo "=== import ==="
run tests/import_main.baga > /tmp/baga_import_out.txt
printf "49\n21\n" | diff - /tmp/baga_import_out.txt > /dev/null \
	&& echo "OK: import + include guard" \
	|| { echo "FAIL: import"; exit 1; }
run tests/import_cycle_a.baga 2>&1 | grep -q "цикличен import" \
	&& echo "OK: import цикълът е хванат" \
	|| { echo "FAIL: import цикълът не е хванат"; exit 1; }
echo "=== -I include path ==="
cp tests/i_flag/main.baga /tmp/baga_i_flag_main.baga
run -I tests/i_flag /tmp/baga_i_flag_main.baga > /tmp/baga_i_flag_out.txt \
	&& test "$(cat /tmp/baga_i_flag_out.txt)" = "42" \
	&& echo "OK: -I include path резолюция" \
	|| { echo "FAIL: -I include path"; exit 1; }
run -Itests/i_flag /tmp/baga_i_flag_main.baga > /dev/null \
	&& echo "OK: -I<dir> слепен вариант" \
	|| { echo "FAIL: -I<dir> слепен вариант"; exit 1; }
echo "=== string interpolation ==="
run examples/interp.baga > /tmp/baga_interp_out.txt
printf 'name=baga n=42 ok=true expr=84\ndollar=$ braces={ } neg=-7\n' | diff - /tmp/baga_interp_out.txt > /dev/null \
	&& echo 'OK: ${expr} интерполация (str/i64/bool/call)' \
	|| { echo "FAIL: интерполация"; cat /tmp/baga_interp_out.txt; exit 1; }
echo "=== bytes type ==="
run examples/bytes.baga > /tmp/baga_bytes_out.txt
printf 'len=4\nat0=222\nhex=deadbeef\nroundtrip=hi\ndec_hex=cafe\ncat_hex=deadbeef00ff\nslice_hex=adbe\n' | diff - /tmp/baga_bytes_out.txt > /dev/null \
	&& echo "OK: bytes тип (hex литерал, len/at/slice/concat, str/hex конверсии)" \
	|| { echo "FAIL: bytes"; cat /tmp/baga_bytes_out.txt; exit 1; }
echo "=== extern fn (FFI) ==="
rm -f /tmp/baga_extern_write.txt
run examples/extern_write.baga | grep -q "written"
test "$(cat /tmp/baga_extern_write.txt)" = "baga ffi works" \
	&& echo "OK: extern fn записва файл" \
	|| { echo "FAIL: extern fn"; exit 1; }
printf 'extern fn bad(v: Vec<i64>) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern.baga
run /tmp/baga_bad_extern.baga 2>&1 | grep -q "неподдържан тип на параметър" \
	&& echo "OK: extern fn типовото ограничение е хванато" \
	|| { echo "FAIL: extern fn типовото ограничение"; exit 1; }
run --emit-c examples/extern_write.baga | grep -v "static void baga_write" | grep -q "baga_write" \
	&& { echo "FAIL: extern write в statement позиция отива към builtin"; exit 1; } \
	|| echo "OK: extern write в statement позиция вика libc"
printf 'extern fn bad(v: void) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern_void.baga
run /tmp/baga_bad_extern_void.baga 2>&1 | grep -q "неподдържан тип на параметър" \
	&& echo "OK: void параметър на extern fn е отхвърлен" \
	|| { echo "FAIL: void параметър на extern fn не е отхвърлен"; exit 1; }
echo "=== arena ==="
run examples/arena.baga > /tmp/baga_arena_out.txt
printf "true\ntrue\narena ok\n" | diff - /tmp/baga_arena_out.txt > /dev/null \
	&& echo "OK: arena алокатор" \
	|| { echo "FAIL: arena"; exit 1; }
echo "=== --test-specs (property-based) ==="
run --test-specs examples/spec_ensures.baga
run --test-specs examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
	&& echo "OK: --test-specs намери контрапример" \
	|| { echo "FAIL: --test-specs не намери контрапример"; exit 1; }
echo "=== --check / --lib (без main, G2) ==="
run --check app-product/httpdbaga/http.baga | grep -q "ok:" \
	&& echo "OK: --check на библиотека без main" \
	|| { echo "FAIL: --check http.baga"; exit 1; }
run --lib app-product/jwtbaga/jwt.baga | grep -q "ok:" \
	&& echo "OK: --lib на jwt.baga" \
	|| { echo "FAIL: --lib jwt.baga"; exit 1; }
run --emit-c app-product/httpdbaga/http.baga 2>/dev/null | grep -q "b_main" \
	&& { echo "FAIL: --emit-c на lib не трябва да емитва b_main"; exit 1; } \
	|| echo "OK: --emit-c на библиотека (без main wrapper)"
run app-product/httpdbaga/http.baga 2>&1 | grep -q "липсва функция 'main'" \
	&& echo "OK: run без main все още изисква main" \
	|| { echo "FAIL: run без main трябва да гърми"; exit 1; }
printf 'fn main() -> i64 {\n    return 7\n}\n' > /tmp/baga_exitcode.baga
rc=0; run /tmp/baga_exitcode.baga > /dev/null 2>&1 || rc=$?
test $rc -eq 7 \
	&& echo "OK: main -> i64 връща exit кода на процеса (kvbaga K3)" \
	|| { echo "FAIL: exit кодът на main се губи"; exit 1; }

# ── 3. sandak (package manager + monorepo packages) ──────────────────────
echo "=== sandak (пакетен мениджър) ==="
SANDAK="$SANDAK" BAGA="$BIN" bash tests/sandak/run_tests.sh

echo "=== sandak build (discovery по sandak.toml) ==="
for d in app-product/*/ apps/*/; do
	[[ -f "$d/sandak.toml" ]] || continue
	name=$(basename "$d")
	(cd "$d" && BAGA="$BIN" "$SANDAK" build > /dev/null) \
		&& echo "OK: sandak build $name" \
		|| { echo "FAIL: sandak build $name"; exit 1; }
done
# bin packages must produce an executable
[[ -x apps/api/target/api ]] \
	&& echo "OK: apps/api target binary" \
	|| { echo "FAIL: apps/api target/api missing"; exit 1; }
[[ -x apps/registry/target/registry ]] \
	&& echo "OK: apps/registry target binary" \
	|| { echo "FAIL: apps/registry target/registry missing"; exit 1; }

# ── 4. Package / product / std tests via baga-test discovery ─────────────
# Specials need env or an external peer; the rest are plain discovery.
echo "=== tls handshake (openssl s_server live: RSA + ECDSA-P256) ==="
run_tls_peer() {
	local kind=$1 key=$2 cert=$3 suites=${4:-}
	local extra=()
	[[ -n "$suites" ]] && extra=(-ciphersuites "$suites")
	openssl s_server -accept 18443 -key "$key" -cert "$cert" \
		-tls1_3 "${extra[@]}" -quiet < /dev/null > /dev/null 2>&1 & echo $! > /tmp/baga_tls_srv.pid
	sleep 1
	local RC=0
	run tests/tls_handshake_test.baga > /tmp/baga_tlshs_out.txt 2>&1 || RC=$?
	kill "$(cat /tmp/baga_tls_srv.pid)" > /dev/null 2>&1 || true
	wait "$(cat /tmp/baga_tls_srv.pid)" 2>/dev/null || true
	if [[ $RC -eq 0 ]] && grep -q "tls_handshake_test: all passed" /tmp/baga_tlshs_out.txt; then
		echo "OK: TLS 1.3 handshake ($kind) — cert + CertificateVerify + Finished"
	else
		echo "FAIL: tls_handshake_test $kind (нужен е openssl s_server на :18443)"
		cat /tmp/baga_tlshs_out.txt
		exit 1
	fi
}
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/baga_tls_rsa_key.pem \
	-out /tmp/baga_tls_rsa_cert.pem -days 2 -subj "/CN=localhost" > /dev/null 2>&1 \
	|| { echo "FAIL: tls RSA cert (нужен е openssl)"; exit 1; }
run_tls_peer "RSA-PSS" /tmp/baga_tls_rsa_key.pem /tmp/baga_tls_rsa_cert.pem
openssl ecparam -name prime256v1 -genkey -noout -out /tmp/baga_tls_ec_key.pem > /dev/null 2>&1 \
	|| { echo "FAIL: tls EC key"; exit 1; }
openssl req -x509 -new -key /tmp/baga_tls_ec_key.pem -out /tmp/baga_tls_ec_cert.pem \
	-days 2 -subj "/CN=localhost" > /dev/null 2>&1 \
	|| { echo "FAIL: tls ECDSA cert"; exit 1; }
run_tls_peer "ECDSA-P256" /tmp/baga_tls_ec_key.pem /tmp/baga_tls_ec_cert.pem
# forced TLS_AES_256_GCM_SHA384 (0x1302): the baga client must switch the
# whole key schedule to SHA-384/HKDF-SHA384 + 32-byte keys
TLSCIPHER=4866 run_tls_peer "AES_256_GCM_SHA384" /tmp/baga_tls_rsa_key.pem /tmp/baga_tls_rsa_cert.pem TLS_AES_256_GCM_SHA384

echo "=== https:// client (openssl -www mock, no real account) ==="
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/baga_https_key.pem \
	-out /tmp/baga_https_cert.pem -days 2 -subj "/CN=localhost" > /dev/null 2>&1 \
	|| { echo "FAIL: https mock cert"; exit 1; }
openssl s_server -accept 18444 -key /tmp/baga_https_key.pem -cert /tmp/baga_https_cert.pem \
	-tls1_3 -www -quiet < /dev/null > /dev/null 2>&1 & echo $! > /tmp/baga_https_srv.pid
sleep 1
RC=0
run tests/std/https_test.baga > /tmp/baga_https_out.txt 2>&1 || RC=$?
kill "$(cat /tmp/baga_https_srv.pid)" > /dev/null 2>&1 || true
wait "$(cat /tmp/baga_https_srv.pid)" 2>/dev/null || true
if [[ $RC -eq 0 ]] && grep -q "https_test: all passed" /tmp/baga_https_out.txt; then
	echo "OK: https:// GET over TLS 1.3 against openssl -www (self-signed mock)"
else
	echo "FAIL: https_test"
	cat /tmp/baga_https_out.txt
	exit 1
fi

echo "=== registry (live Postgres, PORT + PGDATABASE) ==="
PORT="${PORT:-8090}" PGDATABASE=baga_registry "$ROOT/scripts/baga-test" tests/registry_test.baga

echo "=== oauth PG (live Postgres) ==="
OAUTH_PG=1 PGDATABASE=baga_oauth "$ROOT/scripts/baga-test" tests/oauth_pg_test.baga

echo "=== baga-test discovery (tests/**/*_test.baga, без specials по-горе) ==="
mapfile -t DISCOVERED < <(
	find "$ROOT/tests" -type f -name '*_test.baga' | sort | while read -r f; do
		base=$(basename "$f")
		case "$base" in
			tls_handshake_test.baga|https_test.baga|registry_test.baga|oauth_pg_test.baga) continue ;;
			*) echo "$f" ;;
		esac
	done
)
if [[ ${#DISCOVERED[@]} -eq 0 ]]; then
	echo "FAIL: no *_test.baga discovered" >&2
	exit 1
fi
"$ROOT/scripts/baga-test" "${DISCOVERED[@]}"

# ── 5. Extra compiler probes (not *_test.baga) ───────────────────────────
echo "=== par (go/join/chan, !Par) ==="
run examples/par.baga > /tmp/baga_par_out.txt
printf "49\n81\n42\n" | diff - /tmp/baga_par_out.txt > /dev/null \
	&& echo "OK: go/join fan-out" \
	|| { echo "FAIL: par"; cat /tmp/baga_par_out.txt; exit 1; }
run examples/par_chan.baga > /tmp/baga_par_chan_out.txt
printf "240\n" | diff - /tmp/baga_par_chan_out.txt > /dev/null \
	&& echo "OK: chan fan-in" \
	|| { echo "FAIL: par_chan"; cat /tmp/baga_par_chan_out.txt; exit 1; }
run examples/par_pool.baga > /tmp/baga_par_pool_out.txt
printf "385\n" | diff - /tmp/baga_par_pool_out.txt > /dev/null \
	&& echo "OK: pool_map bounded workers" \
	|| { echo "FAIL: par_pool"; cat /tmp/baga_par_pool_out.txt; exit 1; }
run tests/probe_alloc_race.baga > /tmp/baga_race_out.txt
grep -q "total=128032" /tmp/baga_race_out.txt \
	&& echo "OK: конкурентни алокации през global arena (G11 регресия)" \
	|| { echo "FAIL: alloc race"; cat /tmp/baga_race_out.txt; exit 1; }
run examples/par_select.baga > /tmp/baga_par_sel_out.txt
printf "30\n2\n" | diff - /tmp/baga_par_sel_out.txt > /dev/null \
	&& echo "OK: chan_select2_wait/timeout" \
	|| { echo "FAIL: par_select"; cat /tmp/baga_par_sel_out.txt; exit 1; }

echo "=== Map/Vec call-site inference + binary I/O probes ==="
printf 'fn main() {\n    let m: Map<str, i64> = map_new()\n    map_set(m, "a", "текст")\n}\n' > /tmp/baga_map_bad.baga
run /tmp/baga_map_bad.baga 2>&1 | grep -q "стойност от тип str, но картата е Map<str, i64>" \
	&& echo "OK: Map<K,V> стойностен mismatch е отхвърлен" \
	|| { echo "FAIL: map value mismatch трябва да гърми"; exit 1; }
printf 'fn main() {\n    let m = map_new()\n    map_set(m, "k", 1)\n    map_set(m, 2, 3)\n}\n' > /tmp/baga_map_bad2.baga
run /tmp/baga_map_bad2.baga 2>&1 | grep -q "ключ от тип i64, но картата е" \
	&& echo "OK: смесен ключов тип е отхвърлен" \
	|| { echo "FAIL: map key mismatch трябва да гърми"; exit 1; }
printf 'fn p5_fill(v: Vec<str>) { vec_push(v, "abc") }\nfn main() {\n    let xs = vec_new()\n    p5_fill(xs)\n    print(vec_get(xs, 0))\n}\n' > /tmp/baga_vec_infer.baga
test "$(run /tmp/baga_vec_infer.baga)" = "abc" \
	&& echo "OK: неанотиран vec_new се фиксира от параметъра при извикване (tplbaga P5)" \
	|| { echo "FAIL: Vec елементна инференция при извикване"; exit 1; }
printf 'fn p5_put(m: Map<str, str>) { map_set(m, "k", "v") }\nfn main() {\n    let m = map_new()\n    p5_put(m)\n    print(map_get(m, "k"))\n}\n' > /tmp/baga_map_infer.baga
test "$(run /tmp/baga_map_infer.baga)" = "v" \
	&& echo "OK: неанотиран map_new се фиксира от параметъра при извикване (tplbaga P5)" \
	|| { echo "FAIL: Map ключ/стойност инференция при извикване"; exit 1; }
printf 'fn main() {\n    let v: Vec<bytes> = vec_new()\n    vec_push(v, "текст")\n}\n' > /tmp/baga_vec_bad.baga
run /tmp/baga_vec_bad.baga 2>&1 | grep -q "елемент от тип str, но векторът е Vec<bytes>" \
	&& echo "OK: Vec<bytes> елементен mismatch е отхвърлен" \
	|| { echo "FAIL: Vec<bytes> str push трябва да гърми"; exit 1; }
printf 'struct A { x: i64 }\nstruct B { x: i64 }\nfn main() {\n    let v: Vec<A> = vec_new()\n    vec_push(v, B { x: 1 })\n}\n' > /tmp/baga_vec_bad2.baga
run /tmp/baga_vec_bad2.baga 2>&1 | grep -q "елемент от тип B, но векторът е Vec<A>" \
	&& echo "OK: Vec<struct> — различни struct-ове не се смесват (L4)" \
	|| { echo "FAIL: Vec<A> с push на B трябва да гърми"; exit 1; }
printf 'struct A { x: i64 }\nstruct B { x: i64 }\nfn main() {\n    let m: Map<str, A> = map_new()\n    map_set(m, "k", B { x: 1 })\n}\n' > /tmp/baga_map_bad3.baga
run /tmp/baga_map_bad3.baga 2>&1 | grep -q "стойност от тип B, но картата е Map<str, A>" \
	&& echo "OK: Map<str,struct> — различни struct стойности не се смесват (L4)" \
	|| { echo "FAIL: Map<str,A> със set на B трябва да гърми"; exit 1; }
printf 'import "tests/ns_mods/alfa.baga"\nimport "tests/ns_mods/beta.baga"\nfn main() {\n    print(who())\n}\n' > /tmp/baga_ns_amb.baga
run /tmp/baga_ns_amb.baga 2>&1 | grep -q "нееднозначно извикване на 'who'" \
	&& echo "OK: L6 — неуточнено извикване при дубликат е грешка с подсказка" \
	|| { echo "FAIL: нееднозначното извикване трябва да гърми"; exit 1; }
printf 'import "tests/ns_mods/alfa.baga"\nfn main() {\n    print(gama.who())\n}\n' > /tmp/baga_ns_unk.baga
run /tmp/baga_ns_unk.baga 2>&1 | grep -q "непознат модул 'gama'" \
	&& echo "OK: L6 — непознат модул е ясна грешка" \
	|| { echo "FAIL: непознатият модул трябва да гърми"; exit 1; }
printf 'import "tests/ns_mods/alfa.baga"\nfn main() {\n    print(alfa.nope())\n}\n' > /tmp/baga_ns_nofn.baga
run /tmp/baga_ns_nofn.baga 2>&1 | grep -q "модулът 'alfa' няма функция 'nope'" \
	&& echo "OK: L6 — липсваща функция в модул е ясна грешка" \
	|| { echo "FAIL: alfa.nope трябва да гърми"; exit 1; }
printf 'fn dup() -> i64 { return 1 }\nfn dup() -> i64 { return 2 }\nfn main() { print(dup()) }\n' > /tmp/baga_ns_dup.baga
run /tmp/baga_ns_dup.baga 2>&1 | grep -q "повторна дефиниция на функция 'dup'" \
	&& echo "OK: L6 — дубликат в един модул е checker грешка (не от gcc)" \
	|| { echo "FAIL: повторната дефиниция трябва да гърми в checker-а"; exit 1; }
printf 'fn add(a: i64, b: i64) -> i64 { return a + b }\nfn main() {\n    let f = add\n    print(f("x", 1))\n}\n' > /tmp/baga_fn_bad1.baga
run /tmp/baga_fn_bad1.baga 2>&1 | grep -q "fn стойност: аргумент #1 е от тип str, но параметърът е i64" \
	&& echo "OK: L5 — грешен аргумент през fn стойност е отхвърлен" \
	|| { echo "FAIL: fn value arg mismatch трябва да гърми"; exit 1; }
printf 'fn do_io() -> i64 !IO { print("x") return 1 }\nfn main() {\n    let g: fn() -> i64 = do_io\n    print(g())\n}\n' > /tmp/baga_fn_bad2.baga
run /tmp/baga_fn_bad2.baga 2>&1 | grep -q "има ефекти извън анотацията" \
	&& echo "OK: L5 — ефектно несъвместим wrap е отхвърлен" \
	|| { echo "FAIL: !IO в чиста fn анотация трябва да гърми"; exit 1; }
printf 'fn add(a: i64, b: i64) -> i64 { return a + b }\nfn main() {\n    let add = fn (x: i64) -> i64 { return x }\n    print(add(1))\n}\n' > /tmp/baga_fn_bad3.baga
run /tmp/baga_fn_bad3.baga 2>&1 | grep -q "засенчи глобална функция" \
	&& echo "OK: L5 — fn стойност не може да засенчи глобална функция" \
	|| { echo "FAIL: shadowing ban трябва да гърми"; exit 1; }
printf 'fn main() {\n    let x = 5\n    print(x(1))\n}\n' > /tmp/baga_fn_bad4.baga
run /tmp/baga_fn_bad4.baga 2>&1 | grep -q "извикване на не-функция 'x' (i64)" \
	&& echo "OK: L5 — извикване на скалар е ясна грешка" \
	|| { echo "FAIL: извикване на не-функция трябва да гърми"; exit 1; }
echo "=== L3 sum types probes ==="
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let r = Ok(1)\n    print(match r { Ok(v) => v })\n}\n' > /tmp/baga_sum_bad1.baga
run /tmp/baga_sum_bad1.baga 2>&1 | grep -q "не е пълен — липсва вариант 'Err'" \
	&& echo "OK: L3 — непълен match е грешка с името на липсващия вариант" \
	|| { echo "FAIL: непълен match трябва да гърми"; exit 1; }
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let r = Ok("текст")\n}\n' > /tmp/baga_sum_bad2.baga
run /tmp/baga_sum_bad2.baga 2>&1 | grep -q "'Ok': аргументът е от тип str, но payload-ът е i64" \
	&& echo "OK: L3 — грешен payload тип е отхвърлен" \
	|| { echo "FAIL: payload mismatch трябва да гърми"; exit 1; }
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let r = Ok()\n}\n' > /tmp/baga_sum_bad3.baga
run /tmp/baga_sum_bad3.baga 2>&1 | grep -q "конструкторът 'Ok' очаква 1 аргумент" \
	&& echo "OK: L3 — конструктор без аргумент е отхвърлен" \
	|| { echo "FAIL: 0-аргументен конструктор трябва да гърми"; exit 1; }
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let r = Ok\n}\n' > /tmp/baga_sum_bad4.baga
run /tmp/baga_sum_bad4.baga 2>&1 | grep -q "конструкторът 'Ok' изисква 1 аргумент" \
	&& echo "OK: L3 — голяма референция към payload вариант е отхвърлена" \
	|| { echo "FAIL: bare payload variant трябва да гърми"; exit 1; }
printf 'enum A { X(i64) }\nenum B { X(str) }\nfn main() {\n    let a = A::X(1)\n    let b = B::X("s")\n    print(1)\n}\n' > /tmp/baga_sum_a1_ok.baga
run /tmp/baga_sum_a1_ok.baga > /tmp/baga_sum_a1_out.txt \
	&& test "$(cat /tmp/baga_sum_a1_out.txt)" = "1" \
	&& echo "OK: A1 — споделен вариант X между enum-и + A::X / B::X" \
	|| { echo "FAIL: A1 qualified construction"; cat /tmp/baga_sum_a1_out.txt 2>/dev/null; exit 1; }
printf 'enum A { Ok(i64), Err(str) }\nenum B { Ok(str), Err(i64) }\nfn main() { let x = Ok(1) }\n' > /tmp/baga_sum_bad5.baga
run /tmp/baga_sum_bad5.baga 2>&1 | grep -q "нееднозначен" \
	&& echo "OK: A1 — bare Ok при два enum-а е нееднозначен" \
	|| { echo "FAIL: нееднозначният bare Ok трябва да гърми"; exit 1; }
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let v: Vec<Res> = vec_new()\n}\n' > /tmp/baga_sum_bad6.baga
run /tmp/baga_sum_bad6.baga 2>&1 | grep -q "неподдържан елементен тип" \
	&& echo "OK: L3 — Vec<sum enum> е честно отхвърлен (v1)" \
	|| { echo "FAIL: Vec<Res> трябва да гърми"; exit 1; }
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let r = Ok(1)\n    print(match r { Ok(v) => v, Nope(x) => 0, Err(e) => 0 })\n}\n' > /tmp/baga_sum_bad7.baga
run /tmp/baga_sum_bad7.baga 2>&1 | grep -q "патернът 'Nope' не е вариант на enum 'Res'" \
	&& echo "OK: L3 — несъществуващ вариант в патерн е ясна грешка" \
	|| { echo "FAIL: невалиден патерн трябва да гърми"; exit 1; }
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let r = Ok(1, 2)\n}\n' > /tmp/baga_sum_bad8.baga
run /tmp/baga_sum_bad8.baga 2>&1 | grep -q "конструкторът 'Ok' очаква 1 аргумент, получих 2" \
	&& echo "OK: L3 — конструктор с 2 аргумента е отхвърлен" \
	|| { echo "FAIL: 2-аргументен конструктор трябва да гърми"; exit 1; }
printf 'struct A { x: i64 }\nfn main() {\n    let v: Vec<A> = vec_new()\n    vec_push(v, 5)\n}\n' > /tmp/baga_vec_bad3.baga
run /tmp/baga_vec_bad3.baga 2>&1 | grep -q "елемент от тип i64, но векторът е Vec<A>" \
	&& echo "OK: Vec<struct> — скаларен елемент е отхвърлен (L4)" \
	|| { echo "FAIL: Vec<A> с push на i64 трябва да гърми"; exit 1; }
run tests/probe_binary_io.baga > /tmp/baga_bio_out.txt 2>&1 \
	&& grep -q "all passed" /tmp/baga_bio_out.txt \
	&& echo "OK: binary I/O през сокети (NUL/0xFF, chr() UTF-8 капан — G8)" \
	|| { echo "FAIL: probe_binary_io"; cat /tmp/baga_bio_out.txt; exit 1; }
run tests/std/sha_big_probe.baga > /tmp/baga_std_probe.txt \
	&& printf '1310720\ndf0be9d175a152159d1a9c73747a686186eb63b56466d5eed6ad6f540d133aff\n' | diff - /tmp/baga_std_probe.txt > /dev/null \
	&& echo "OK: std/sha256 върху 1.25 MB вход (oracle: hashlib)" \
	|| { echo "FAIL: sha_big_probe"; cat /tmp/baga_std_probe.txt; exit 1; }
printf 'fn main() {\n    print("ел0")\n    print("елa")\n    print("елf9")\n}\n' > /tmp/baga_hexlit.baga
run /tmp/baga_hexlit.baga > /tmp/baga_hexlit_out.txt \
	&& printf 'ел0\nелa\nелf9\n' | diff - /tmp/baga_hexlit_out.txt > /dev/null \
	&& echo "OK: не-ASCII литерал пред ASCII hex цифра (greedy escape регресия)" \
	|| { echo "FAIL: escape на не-ASCII литерал"; cat /tmp/baga_hexlit_out.txt; exit 1; }
printf 'fn main() {\n    let b = bytes_new(2)\n    bytes_set(b, 5, 1)\n}\n' > /tmp/baga_bytes_oob.baga
run /tmp/baga_bytes_oob.baga 2>&1 | grep -q "bytes_set: индекс 5 извън границите" \
	&& echo "OK: S2 — bytes_set извън границите е хванат" \
	|| { echo "FAIL: bytes_set OOB трябва да гърми"; exit 1; }

echo "=== MEM-2: drop seatbelt (checker) ==="
printf 'fn main() {\n    let v = vec_new()\n    drop(v)\n    print(vec_len(v))\n}\n' > /tmp/baga_drop_p1.baga
run /tmp/baga_drop_p1.baga 2>&1 | grep -q "използване на 'v' след free" \
	&& echo "OK: use-after-drop е хванат" \
	|| { echo "FAIL: use-after-drop трябва да гърми"; exit 1; }
printf 'fn main() {\n    let v = vec_new()\n    drop(v)\n    drop(v)\n}\n' > /tmp/baga_drop_p2.baga
run /tmp/baga_drop_p2.baga 2>&1 | grep -q "повторен drop на 'v'" \
	&& echo "OK: повторен drop е хванат" \
	|| { echo "FAIL: повторен drop трябва да гърми"; exit 1; }
printf 'fn main() {\n    let v = vec_new()\n    for i in 0..3 { drop(v) }\n}\n' > /tmp/baga_drop_p3.baga
run /tmp/baga_drop_p3.baga 2>&1 | grep -q "външна за цикъла променлива" \
	&& echo "OK: drop на външна за цикъла променлива е хванат" \
	|| { echo "FAIL: drop в цикъл на външна променлива трябва да гърми"; exit 1; }
printf 'fn f(v: Vec<i64>) { drop(v) }\nfn main() { print(1) }\n' > /tmp/baga_drop_p4.baga
run /tmp/baga_drop_p4.baga 2>&1 | grep -q "drop на параметър 'v'" \
	&& echo "OK: drop на параметър е хванат" \
	|| { echo "FAIL: drop на параметър трябва да гърми"; exit 1; }
printf 'fn main() {\n    let v = vec_new()\n    let f = fn [v] (x: i64) -> i64 { return x }\n    drop(v)\n}\n' > /tmp/baga_drop_p5.baga
run /tmp/baga_drop_p5.baga 2>&1 | grep -q "заснет от ламбда" \
	&& echo "OK: drop на заснета от ламбда променлива е хванат" \
	|| { echo "FAIL: drop на capture трябва да гърми"; exit 1; }
printf 'fn main() {\n    let s = "abc"\n    drop(s)\n}\n' > /tmp/baga_drop_p6.baga
run /tmp/baga_drop_p6.baga 2>&1 | grep -q "неподдържан тип str" \
	&& echo "OK: drop на str е хванат" \
	|| { echo "FAIL: drop на str трябва да гърми"; exit 1; }
printf 'fn main() {\n    drop(vec_new())\n}\n' > /tmp/baga_drop_p7.baga
run /tmp/baga_drop_p7.baga 2>&1 | grep -q "drop очаква локална променлива" \
	&& echo "OK: drop на израз е хванат" \
	|| { echo "FAIL: drop на израз трябва да гърми"; exit 1; }
# if/else join: drop и в двата клона → use след това е грешка
printf 'fn main() {\n    let v = vec_new()\n    vec_push(v, 7)\n    if true { drop(v) } else { drop(v) }\n    print(vec_len(v))\n}\n' > /tmp/baga_drop_pos1.baga
run /tmp/baga_drop_pos1.baga 2>&1 | grep -q "използване на 'v' след free" \
	&& echo "OK: drop в двата if-клона се слива (intersection)" \
	|| { echo "FAIL: drop в двата клона + use трябва да гърми"; exit 1; }
# drop само в един клон → НЕ е сигурно drop-нато, програмата върви
printf 'fn main() {\n    let v = vec_new()\n    vec_push(v, 7)\n    if true { drop(v) }\n    print(vec_len(v))\n}\n' > /tmp/baga_drop_pos2.baga
test "$(run /tmp/baga_drop_pos2.baga)" = "1" \
	&& echo "OK: drop само в един if-клон не е definite — компилира и върви" \
	|| { echo "FAIL: drop в един клон не трябва да блокира"; exit 1; }
printf 'fn main() {\n    let v = vec_new()\n    vec_push(v, 7)\n    if false { print(0) } else { drop(v) }\n    print(vec_len(v))\n}\n' > /tmp/baga_drop_pos3.baga
test "$(run /tmp/baga_drop_pos3.baga)" = "1" \
	&& echo "OK: drop само в else-клон не е definite — компилира и върви" \
	|| { echo "FAIL: drop само в else не трябва да блокира"; exit 1; }
# match/catch join: arm-овете и catch handler-ът са алтернативни пътища
printf 'fn main() {\n    let v = vec_new()\n    vec_push(v, 7)\n    let n = 1\n    let r = match n { 1 => { drop(v) 0 }, _ => 0 }\n    print(vec_len(v))\n}\n' > /tmp/baga_drop_m1.baga
test "$(run /tmp/baga_drop_m1.baga)" = "1" \
	&& echo "OK: drop в един match arm не е definite — компилира и върви" \
	|| { echo "FAIL: drop в един arm не трябва да блокира"; exit 1; }
printf 'fn main() {\n    let v = vec_new()\n    vec_push(v, 7)\n    let n = 1\n    let r = match n { 1 => { drop(v) 0 }, _ => { drop(v) 0 } }\n    print(vec_len(v))\n}\n' > /tmp/baga_drop_m2.baga
run /tmp/baga_drop_m2.baga 2>&1 | grep -q "използване на 'v' след free" \
	&& echo "OK: drop във всички match arm-ове се слива (intersection)" \
	|| { echo "FAIL: drop във всички arm-ове + use трябва да гърми"; exit 1; }
printf 'fn f() -> i64 !IO { print("f") return 1 }\nfn main() {\n    let v = vec_new()\n    vec_push(v, 7)\n    let r = f() catch !IO => { drop(v) 0 }\n    print(vec_len(v))\n}\n' > /tmp/baga_drop_c1.baga
test "$(run /tmp/baga_drop_c1.baga)" = "$(printf 'f\n1')" \
	&& echo "OK: drop в catch handler не е definite — компилира и върви" \
	|| { echo "FAIL: drop в catch handler не трябва да блокира"; exit 1; }

echo "=== MEM-3: arena_free seatbelt (checker) ==="
printf 'fn main() {\n    let a = arena_new()\n    arena_free(a)\n    arena_free(a)\n}\n' > /tmp/baga_arena_p1.baga
run /tmp/baga_arena_p1.baga 2>&1 | grep -q "повторен arena_free на 'a'" \
	&& echo "OK: повторен arena_free е хванат" \
	|| { echo "FAIL: повторен arena_free трябва да гърми"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    arena_free(a)\n    let p = arena_alloc(a, 8)\n    print(p)\n}\n' > /tmp/baga_arena_p2.baga
run /tmp/baga_arena_p2.baga 2>&1 | grep -q "използване на арена 'a' след arena_free" \
	&& echo "OK: arena_alloc след free е хванат" \
	|| { echo "FAIL: arena_alloc след free трябва да гърми"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    arena_free(a)\n    arena_reset(a)\n}\n' > /tmp/baga_arena_p3.baga
run /tmp/baga_arena_p3.baga 2>&1 | grep -q "използване на арена 'a' след arena_free" \
	&& echo "OK: arena_reset след free е хванат" \
	|| { echo "FAIL: arena_reset след free трябва да гърми"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    arena_free(a)\n    print(a)\n}\n' > /tmp/baga_arena_p4.baga
run /tmp/baga_arena_p4.baga 2>&1 | grep -q "използване на 'a' след free" \
	&& echo "OK: use на free-ната арена е хванат" \
	|| { echo "FAIL: use на free-ната арена трябва да гърми"; exit 1; }
# happy path still works
run examples/arena.baga > /tmp/baga_arena_mem3.txt
printf "true\ntrue\narena ok\n" | diff - /tmp/baga_arena_mem3.txt > /dev/null \
	&& echo "OK: arena happy path (MEM-3 regress)" \
	|| { echo "FAIL: arena happy path"; exit 1; }
# MEM-3 region: free arena invalidates arena_alloc results
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    arena_free(a)\n    print(p)\n}\n' > /tmp/baga_arena_reg1.baga
run /tmp/baga_arena_reg1.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: region — use of arena_alloc result after free е хванат" \
	|| { echo "FAIL: p след arena_free(a) трябва да гърми"; exit 1; }
# MEM-3 mut rebind region
printf 'fn main() {\n    let a = arena_new()\n    let mut p = arena_alloc(a, 8)\n    let b = arena_new()\n    p = arena_alloc(b, 8)\n    arena_free(a)\n    print(p)\n    arena_free(b)\n}\n' > /tmp/baga_arena_reg2.baga
test "$(run /tmp/baga_arena_reg2.baga 2>/dev/null)" != "" \
	&& echo "OK: mut rebind — free of old arena не убива rebind-натия p" \
	|| { echo "FAIL: mut rebind region"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let mut p = arena_alloc(a, 8)\n    p = arena_alloc(a, 8)\n    arena_free(a)\n    print(p)\n}\n' > /tmp/baga_arena_reg3.baga
run /tmp/baga_arena_reg3.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: mut rebind same arena — free still kills p" \
	|| { echo "FAIL: rebind same arena + free"; exit 1; }

# ── 6. Static verifier oracle ────────────────────────────────────────────
bash "$ROOT/scripts/run_verify.sh"

# ── 7. Optional LLVM oracle (separate make target; skip if not built) ────
echo "=== LLVM оракул (C vs lli-14) ==="
if [[ -x ./baga-llvm ]]; then
	make -s test-llvm
else
	echo "(baga-llvm липсва — пропускам LLVM оракула; make llvm && make test-llvm)"
fi

echo ""
echo "Всички тестове минаха. ⚔️"
