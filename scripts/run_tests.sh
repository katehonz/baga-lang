#!/usr/bin/env bash
# run_tests.sh — full baga regression suite.
#
# Layout (no duplication with the package system):
#   Makefile          → C toolchain only (baga, sandak, llvm, par-rt)
#   sandak            → build every package that has sandak.toml
#   scripts/baga-test → discover and run tests/**/*_test.baga
#   scripts/run_verify.sh → --verify oracle (M0–M23)
#   scripts/self_parity.sh → self-hosting паритет (LP7)
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
echo "=== generics (M21: мономорфизация) ==="
run examples/generics.baga > /tmp/baga_gen_out.txt
grep -q "^здравей$" /tmp/baga_gen_out.txt \
	&& grep -q "^14$" /tmp/baga_gen_out.txt \
	&& echo "OK: generics (извод + явни типови аргументи + транзитивни инстанции)" \
	|| { echo "FAIL: generics"; exit 1; }
echo "=== traits (M23: trait/impl + статичен dispatch) ==="
run examples/traits.baga > /tmp/baga_tr_out.txt
grep -q "^1200$" /tmp/baga_tr_out.txt \
	&& grep -q "^21$" /tmp/baga_tr_out.txt \
	&& echo "OK: traits (методи, вериги, trait bounds на generic fn)" \
	|| { echo "FAIL: traits"; exit 1; }
echo "=== generic_structs (M24: мономорфизация на struct) ==="
run examples/generic_structs.baga > /tmp/baga_gs_out.txt
grep -q "^x$" /tmp/baga_gs_out.txt \
	&& grep -q "^a$" /tmp/baga_gs_out.txt \
	&& echo "OK: generic_structs (извод, swap, методи, Vec от инстанции)" \
	|| { echo "FAIL: generic_structs"; exit 1; }
echo "=== effects_payload (M20: raise/catch с payload) ==="
run examples/effects_payload.baga > /tmp/baga_effp_out.txt
grep -q "празно име" /tmp/baga_effp_out.txt \
	&& grep -q "^7$" /tmp/baga_effp_out.txt \
	&& grep -q "^3.5$" /tmp/baga_effp_out.txt \
	&& echo "OK: effect payloads (raise/catch/? + вериги)" \
	|| { echo "FAIL: effect payloads"; exit 1; }
echo "=== effects_raise_diverge (M20: raise излиза от fn) ==="
run examples/effects_raise_diverge.baga > /tmp/baga_effr_out.txt
grep -q "^hi$" /tmp/baga_effr_out.txt \
	&& grep -q "^7$" /tmp/baga_effr_out.txt \
	&& grep -q "^zero$" /tmp/baga_effr_out.txt \
	&& grep -q "^gone$" /tmp/baga_effr_out.txt \
	&& ! grep -q "NO" /tmp/baga_effr_out.txt \
	&& echo "OK: raise дивергира (няма fall-through, няма страничен ефект след него)" \
	|| { echo "FAIL: raise diverge"; cat /tmp/baga_effr_out.txt; exit 1; }
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
echo "=== noreturn_bad (очакваме compile грешка) ==="
run examples/noreturn_bad.baga 2>&1 | grep -q "може да падне от края без return" \
	&& echo "OK: M19 missing-return хвана падането от края" \
	|| { echo "FAIL: M19 не хвана падането от края"; exit 1; }
echo "=== tail_return (implicit return на последния израз) ==="
run examples/tail_return.baga > /tmp/baga_tail_out.txt \
	&& grep -q "^42$" /tmp/baga_tail_out.txt \
	&& echo "OK: implicit return на последния израз" \
	|| { echo "FAIL: implicit return"; exit 1; }
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
run tests/generic_fn_value_bad.baga 2>&1 | grep -q "не може да се ползва като стойност" \
	&& echo "OK: generic fn като стойност без анотация е честна грешка (M25)" \
	|| { echo "FAIL: generic fn като стойност без анотация не е хванат"; exit 1; }
run tests/generic_spec_bad.baga 2>&1 | grep -q "параметър 'x' е i64 в spec-а, но T" \
	&& echo "OK: spec с i64 срещу generic T е честна грешка (M27)" \
	|| { echo "FAIL: spec с i64 срещу generic T не е хванат"; exit 1; }
run tests/generic_spec_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
	&& echo "OK: spec върху generic хваща runtime нарушение (M27)" \
	|| { echo "FAIL: spec върху generic не хвана runtime нарушение"; exit 1; }
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

echo "=== boilaDB file-size gate (400-line hard limit, ARCHITECTURE.md §9) ==="
bash "$ROOT/app-product/boilaDB/scripts/filesize.sh" \
	&& echo "OK: boilaDB filesize" \
	|| { echo "FAIL: boilaDB filesize"; exit 1; }

echo "=== global file-size gate (600-line hard limit, без boilaDB) ==="
bash "$ROOT/scripts/filesize-global.sh" \
	&& echo "OK: filesize-global" \
	|| { echo "FAIL: filesize-global"; exit 1; }

echo "=== boilaDB layer gate (one-way dependencies, ARCHITECTURE.md §3) ==="
bash "$ROOT/app-product/boilaDB/scripts/deps.sh" \
	&& echo "OK: boilaDB deps" \
	|| { echo "FAIL: boilaDB deps"; exit 1; }

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

echo "=== tls server (baga TLS 1.3 server: loopback + openssl s_client) ==="
# Loopback: a go_bg worker runs tls_accept (std/net/tls_server.baga) while
# main drives the pure-Baga client flight and asserts every step — cert,
# CertificateVerify (RSA-PSS / ECDSA-P256), both Finished HMACs, app echo.
run_tls_server_loopback() {
	local kind=$1 key=$2 cert=$3 suites=${4:-}
	local RC=0
	if [[ -n "$suites" ]]; then
		TLS_CIPHER="$suites" TLSCIPHER="$suites" TLSKEYPATH="$key" TLSCERTPATH="$cert" \
			run tests/tls_server_test.baga > /tmp/baga_tlssrv_lb.txt 2>&1 || RC=$?
	else
		TLSKEYPATH="$key" TLSCERTPATH="$cert" \
			run tests/tls_server_test.baga > /tmp/baga_tlssrv_lb.txt 2>&1 || RC=$?
	fi
	if [[ $RC -eq 0 ]] && grep -q "tls_server_test: all passed" /tmp/baga_tlssrv_lb.txt; then
		echo "OK: TLS 1.3 server handshake ($kind) — baga client verifies baga server"
	else
		echo "FAIL: tls_server_test loopback ($kind)"
		cat /tmp/baga_tlssrv_lb.txt
		exit 1
	fi
}
run_tls_server_loopback "RSA-PSS" /tmp/baga_tls_rsa_key.pem /tmp/baga_tls_rsa_cert.pem
run_tls_server_loopback "ECDSA-P256" /tmp/baga_tls_ec_key.pem /tmp/baga_tls_ec_cert.pem
run_tls_server_loopback "AES_256_GCM_SHA384" /tmp/baga_tls_rsa_key.pem /tmp/baga_tls_rsa_cert.pem 4866
# Third-party wire proof: openssl s_client against the baga server
# (TLSSERVER=1 listener on :18446). Self-signed — s_client reports the
# trust error but must complete the TLSv1.3 handshake and negotiate a suite.
run_tls_sclient() {
	local kind=$1 key=$2 cert=$3
	TLSSERVER=1 TLSKEYPATH="$key" TLSCERTPATH="$cert" \
		run tests/tls_server_test.baga > /tmp/baga_tlssrv_live.txt 2>&1 & echo $! > /tmp/baga_tlssrv.pid
	local up=0
	for _ in $(seq 1 30); do
		if (exec 3<>/dev/tcp/127.0.0.1/18446) 2>/dev/null; then up=1; break; fi
		sleep 1
	done
	if [[ $up -eq 1 ]]; then
		printf 'hello\n' | timeout 60 openssl s_client -connect 127.0.0.1:18446 \
			-tls1_3 > /tmp/baga_sclient.txt 2>&1 || true
		kill "$(cat /tmp/baga_tlssrv.pid)" > /dev/null 2>&1 || true
		wait "$(cat /tmp/baga_tlssrv.pid)" 2>/dev/null || true
		if grep -q "Cipher is TLS_AES" /tmp/baga_sclient.txt; then
			echo "OK: openssl s_client negotiated TLSv1.3 with the baga server ($kind)"
			return 0
		fi
	else
		kill "$(cat /tmp/baga_tlssrv.pid)" > /dev/null 2>&1 || true
		wait "$(cat /tmp/baga_tlssrv.pid)" 2>/dev/null || true
	fi
	echo "FAIL: openssl s_client vs baga TLS server ($kind)"
	cat /tmp/baga_tlssrv_live.txt /tmp/baga_sclient.txt 2>/dev/null
	exit 1
}
run_tls_sclient "RSA" /tmp/baga_tls_rsa_key.pem /tmp/baga_tls_rsa_cert.pem
run_tls_sclient "ECDSA-P256" /tmp/baga_tls_ec_key.pem /tmp/baga_tls_ec_cert.pem

echo "=== boilaDB SSL (SSLRequest → 'S' → TLS 1.3 → PG wire) ==="
# Сървърът отговаря 'S' само при зададени BOILA_TLS_CERT/BOILA_TLS_KEY
# (иначе 'N' — виж другите тестове); клиентът пита само при PGSSLMODE
# (require/prefer) — без него pgbaga остава на историческия plaintext път.
RC=0
PGSSLMODE=require BOILA_TLS_CERT=/tmp/baga_tls_rsa_cert.pem BOILA_TLS_KEY=/tmp/baga_tls_rsa_key.pem \
	run tests/boila_ssl_test.baga > /tmp/baga_boila_ssl_out.txt 2>&1 || RC=$?
if [[ $RC -eq 0 ]] && grep -q "boila_ssl_test: all passed" /tmp/baga_boila_ssl_out.txt; then
	echo "OK: boilaDB SSL — TLS 1.3 transport, simple + extended protocol"
else
	echo "FAIL: boila_ssl_test"
	cat /tmp/baga_boila_ssl_out.txt
	exit 1
fi

echo "=== registry (live Postgres, PORT + PGDATABASE) ==="
# 8000 keeps clear of the crowded framework defaults (8080/8090) and of
# ambient dev servers; override with REGISTRY_PORT when 8000 is taken.
REGISTRY_PORT="${REGISTRY_PORT:-8000}"
PORT="$REGISTRY_PORT" PGDATABASE=baga_registry "$ROOT/scripts/baga-test" tests/registry_test.baga
echo "=== registry gRPC dual (B3, live Postgres) ==="
PORT="$REGISTRY_PORT" PGDATABASE=baga_registry "$ROOT/scripts/baga-test" tests/registry_grpc_test.baga

echo "=== oauth PG (live Postgres) ==="
OAUTH_PG=1 PGDATABASE=baga_oauth "$ROOT/scripts/baga-test" tests/oauth_pg_test.baga
echo "=== oauth PG pool (O7: OAUTH_WORKERS=2, 1 long-lived DB на worker) ==="
OAUTH_PG=1 OAUTH_WORKERS=2 PGDATABASE=baga_oauth "$ROOT/scripts/baga-test" tests/oauth_pg_test.baga

echo "=== baga-test discovery (tests/**/*_test.baga, без specials по-горе) ==="
mapfile -t DISCOVERED < <(
	find "$ROOT/tests" -type f -name '*_test.baga' | sort | while read -r f; do
		base=$(basename "$f")
		case "$base" in
			tls_handshake_test.baga|tls_server_test.baga|https_test.baga|boila_ssl_test.baga|registry_test.baga|registry_grpc_test.baga|oauth_pg_test.baga) continue ;;
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
printf 'import "tests/ns_alias/mod1/util.baga"\nimport "tests/ns_alias/mod2/util.baga" as util2\nfn main() {\n    print(who())\n}\n' > /tmp/baga_ns_alias_amb.baga
run /tmp/baga_ns_alias_amb.baga 2>&1 | grep -q "уточни с util.who или util2.who" \
	&& echo "OK: L6 — alias влиза в подсказката за нееднозначно извикване" \
	|| { echo "FAIL: alias подсказката трябва да гърми"; exit 1; }
printf 'import "tests/ns_alias/mod1/util.baga" as u1\nimport "tests/ns_alias/mod1/util.baga" as u2\nfn main() {}\n' > /tmp/baga_ns_alias_conf.baga
run /tmp/baga_ns_alias_conf.baga 2>&1 | grep -q "вече има import alias 'u1'" \
	&& echo "OK: L6 — втори различен alias за същия файл е грешка" \
	|| { echo "FAIL: конфликтният alias трябва да гърми"; exit 1; }
printf 'import "tests/ns_alias/mod1/util.baga" as\nfn main() {}\n' > /tmp/baga_ns_alias_bad.baga
run /tmp/baga_ns_alias_bad.baga 2>&1 | grep -q "очаквах име на alias след 'as'" \
	&& echo "OK: L6 — as без име е ясна грешка" \
	|| { echo "FAIL: as без име трябва да гърми"; exit 1; }
printf 'import "tests/ns_alias/mod1/util.baga"\nimport "tests/ns_alias/mod2/util.baga" as util2\nfn main() {\n    let r = Rec { v: 1 }\n    print(r.v)\n}\n' > /tmp/baga_ns_struct_amb.baga
run /tmp/baga_ns_struct_amb.baga 2>&1 | grep -q "нееднозначен struct 'Rec'.*уточни с util.Rec или util2.Rec" \
	&& echo "OK: L6 — неуточнен struct при дубликат е грешка с подсказка" \
	|| { echo "FAIL: нееднозначният struct трябва да гърми"; exit 1; }
printf 'struct Pt { x: i64 }\nstruct Pt { y: i64 }\nfn main() {}\n' > /tmp/baga_ns_struct_dup.baga
run /tmp/baga_ns_struct_dup.baga 2>&1 | grep -q "повторна дефиниция на struct 'Pt' в модул" \
	&& echo "OK: L6 — дублиран struct в един модул е checker грешка (не от gcc)" \
	|| { echo "FAIL: дублираният struct трябва да гърми в checker-а"; exit 1; }
printf 'fn main() {\n    let v: Vec<Vec<i64>> = vec_new()\n    print(vec_len(v))\n}\n' > /tmp/baga_lp1_nested_vec.baga
run /tmp/baga_lp1_nested_vec.baga 2>&1 | grep -q "^0$" \
	&& echo "OK: LP1 — Vec<Vec<T>> работи (gap G1 затворен; >> се цепи C++11-стил)" \
	|| { echo "FAIL: вложеният Vec трябва да работи"; exit 1; }
printf 'fn main() {\n    let v: Vec<Vec<i64>> = vec_new()\n    let s: Vec<str> = vec_new()\n    vec_push(v, s)\n}\n' > /tmp/baga_lp1_nested_mix.baga
run /tmp/baga_lp1_nested_mix.baga 2>&1 | grep -q "елемент от тип Vec<str>, но векторът е Vec<Vec<i64>>" \
	&& echo "OK: LP1 — Vec<Vec<i64>> срещу Vec<str> елемент е checker грешка" \
	|| { echo "FAIL: смесеният вложен елемент трябва да гърми"; exit 1; }
printf 'enum Mode { Add, Mul }\nfn take(m: Mode) -> i64 { return m + 1 }\nfn main() -> i64 {\n    print(take(Add))\n    print(take(Mul))\n    let x: i64 = Mul\n    print(x)\n    return 0\n}\n' > /tmp/baga_lp1_enum_i64.baga
run /tmp/baga_lp1_enum_i64.baga 2>&1 | grep -q "^1$" \
	&& echo "OK: LP1 — enum без payload-и е i64-базиран тип (gap G2 затворен, обединени)" \
	|| { echo "FAIL: old-style enum като типов аргумент трябва да работи"; exit 1; }
printf 'fn g(x: i64) -> i64 { return x }\nfn main() { print(g(3.0)) }\n' > /tmp/baga_lp2_mix1.baga
run /tmp/baga_lp2_mix1.baga 2>&1 | grep -q "аргумент #1 е от тип f64, но параметърът е i64" \
	&& echo "OK: LP2 — f64 литерал към i64 параметър е checker грешка" \
	|| { echo "FAIL: f64→i64 аргумент трябва да гърми"; exit 1; }
printf 'fn f(x: f64) -> f64 { return x }\nfn main() { print(f(3)) }\n' > /tmp/baga_lp2_mix2.baga
run /tmp/baga_lp2_mix2.baga 2>&1 | grep -q "аргумент #1 е от тип i64, но параметърът е f64" \
	&& echo "OK: LP2 — i64 литерал към f64 параметър е checker грешка" \
	|| { echo "FAIL: i64→f64 аргумент трябва да гърми"; exit 1; }
printf 'fn main() {\n    let x = 1.5\n    print("${x}")\n}\n' > /tmp/baga_lp2_interp.baga
run /tmp/baga_lp2_interp.baga 2>&1 | grep -q "^1.5$" \
	&& echo "OK: LP2 — f64 в интерполация работи през f64_to_str (%g)" \
	|| { echo "FAIL: f64 интерполацията трябва да работи"; exit 1; }
printf 'fn main() {\n    let m = map_new()\n    map_set(m, 1.5, 1)\n}\n' > /tmp/baga_lp2_mapkey.baga
run /tmp/baga_lp2_mapkey.baga 2>&1 | grep -q "неподдържан ключов тип f64" \
	&& echo "OK: LP2 — f64 като Map ключ е честен отказ" \
	|| { echo "FAIL: f64 map ключ трябва да гърми"; exit 1; }
printf 'fn main() {\n    let s = "a\\0b"\n    print(len(s))\n}\n' > /tmp/baga_lp3_nul.baga
run /tmp/baga_lp3_nul.baga 2>&1 | grep -q "\\\\0 в str литерал" \
	&& echo "OK: LP3 — \\0 в str литерал е compile грешка (не тихо отрязване)" \
	|| { echo "FAIL: \\0 в str трябва да гърми"; exit 1; }
printf 'fn main() {\n    print(ord(65))\n}\n' > /tmp/baga_lp3_ord.baga
run /tmp/baga_lp3_ord.baga 2>&1 | grep -q "'ord': аргумент #1 е от тип i64, но параметърът е str" \
	&& echo "OK: LP3 — builtin arg type check хваща ord(65) преди runtime segfault" \
	|| { echo "FAIL: ord(65) трябва да гърми в checker-а"; exit 1; }
printf 'fn main() {\n    print(len())\n}\n' > /tmp/baga_lp3_arity.baga
run /tmp/baga_lp3_arity.baga 2>&1 | grep -q "'len' очаква 1 аргумент(а), получих 0" \
	&& echo "OK: LP3 — builtin arity check хваща len() без аргументи" \
	|| { echo "FAIL: arity-то на builtin-ите трябва да се проверява"; exit 1; }
printf 'fn main() {\n    print(bytes_len("str"))\n}\n' > /tmp/baga_lp3_bytes_arg.baga
run /tmp/baga_lp3_bytes_arg.baga 2>&1 | grep -q "'bytes_len': аргумент #1 е от тип str, но параметърът е bytes" \
	&& echo "OK: LP3 — str към bytes builtin е checker грешка" \
	|| { echo "FAIL: bytes_len(str) трябва да гърми"; exit 1; }
printf 'fn r(x: i64) -> i64 !A { return x }\nfn main() -> i64 {\n    let v = r(1) catch !A => 10 catch !A => 20\n    print(v)\n    return 0\n}\n' > /tmp/baga_lp4_dup.baga
run /tmp/baga_lp4_dup.baga 2>&1 | grep -q "мъртъв catch" \
	&& echo "OK: LP4 — дублиран catch е грешка (вторият вече е изчистен ефект)" \
	|| { echo "FAIL: дублираният catch трябва да гърми"; exit 1; }
printf 'fn r(x: i64) -> i64 !A { return x }\nfn main() -> i64 {\n    let v = r(1) catch !A => 10 catch !C => 20\n    print(v)\n    return 0\n}\n' > /tmp/baga_lp4_phantom.baga
run /tmp/baga_lp4_phantom.baga 2>&1 | grep -q "мъртъв catch" \
	&& echo "OK: LP4 — catch на ефект, който изразът няма, е грешка (правопис)" \
	|| { echo "FAIL: phantom catch трябва да гърми"; exit 1; }
printf 'fn main() -> i64 {\n    let v = 5 catch !IO => 0\n    print(v)\n    return 0\n}\n' > /tmp/baga_lp4_pure.baga
run /tmp/baga_lp4_pure.baga 2>&1 | grep -q "мъртъв catch" \
	&& echo "OK: LP4 — catch върху чист израз е грешка" \
	|| { echo "FAIL: catch върху чист израз трябва да гърми"; exit 1; }
printf 'fn w(x: i64) -> i64 !IO { return x }\nfn main() -> i64 !Par {\n    let h = go(w, 1)\n    let r = join(h)?\n    print(r)\n    return 0\n}\n' > /tmp/baga_lp4_go.baga
run /tmp/baga_lp4_go.baga 2>&1 | grep -q "необработен ефект !IO" \
	&& echo "OK: LP4 — ефектите на go_bg worker се propagate-ват към callers" \
	|| { echo "FAIL: worker ефектите трябва да се propagate-ват"; exit 1; }
printf 'fn main() {\n    let v: Vec<i64> = vec_new()\n    drop(v)\n    drop(v)\n}\n' > /tmp/baga_lp5_dd.baga
run /tmp/baga_lp5_dd.baga 2>&1 | grep -q "повторен drop на 'v'" \
	&& echo "OK: LP5 — двоен drop е compile грешка (MEM-1)" \
	|| { echo "FAIL: двойният drop трябва да гърми"; exit 1; }
printf 'fn main() {\n    let v: Vec<i64> = vec_new()\n    drop(v)\n    print(vec_len(v))\n}\n' > /tmp/baga_lp5_du.baga
run /tmp/baga_lp5_du.baga 2>&1 | grep -q "използване на 'v' след free" \
	&& echo "OK: LP5 — use-after-drop е compile грешка (MEM-1)" \
	|| { echo "FAIL: use-after-drop трябва да гърми"; exit 1; }
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
printf 'enum Res { Ok(i64), Err(str) }\nfn main() {\n    let v: Vec<Res> = vec_new()\n    vec_push(v, Ok(3))\n    let r = vec_get(v, 0)\n    print(match r { Ok(x) => x, Err(e) => 0 })\n}\n' > /tmp/baga_sum_a2_vec.baga
run /tmp/baga_sum_a2_vec.baga > /tmp/baga_sum_a2_out.txt \
	&& test "$(cat /tmp/baga_sum_a2_out.txt)" = "3" \
	&& echo "OK: A2 — Vec<Res> push/get + match" \
	|| { echo "FAIL: Vec<Res>"; cat /tmp/baga_sum_a2_out.txt 2>/dev/null; exit 1; }
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
# MEM-3 alias / pointer arithmetic inherit region
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let q = p\n    arena_free(a)\n    print(q)\n}\n' > /tmp/baga_arena_alias.baga
run /tmp/baga_arena_alias.baga 2>&1 | grep -q "използване на 'q' след free" \
	&& echo "OK: region — alias q = p умира с арената" \
	|| { echo "FAIL: let q = p трябва да наследи region"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let q = p + 8\n    arena_free(a)\n    print(q)\n}\n' > /tmp/baga_arena_arith.baga
run /tmp/baga_arena_arith.baga 2>&1 | grep -q "използване на 'q' след free" \
	&& echo "OK: region — p + 8 наследява region" \
	|| { echo "FAIL: p + 8 трябва да наследи region"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let mut p = arena_alloc(a, 16)\n    p = p + 8\n    arena_free(a)\n    print(p)\n}\n' > /tmp/baga_arena_readd.baga
run /tmp/baga_arena_readd.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: region — p = p + 8 пази region" \
	|| { echo "FAIL: p = p + 8 трябва да пази region"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let q = 8 + p\n    arena_free(a)\n    print(q)\n}\n' > /tmp/baga_arena_addn.baga
run /tmp/baga_arena_addn.baga 2>&1 | grep -q "използване на 'q' след free" \
	&& echo "OK: region — 8 + p наследява region" \
	|| { echo "FAIL: 8 + p трябва да наследи region"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let q = p - 8\n    arena_free(a)\n    print(q)\n}\n' > /tmp/baga_arena_sub.baga
run /tmp/baga_arena_sub.baga 2>&1 | grep -q "използване на 'q' след free" \
	&& echo "OK: region — p - 8 наследява region" \
	|| { echo "FAIL: p - 8 трябва да наследи region"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let q = if true { p } else { p + 8 }\n    arena_free(a)\n    print(q)\n}\n' > /tmp/baga_arena_if.baga
run /tmp/baga_arena_if.baga 2>&1 | grep -q "използване на 'q' след free" \
	&& echo "OK: region — if с еднакъв region в двата клона" \
	|| { echo "FAIL: if p / p+8 трябва да наследи region"; exit 1; }
# p - q е offset, не указател — не се тагва
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let q = arena_alloc(a, 8)\n    let d = p - q\n    arena_free(a)\n    print(d)\n}\n' > /tmp/baga_arena_diff.baga
run /tmp/baga_arena_diff.baga >/tmp/baga_arena_diff.out 2>/tmp/baga_arena_diff.err
grep -q "използване на 'd' след free" /tmp/baga_arena_diff.err \
	&& { echo "FAIL: p - q не трябва да се тагва като указател"; exit 1; } \
	|| echo "OK: region — p - q е offset, не указател"
# happy: употреба преди free
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let q = p + 8\n    print(q - p)\n    arena_free(a)\n}\n' > /tmp/baga_arena_okarith.baga
test "$(run /tmp/baga_arena_okarith.baga)" = "8" \
	&& echo "OK: region — p+8 е валиден преди arena_free" \
	|| { echo "FAIL: p+8 преди free трябва да върви"; exit 1; }
# MEM-3 handle alias identity
printf 'fn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 16)\n    let b = a\n    arena_free(b)\n    print(p)\n}\n' > /tmp/baga_arena_halias.baga
run /tmp/baga_arena_halias.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: handle alias — free(b) убива p от a" \
	|| { echo "FAIL: arena_free(b) трябва да убие payload от a"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let b = a\n    arena_free(a)\n    arena_free(b)\n}\n' > /tmp/baga_arena_hdfree.baga
run /tmp/baga_arena_hdfree.baga 2>&1 | grep -q "повторен arena_free на 'b'" \
	&& echo "OK: handle alias — двоен free през b" \
	|| { echo "FAIL: arena_free(a)+arena_free(b) трябва да гърми"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let b = a\n    arena_free(a)\n    print(b)\n}\n' > /tmp/baga_arena_huse.baga
run /tmp/baga_arena_huse.baga 2>&1 | grep -q "използване на 'b' след free" \
	&& echo "OK: handle alias — use на b след free(a)" \
	|| { echo "FAIL: print(b) след arena_free(a) трябва да гърми"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let b = a\n    let p = arena_alloc(b, 16)\n    arena_free(a)\n    print(p)\n}\n' > /tmp/baga_arena_halloc.baga
run /tmp/baga_arena_halloc.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: handle alias — alloc през b, free(a) убива p" \
	|| { echo "FAIL: alloc през алиас трябва да се тагва към a"; exit 1; }
printf 'fn main() {\n    let mut b = 0\n    let a = arena_new()\n    b = a\n    let p = arena_alloc(a, 8)\n    arena_free(b)\n    print(p)\n}\n' > /tmp/baga_arena_hasgn.baga
run /tmp/baga_arena_hasgn.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: handle alias — b = a, free(b) убива p" \
	|| { echo "FAIL: assign алиас трябва да споделя identity"; exit 1; }
# MEM-3 fn summary: return arena_alloc(param) + struct lit field
printf 'fn mk(a: i64) -> i64 {\n    return arena_alloc(a, 16)\n}\nfn main() {\n    let a = arena_new()\n    let p = mk(a)\n    arena_free(a)\n    print(p)\n}\n' > /tmp/baga_arena_fn.baga
run /tmp/baga_arena_fn.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: fn summary — mk(a) наследява region на a" \
	|| { echo "FAIL: return arena_alloc(param) трябва да тагва call site"; exit 1; }
printf 'fn mk(a: i64) -> i64 {\n    let p = arena_alloc(a, 16)\n    return p\n}\nfn main() {\n    let a = arena_new()\n    let q = mk(a)\n    arena_free(a)\n    print(q)\n}\n' > /tmp/baga_arena_fn2.baga
run /tmp/baga_arena_fn2.baga 2>&1 | grep -q "използване на 'q' след free" \
	&& echo "OK: fn summary — return p след alloc(param)" \
	|| { echo "FAIL: return p трябва да обобщи region на param"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    let p = mk(a)\n    arena_free(a)\n    print(p)\n}\nfn mk(a: i64) -> i64 {\n    return arena_alloc(a, 8)\n}\n' > /tmp/baga_arena_fnord.baga
run /tmp/baga_arena_fnord.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: fn summary — callee след caller в файла" \
	|| { echo "FAIL: редът на fn не трябва да крие резюмето"; exit 1; }
printf 'struct Buf { p: i64 n: i64 }\nfn main() {\n    let a = arena_new()\n    let s = Buf { p: arena_alloc(a, 8), n: 8 }\n    arena_free(a)\n    print(s.p)\n}\n' > /tmp/baga_arena_struct.baga
run /tmp/baga_arena_struct.baga 2>&1 | grep -q "използване на 's' след free" \
	&& echo "OK: struct lit — поле от arena_alloc тагва s" \
	|| { echo "FAIL: struct поле трябва да наследи region"; exit 1; }
printf 'fn add(x: i64, y: i64) -> i64 { return x + y }\nfn main() {\n    let a = arena_new()\n    let n = add(a, 1)\n    arena_free(a)\n    print(n)\n}\n' > /tmp/baga_arena_add.baga
run /tmp/baga_arena_add.baga >/tmp/baga_arena_add.out 2>/tmp/baga_arena_add.err
grep -q "използване на 'n' след free" /tmp/baga_arena_add.err \
	&& { echo "FAIL: add(a,1) не трябва да се тагва като указател"; exit 1; } \
	|| echo "OK: fn summary — add не е region"
printf 'fn wrap(a: i64) -> i64 { return a }\nfn main() {\n    let a = arena_new()\n    let p = arena_alloc(a, 8)\n    let b = wrap(a)\n    arena_free(b)\n    print(p)\n}\n' > /tmp/baga_arena_hfn.baga
run /tmp/baga_arena_hfn.baga 2>&1 | grep -q "използване на 'p' след free" \
	&& echo "OK: handle fn — wrap(a) споделя identity" \
	|| { echo "FAIL: return a трябва да пренесе handle identity"; exit 1; }
printf 'struct Buf { p: i64 n: i64 }\nfn main() {\n    let a = arena_new()\n    let mut s = Buf { p: 0, n: 0 }\n    s.p = arena_alloc(a, 8)\n    arena_free(a)\n    print(s.p)\n}\n' > /tmp/baga_arena_fasgn.baga
run /tmp/baga_arena_fasgn.baga 2>&1 | grep -q "използване на 's' след free" \
	&& echo "OK: field assign — s.p = arena_alloc тагва s" \
	|| { echo "FAIL: field assign трябва да тагне struct-а"; exit 1; }

echo "=== statement-level { } блок ==="
printf 'fn main() {\n    print("before")\n    {\n        print("inside")\n    }\n    print("after")\n}\n' > /tmp/baga_blk.baga
test "$(run /tmp/baga_blk.baga)" = "$(printf 'before\ninside\nafter')" \
	&& echo "OK: гол блок на statement ниво изпълнява тялото" \
	|| { echo "FAIL: statement блок гълта print"; exit 1; }
printf 'fn main() {\n    let n = { 3 }\n    print(n)\n}\n' > /tmp/baga_blkval.baga
test "$(run /tmp/baga_blkval.baga)" = "3" \
	&& echo "OK: блок като стойност (let n = { 3 })" \
	|| { echo "FAIL: блок като стойност"; exit 1; }

echo "=== MEM-3 --warn-leaks ==="
printf 'fn main() {\n    let v = vec_new()\n    print(vec_len(v))\n}\n' > /tmp/baga_leak_v.baga
run --warn-leaks --check /tmp/baga_leak_v.baga 2>&1 | grep -q "изтичане: 'v' излиза от scope без drop" \
	&& echo "OK: --warn-leaks хваща Vec без drop" \
	|| { echo "FAIL: Vec leak трябва да предупреди"; exit 1; }
printf 'fn main() {\n    let v = vec_new()\n    drop(v)\n}\n' > /tmp/baga_leak_ok.baga
run --warn-leaks --check /tmp/baga_leak_ok.baga 2>&1 | grep -q "изтичане" \
	&& { echo "FAIL: drop-нат Vec не трябва да предупреждава"; exit 1; } \
	|| echo "OK: drop-нат Vec е тих"
printf 'fn mk() -> Vec<i64> {\n    let v = vec_new()\n    return v\n}\nfn main() { let _x = mk() drop(_x) }\n' > /tmp/baga_leak_ret.baga
run --warn-leaks --check /tmp/baga_leak_ret.baga 2>&1 | grep -q "изтичане: 'v'" \
	&& { echo "FAIL: върнат Vec не трябва да предупреждава"; exit 1; } \
	|| echo "OK: return v не е теч"
printf 'fn main() {\n    let a = arena_new()\n}\n' > /tmp/baga_leak_ar.baga
run --check /tmp/baga_leak_ar.baga 2>&1 | grep -q "арена 'a' излиза от scope без arena_free" \
	&& echo "OK: забравена арена е грешка (без --warn-leaks)" \
	|| { echo "FAIL: arena_new без free трябва да е грешка"; exit 1; }
printf 'fn main() {\n    let a = arena_new()\n    arena_free(a)\n}\n' > /tmp/baga_leak_arf.baga
run --check /tmp/baga_leak_arf.baga 2>&1 | grep -q "арена" \
	&& { echo "FAIL: arena_free не трябва да гърми"; exit 1; } \
	|| echo "OK: arena_free е тих"
# без флага — мълчаливо (регресия: не ръси stderr в обичайния път)
run --check /tmp/baga_leak_v.baga 2>&1 | grep -q "изтичане" \
	&& { echo "FAIL: без --warn-leaks не трябва да има предупреждение"; exit 1; } \
	|| echo "OK: без --warn-leaks е мълчаливо"

# ── 6. Static verifier oracle ────────────────────────────────────────────
bash "$ROOT/scripts/run_verify.sh"

# ── 6b. Self-hosting parity (LP7) ────────────────────────────────────────
bash "$ROOT/scripts/self_parity.sh"

# ── 7. Optional LLVM oracle (separate make target; skip if not built) ────
echo "=== LLVM оракул (C vs lli-14) ==="
if [[ -x ./baga-llvm ]]; then
	make -s test-llvm
else
	echo "(baga-llvm липсва — пропускам LLVM оракула; make llvm && make test-llvm)"
fi

echo ""
echo "Всички тестове минаха. ⚔️"
