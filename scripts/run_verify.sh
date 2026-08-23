#!/usr/bin/env bash
# run_verify.sh — static verifier oracle (M0–M23 + proofs + --json).
# Called from scripts/run_tests.sh. Requires ./baga at repo root.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BAGA:-./baga}"
BAGAIFLAGS="${BAGAIFLAGS:--I . -I app-product}"

echo "=== verify (статична верификация, M0–M23) ==="
# C2: raft_term / tpc_decide са чисти продуктови фрагменти (не пълен Raft).
for f in abs_val max2 clamp sum liveness_struct raft_term tpc_decide shr_floor lshr_bounds; do \
	"$BIN" $BAGAIFLAGS --verify examples/verify/$f.baga > /tmp/baga_verify_out.txt || true; \
	grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
		&& echo "OK: $f доказано (completeness)" \
		|| { echo "FAIL: $f — очаквах ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
done
"$BIN" $BAGAIFLAGS --verify examples/verify/bad_abs.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
	&& echo "OK: bad_abs оброчено с контрапример (soundness)" \
	|| { echo "FAIL: bad_abs — очаквах ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/nonlinear.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
	&& echo "OK: nonlinear — x*y>=0 е оброчено с реализируем контрапример (M8b)" \
	|| { echo "FAIL: nonlinear"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/bad_loop.baga > /tmp/baga_verify_out.txt || true; \
grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: bad_loop — грешен инвариант не води до фалшиво доказателство (soundness)" \
	|| { echo "FAIL: bad_loop"; cat /tmp/baga_verify_out.txt; exit 1; }
for f in vec_safe vec_guard vec_param; do \
	"$BIN" $BAGAIFLAGS --verify examples/verify/$f.baga > /tmp/baga_verify_out.txt || true; \
	grep -q "граница.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: $f — достъпът до вектор е доказано в границите (M2)" \
		|| { echo "FAIL: $f — очаквах граница ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
done
"$BIN" $BAGAIFLAGS --verify examples/verify/vec_oob.baga > /tmp/baga_verify_out.txt || true; \
grep -q "граница.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: vec_oob — извън-границите достъп е оброчен (M2 soundness)" \
	|| { echo "FAIL: vec_oob — очаквах граница ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/vec_param_unsafe.baga > /tmp/baga_verify_out.txt || true; \
grep -q "граница.*НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "граница.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: vec_param_unsafe — неохраняван достъп не се доказва (M2 soundness)" \
	|| { echo "FAIL: vec_param_unsafe"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/recursive.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ПРОПУСНАТО" /tmp/baga_verify_out.txt \
	&& echo "OK: recursive — честно пропуснато" \
	|| { echo "FAIL: recursive"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/sum_rec.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: sum_rec — рекурсия доказана с индукционна хипотеза (M5)" \
	|| { echo "FAIL: sum_rec — очаквах ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/sum_rec.baga | grep -q "частична коректност" \
	&& echo "OK: sum_rec — бележка за частична коректност (M5 честност)" \
	|| { echo "FAIL: sum_rec — липсва бележка за частична коректност"; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/bad_rec.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример" /tmp/baga_verify_out.txt \
	&& echo "OK: bad_rec — грешен ensures на рекурсия е оброчен (M5 soundness)" \
	|| { echo "FAIL: bad_rec — очаквах ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/call_req_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "извикване.*ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
	&& echo "OK: call_req_bad — requires при извикване е оброчен с контрапример (M5 soundness)" \
	|| { echo "FAIL: call_req_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/term_dec.baga > /tmp/baga_verify_out.txt || true; \
grep -q "терминация: доказана" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША|частична коректност)" /tmp/baga_verify_out.txt \
	&& echo "OK: term_dec — decreases доказва терминация, пълна коректност (M6)" \
	|| { echo "FAIL: term_dec"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/term_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "терминация.*намалява.*ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "частична коректност" /tmp/baga_verify_out.txt \
	&& echo "OK: term_bad — ненамаляваща мярка е оброчена, ensures остава частична коректност (M6 soundness)" \
	|| { echo "FAIL: term_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/int_exact.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: int_exact — n > 0 ⇒ n >= 1 чрез integer tightening (M7)" \
	|| { echo "FAIL: int_exact"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/int_exact_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 1" /tmp/baga_verify_out.txt \
	&& echo "OK: int_exact_bad — n > 0 ⇏ n >= 2 е оброчен с n = 1 (M7 soundness)" \
	|| { echo "FAIL: int_exact_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/spurious.baga > /tmp/baga_verify_out.txt || true; \
grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: spurious — няма фалшиво оборване през абстрактни стойности (M8 soundness)" \
	|| { echo "FAIL: spurious — очаквах UNKNOWN без ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/fact_full.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && grep -q "терминация: доказана" /tmp/baga_verify_out.txt \
	&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: fact_full — факториел напълно доказан през продуктова аксиома (M8b)" \
	|| { echo "FAIL: fact_full"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/square.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: square — x * x >= 0 без предусловия (M8b)" \
	|| { echo "FAIL: square"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/fact_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
	&& echo "OK: fact_bad — грешно твърдение за продукт е оброчено conclusively (M8b soundness)" \
	|| { echo "FAIL: fact_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/sign_prod.baga > /tmp/baga_verify_out.txt || true; \
grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^4$' \
	&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: sign_prod — пълна знакова таблица за продукти (M9)" \
	|| { echo "FAIL: sign_prod"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/sum_sq.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt && ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: sum_sq — x*x + y*y >= 0 (M9)" \
	|| { echo "FAIL: sum_sq"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/div_const.baga > /tmp/baga_verify_out.txt || true; \
grep -q "half_nonneg" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "half_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: div_const — n/k знак + soundness (M9)" \
	|| { echo "FAIL: div_const"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/mod_const.baga > /tmp/baga_verify_out.txt || true; \
grep -q "mod3" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "mod_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: mod_const — n%k bounds + soundness (M9b)" \
	|| { echo "FAIL: mod_const"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/poly_depth.baga > /tmp/baga_verify_out.txt || true; \
grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -qE '^[4-9]$|^[1-9][0-9]' \
	&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: poly_depth — square dominance + product mono (M10)" \
	|| { echo "FAIL: poly_depth"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/poly_even.baga > /tmp/baga_verify_out.txt || true; \
grep -q "quartic:" /tmp/baga_verify_out.txt && grep -q "sixth:" /tmp/baga_verify_out.txt \
	&& grep -c "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^4$' \
	&& grep -q "quartic_bad:" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
	&& echo "OK: poly_even — n^4/n^6 >= 0 доказани; n^4 >= 1 оброчено при 0 (M20)" \
	|| { echo "FAIL: poly_even"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/poly_consec.baga > /tmp/baga_verify_out.txt || true; \
grep -q "consec:" /tmp/baga_verify_out.txt \
	&& grep -c "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^4$' \
	&& grep -q "consec_bad:" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
	&& grep -q "gap2:" /tmp/baga_verify_out.txt && grep -q "контрапример: n = -1" /tmp/baga_verify_out.txt \
	&& echo "OK: poly_consec — n(n±1)>=0 доказано; n(n+1)>=1 и n(n+2)>=0 оброчени (M21)" \
	|| { echo "FAIL: poly_consec"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/div_mod_id.baga > /tmp/baga_verify_out.txt || true; \
grep -q "rebuild:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "rebuild_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: div_mod_id — n = q*k + r (M10)" \
	|| { echo "FAIL: div_mod_id"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/floor_mul.baga > /tmp/baga_verify_out.txt || true; \
grep -q "floor4" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "floor_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: floor_mul — k*(n/k)<=n (M11)" \
	|| { echo "FAIL: floor_mul"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/complete_sq.baga > /tmp/baga_verify_out.txt || true; \
grep -q "complete_m1" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "too_strong" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: complete_sq — (x±1)^2 (M11)" \
	|| { echo "FAIL: complete_sq"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/var_div.baga > /tmp/baga_verify_out.txt || true; \
grep -q "rebuild_var" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "unsafe_div" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: var_div — n/m, n%m, n=qm+r (M12)" \
	|| { echo "FAIL: var_div"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/amgm.baga > /tmp/baga_verify_out.txt || true; \
grep -c "ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^[2-9]' \
	&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: amgm — (x-y)^2 >= 0 (M12)" \
	|| { echo "FAIL: amgm"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/nonlinear_if.baga > /tmp/baga_verify_out.txt || true; \
grep -q "sq_guard:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "sq_pos_branch" /tmp/baga_verify_out.txt \
	&& grep -q "sq_guard_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: nonlinear_if — products in if-guards (M13)" \
	|| { echo "FAIL: nonlinear_if"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/bitwise_laws.baga > /tmp/baga_verify_out.txt || true; \
grep -q "or_zero" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "bit_lsb" /tmp/baga_verify_out.txt \
	&& grep -q "bit_lsb_bad" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: bitwise_laws — BV identities + n&1 (M13)" \
	|| { echo "FAIL: bitwise_laws"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/bitwise_mask.baga > /tmp/baga_verify_out.txt || true; \
grep -q "and3:" /tmp/baga_verify_out.txt && grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "and_self:" /tmp/baga_verify_out.txt && grep -q "or_allones:" /tmp/baga_verify_out.txt \
	&& grep -q "and_nonneg:" /tmp/baga_verify_out.txt && grep -q "or_nonneg:" /tmp/baga_verify_out.txt \
	&& grep -q "and3_bad:" /tmp/baga_verify_out.txt && grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
	&& echo "OK: bitwise_mask — 2^k-1 маски, идемпотентност, nonneg and/or; n&3>=1 оброчено (M20)" \
	|| { echo "FAIL: bitwise_mask"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/bitwise_xor_not.baga > /tmp/baga_verify_out.txt || true; \
grep -q "xor_not:" /tmp/baga_verify_out.txt && grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "xor_not_plus:" /tmp/baga_verify_out.txt && grep -q "ensures #2.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "xor_not_bad:" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
	&& echo "OK: bitwise_xor_not — n^-1 = -n-1 доказано; ~n>=0 оброчено при 0 (M21)" \
	|| { echo "FAIL: bitwise_xor_not"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/par_join.baga > /tmp/baga_verify_out.txt || true; \
grep -q "par_double:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& ! grep -qE "^  (ensures|извикване|граница|протокол).*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt \
	&& echo "OK: par_join — fork-join детерминизъм: spec на worker през go/join (M14)" \
	|| { echo "FAIL: par_join"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/par_join_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ОБРОЧЕНО" /tmp/baga_verify_out.txt && grep -q "контрапример: n = 0" /tmp/baga_verify_out.txt \
	&& echo "OK: par_join_bad — грешен ensures през join е оброчен (M14 soundness)" \
	|| { echo "FAIL: par_join_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/par_detach_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "протокол (join след detach.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: par_detach_bad — join след detach е статично оброчен (M14 протокол)" \
	|| { echo "FAIL: par_detach_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/par_chan.baga > /tmp/baga_verify_out.txt || true; \
grep -q "send_after_close:" /tmp/baga_verify_out.txt && grep -q "ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "recv_claim:" /tmp/baga_verify_out.txt && grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
	&& echo "OK: par_chan — send след close ⇒ -1 доказано; recv payload честно UNKNOWN (M14)" \
	|| { echo "FAIL: par_chan"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/mem_drop.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ok_seq:" /tmp/baga_verify_out.txt && grep -q "ok_map:" /tmp/baga_verify_out.txt \
	&& grep -c "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt | grep -q '^4$' \
	&& grep -q "протокол (използване след drop.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& grep -q "протокол (повторен drop.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& grep -q "контрапример: n = 1" /tmp/baga_verify_out.txt \
	&& ! grep -q "протокол.*НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
	&& echo "OK: mem_drop — alloc→drop: чистите са доказани; use-after-drop и повторен drop зад if-клон са оброчени с контрапример (MEM-2)" \
	|| { echo "FAIL: mem_drop"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/ovf_add.baga > /tmp/baga_verify_out.txt || true; \
grep -q "inc_bounded:" /tmp/baga_verify_out.txt && grep -q "1/1 операции доказано безопасни" /tmp/baga_verify_out.txt \
	&& grep -q "контрапример: n = 9223372036854775807" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_add — ограниченото събиране е доказано; неограниченото е оброчено при INT64_MAX (M15)" \
	|| { echo "FAIL: ovf_add"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/ovf_mul.baga > /tmp/baga_verify_out.txt || true; \
grep -q "mul_bounded:" /tmp/baga_verify_out.txt && grep -q "1/1 операции доказано безопасни" /tmp/baga_verify_out.txt \
	&& grep -q "аритметика (преливане: (a \* b)): ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_mul — FM граници доказват продукт; неограничен продукт е оброчен (M15)" \
	|| { echo "FAIL: ovf_mul"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/div_zero.baga > /tmp/baga_verify_out.txt || true; \
grep -q "div_safe:" /tmp/baga_verify_out.txt && grep -q "1/1 операции доказано безопасни" /tmp/baga_verify_out.txt \
	&& grep -q "контрапример: n = 0 m = 0" /tmp/baga_verify_out.txt \
	&& echo "OK: div_zero — m >= 1 доказва безопасно деление; без нея m = 0 е оброчен (M15)" \
	|| { echo "FAIL: div_zero"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/loop_havoc.baga > /tmp/baga_verify_out.txt || true; \
grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ensures.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: loop_havoc — няма фалшиво ДОКАЗАНО през стойности от преди цикъла (M15 soundness)" \
	|| { echo "FAIL: loop_havoc"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/abs_val.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "аритметика.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& grep -q "x = -9223372036854775808" /tmp/baga_verify_out.txt \
	&& echo "OK: abs_val — ensures доказан, но abs(INT64_MIN) преливане е хванато (M15)" \
	|| { echo "FAIL: abs_val arith"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/chan_inv.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: chan_inv — съдържателен инвариант: send discharge + recv instantiate (M16)" \
	|| { echo "FAIL: chan_inv"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/chan_inv_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt && ! grep -q "ensures.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: chan_inv_bad — недоказуем payload изпуска аксиомата, recv е честно UNKNOWN (M16)" \
	|| { echo "FAIL: chan_inv_bad"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/chan_inv_par.baga > /tmp/baga_verify_out.txt || true; \
grep -q "boss:" /tmp/baga_verify_out.txt && grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "канален инвариант на 'worker' при извикване.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: chan_inv_par — cross-thread инвариант с discharge при spawn (M16 rely–guarantee)" \
	|| { echo "FAIL: chan_inv_par"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/chan_inv_escape.baga > /tmp/baga_verify_out.txt || true; \
grep -q "boss2:" /tmp/baga_verify_out.txt && grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
	&& ! grep -q "boss2:" -A1 /tmp/baga_verify_out.txt | grep -q "ДОКАЗАНО" \
	&& echo "OK: chan_inv_escape — worker без requires изпуска аксиомата при spawn (M16 drop rule)" \
	|| { echo "FAIL: chan_inv_escape"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/waitfor.baga > /tmp/baga_verify_out.txt || true; \
grep -q "протокол (wait-for: последователни send/recv): ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "протокол (wait-for: join след send): ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "протокол (wait-for: recv след producer): ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "протокол (wait-for цикъл: recv без send): ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& grep -q "протокол (wait-for цикъл: join преди send): ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& ! grep -q "протокол (wait-for.*НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
	&& echo "OK: waitfor — wait-for ацикличност: send/recv и join-след-send доказани; join-преди-send и recv-без-send оброчени (M19)" \
	|| { echo "FAIL: waitfor"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/send_block.baga > /tmp/baga_verify_out.txt || true; \
test "$(grep -c 'протокол (wait-for: send — свободен слот): ДОКАЗАНО' /tmp/baga_verify_out.txt)" -eq 10 \
	&& grep -q "протокол (wait-for: worker send се побира в буфера): ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& test "$(grep -c 'протокол (wait-for цикъл: send върху пълен буфер без consumer): ОБРОЧЕНО' /tmp/baga_verify_out.txt)" -eq 2 \
	&& grep -q "протокол (wait-for цикъл: worker send върху пълен буфер): ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& ! grep -q "протокол (wait-for.*НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
	&& ! grep -A6 "cap_unknown:" /tmp/baga_verify_out.txt | grep -q "свободен слот\|пълен буфер" \
	&& ! grep -A5 "close_unblocks:" /tmp/baga_verify_out.txt | grep -q "пълен буфер" \
	&& echo "OK: send_block — send-blocking върху пълен буфер: cap/credit сметка доказана; двоен send без consumer и worker over-send оброчени; символен cap — честно мълчание (M24)" \
	|| { echo "FAIL: send_block"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/pair_recv2.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: pair_recv2 — ok-flag + content инвариант през cell2 проекции (M17)" \
	|| { echo "FAIL: pair_recv2"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/pair_select.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt && grep -q "ensures #2.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "which_too_high:" /tmp/baga_verify_out.txt && grep -q "НЕ МОГА ДА РЕША" /tmp/baga_verify_out.txt \
	&& echo "OK: pair_select — which ∈ [0,3] доказано; ≤ 2 честно UNKNOWN (M17)" \
	|| { echo "FAIL: pair_select"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/pair_go.baga > /tmp/baga_verify_out.txt || true; \
grep -q "boss:" /tmp/baga_verify_out.txt && grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "requires на 'worker' при извикване.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: pair_go — packed аргумент, requires върху компоненти discharged при spawn (M17)" \
	|| { echo "FAIL: pair_go"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/ovf_eff_safe.baga > /tmp/baga_verify_out.txt \
	&& grep -q "ефект !Overflow: безопасна — типът е точен" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_eff_safe — доказано безопасна, без !Overflow ⇒ типът е точен (M18)" \
	|| { echo "FAIL: ovf_eff_safe"; cat /tmp/baga_verify_out.txt; exit 1; }
rc=0; "$BIN" $BAGAIFLAGS --verify examples/verify/ovf_eff_refuted.baga > /tmp/baga_verify_out.txt || rc=$?; \
test $rc -ne 0 \
	&& grep -q "прелива при n = 9223372036854775807, а !Overflow не е деклариран" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_eff_refuted — недекларирано преливане е нарушение, ненулев exit (M18)" \
	|| { echo "FAIL: ovf_eff_refuted"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/ovf_eff_declared.baga > /tmp/baga_verify_out.txt \
	&& grep -q "ефект !Overflow: деклариран — прелива при" /tmp/baga_verify_out.txt \
	&& ! grep -q "не е деклариран" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_eff_declared — декларираното !Overflow discharge-ва преливането, exit 0 (M18)" \
	|| { echo "FAIL: ovf_eff_declared"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/ovf_eff_unknown.baga > /tmp/baga_verify_out.txt \
	&& grep -q "ефект !Overflow: безопасността не е доказуема — декларирай !Overflow" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_eff_unknown — недоказуема безопасност иска декларация, без провал (M18)" \
	|| { echo "FAIL: ovf_eff_unknown"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/ovf_eff_redundant.baga > /tmp/baga_verify_out.txt \
	&& grep -q "деклариран, но аритметиката е доказано безопасна" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_eff_redundant — излишно, но честно !Overflow върху безопасна функция (M18)" \
	|| { echo "FAIL: ovf_eff_redundant"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --verify examples/verify/ovf_eff_skip.baga > /tmp/baga_verify_out.txt || true; \
grep -q "ПРОПУСНАТО" /tmp/baga_verify_out.txt && ! grep -q "ефект !Overflow" /tmp/baga_verify_out.txt \
	&& echo "OK: ovf_eff_skip — пропусната функция няма ефект !Overflow ред (M18 честност)" \
	|| { echo "FAIL: ovf_eff_skip"; cat /tmp/baga_verify_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS examples/verify/ovf_eff_propagate.baga 2>&1 | grep -q "необработен ефект !Overflow" \
	&& echo "OK: ovf_eff_propagate — !Overflow се разпространява и checker-ът го изисква (M18)" \
	|| { echo "FAIL: ovf_eff_propagate"; exit 1; }
"$BIN" $BAGAIFLAGS --check examples/verify/ovf_eff_propagate_ok.baga | grep -q "ok:" \
	&& echo "OK: ovf_eff_propagate_ok — декларираното !Overflow обработва ефекта (M18)" \
	|| { echo "FAIL: ovf_eff_propagate_ok"; exit 1; }
"$BIN" $BAGAIFLAGS --verify --json examples/verify/ovf_eff_refuted.baga | grep -q '"overflow_effect"' \
	&& "$BIN" $BAGAIFLAGS --verify --json examples/verify/ovf_eff_refuted.baga | grep -q '"result": "refuted"' \
	&& echo "OK: ovf_eff JSON — overflow_effect полето е машинно четимо (M18)" \
	|| { echo "FAIL: ovf_eff JSON"; exit 1; }
"$BIN" $BAGAIFLAGS --proofs examples/verify/ovf_eff_safe.baga > /tmp/baga_proofs_out.txt; \
grep -q "theorem inc_bounded_overflow_safe" /tmp/baga_proofs_out.txt \
	&& grep -q "ДОКАЗАНО (M18" /tmp/baga_proofs_out.txt \
	&& echo "OK: proofs — overflow_safe теорема в --proofs (M18)" \
	|| { echo "FAIL: proofs overflow_safe"; cat /tmp/baga_proofs_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --proofs examples/verify/sum.baga > /tmp/baga_proofs_out.txt; \
grep -q "lemma add_repeated_invariant_1" /tmp/baga_proofs_out.txt \
	&& grep -q "invariant: (s >= 0)" /tmp/baga_proofs_out.txt \
	&& grep -q "ДОКАЗАНО (init + preservation, Hoare)" /tmp/baga_proofs_out.txt \
	&& echo "OK: proofs — верифицирани while инварианти в --proofs" \
	|| { echo "FAIL: proofs invariants"; cat /tmp/baga_proofs_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --proofs examples/verify/fact_full.baga > /tmp/baga_proofs_out.txt; \
grep -q "decreases measure — proven statically (full correctness)" /tmp/baga_proofs_out.txt \
	&& echo "OK: proofs — реална терминация чрез decreases в --proofs" \
	|| { echo "FAIL: proofs termination"; cat /tmp/baga_proofs_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS --proofs examples/verify/bad_loop.baga > /tmp/baga_proofs_out.txt; \
grep -q "НЕ Е ДОКАЗАНА" /tmp/baga_proofs_out.txt \
	&& echo "OK: proofs — недоказан инвариант е отбелязан честно" \
	|| { echo "FAIL: proofs unproven invariant"; cat /tmp/baga_proofs_out.txt; exit 1; }
"$BIN" $BAGAIFLAGS examples/par_select.baga > /tmp/baga_par_sel_out.txt; \
printf "30\n2\n" | diff - /tmp/baga_par_sel_out.txt > /dev/null \
	&& echo "OK: chan_select2_wait/timeout" \
	|| { echo "FAIL: par_select"; cat /tmp/baga_par_sel_out.txt; exit 1; }
for f in elem_param elem_push elem_set elem_slice elem_concat; do \
	"$BIN" $BAGAIFLAGS --verify examples/verify/$f.baga > /tmp/baga_verify_out.txt || true; \
	grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: $f — елементен инвариант доказан (M3)" \
		|| { echo "FAIL: $f — очаквах ensures ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
done
for f in elem_bad elem_set_bad; do \
	"$BIN" $BAGAIFLAGS --verify examples/verify/$f.baga > /tmp/baga_verify_out.txt || true; \
	grep -q "ensures #1.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
		&& echo "OK: $f — нарушен елементен инвариант е оброчен (M3 soundness)" \
		|| { echo "FAIL: $f — очаквах ensures ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
done
"$BIN" $BAGAIFLAGS --verify examples/verify/elem_slice_bad.baga > /tmp/baga_verify_out.txt || true; \
grep -q "граница.*ОБРОЧЕНО" /tmp/baga_verify_out.txt \
	&& echo "OK: elem_slice_bad — out-of-bounds достъп е оброчен (M3 bounds)" \
	|| { echo "FAIL: elem_slice_bad — очаквах граница ОБРОЧЕНО"; cat /tmp/baga_verify_out.txt; exit 1; }
for f in sorted_param sorted_push; do \
	"$BIN" $BAGAIFLAGS --verify examples/verify/$f.baga > /tmp/baga_verify_out.txt || true; \
	grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
		&& echo "OK: $f — sorted + element axiom (relational M3+)" \
		|| { echo "FAIL: $f — очаквах ensures ДОКАЗАНО"; cat /tmp/baga_verify_out.txt; exit 1; }; \
done
"$BIN" $BAGAIFLAGS --verify examples/verify/spec_guarantees.baga > /tmp/baga_verify_out.txt || true; \
grep -q "guarantee #1.*проза" /tmp/baga_verify_out.txt \
	&& grep -q "guarantee #3 (output >= arr): ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& grep -q "guarantee #4 (output < arr + 10): ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: spec_guarantees — M22: проверяеми guarantee-та + честна проза" \
	|| { echo "FAIL: spec_guarantees — M22"; cat /tmp/baga_verify_out.txt; exit 1; }
rc=0; "$BIN" $BAGAIFLAGS --verify examples/verify/sorted_not_le.baga > /tmp/baga_verify_out.txt || rc=$?; \
grep -qE "ensures #1.*(ОБРОЧЕНО|НЕ МОГА ДА РЕША)" /tmp/baga_verify_out.txt && ! grep -q "ensures #1.*ДОКАЗАНО" /tmp/baga_verify_out.txt \
	&& echo "OK: sorted_not_le — sorted ≠ v[*]<=0 (soundness)" \
	|| { echo "FAIL: sorted_not_le — sorted не трябва да доказва output<=0"; cat /tmp/baga_verify_out.txt; exit 1; }
for f in abs_val max2 clamp; do \
	"$BIN" $BAGAIFLAGS --test-specs examples/verify/$f.baga > /dev/null 2>&1 \
		&& echo "OK: $f — оракулът (--test-specs) съгласен с ДОКАЗАНО" \
		|| { echo "FAIL: $f — оракулът не е съгласен"; exit 1; }; \
done
echo "=== --verify --json (машинен изход) ==="
"$BIN" $BAGAIFLAGS --verify --json examples/verify/max2.baga > /tmp/baga_verify_json.txt
python3 -c "import json; d=json.load(open('/tmp/baga_verify_json.txt')); f=d['functions'][0]; assert f['name']=='max2' and f['ensures'][0]['result']=='proven' and f['arith']==[]" \
	&& echo "OK: --verify --json валиден JSON, proven" \
	|| { echo "FAIL: --verify --json"; cat /tmp/baga_verify_json.txt; exit 1; }
rc=0; "$BIN" $BAGAIFLAGS --verify --json examples/verify/bad_abs.baga > /tmp/baga_verify_json_bad.txt || rc=$?; test $rc -eq 1 \
	&& python3 -c "import json; d=json.load(open('/tmp/baga_verify_json_bad.txt')); e=d['functions'][0]['ensures'][0]; assert e['result']=='refuted' and e['counterexample']" \
	&& echo "OK: --verify --json refuted + контрапример, exit=1" \
	|| { echo "FAIL: --verify --json refuted"; cat /tmp/baga_verify_json_bad.txt; exit 1; }
echo "=== LP6 soundness лов (адверсариална батерия) ==="
"$BIN" $BAGAIFLAGS --verify examples/verify/lp6_hunt.baga > /tmp/baga_lp6_out.txt || true; \
for s in lp6_div_neg_bad lp6_mod_neg_bad lp6_lsb_mod2_bad lp6_shr_neg_bad lp6_shr_floor_bad lp6_lshr_neg_bad lp6_lshr_floor_bad lp6_prod_bad lp6_sq_neg_bad; do \
	grep -A2 "$s:" /tmp/baga_lp6_out.txt | grep -q "ensures #1.*ОБРОЧЕНО" \
		&& echo "OK: LP6 $s — фалшивото твърдение е оброчено (не ДОКАЗАНО)" \
		|| { echo "FAIL: LP6 $s — очаквах ОБРОЧЕНО"; cat /tmp/baga_lp6_out.txt; exit 1; }; \
done
for s in lp6_contra_inv lp6_broken_inv; do \
	grep -A2 "$s:" /tmp/baga_lp6_out.txt | grep -q "ensures #1.*НЕ МОГА ДА РЕША" \
		&& echo "OK: LP6 $s — противоречив/неопазен инвариант не доказва нищо (честно UNKNOWN)" \
		|| { echo "FAIL: LP6 $s — очаквах НЕ МОГА ДА РЕША"; cat /tmp/baga_lp6_out.txt; exit 1; }; \
done
grep -A6 "lp6_dec_neg:" /tmp/baga_lp6_out.txt | grep -q "decreases.*>= 0 при входа.*ОБРОЧЕНО" \
	&& grep -A6 "lp6_dec_neg:" /tmp/baga_lp6_out.txt | grep -q "частична коректност" \
	&& ! grep -A6 "lp6_dec_neg:" /tmp/baga_lp6_out.txt | grep -q "терминация: доказана" \
	&& echo "OK: LP6 lp6_dec_neg — неизпълнима decreases мярка е оброчена, без фалшива терминация (M6)" \
	|| { echo "FAIL: LP6 lp6_dec_neg"; cat /tmp/baga_lp6_out.txt; exit 1; }
grep -A2 "lp6_elem_drop:" /tmp/baga_lp6_out.txt | grep -q "ensures #1.*ОБРОЧЕНО" \
	&& grep -A3 "lp6_elem_drop:" /tmp/baga_lp6_out.txt | grep -q "без свидетел" \
	&& echo "OK: LP6 lp6_elem_drop — нарушен елементен инвариант е оброчен; празен контрапример вече е честен (без свидетел)" \
	|| { echo "FAIL: LP6 lp6_elem_drop"; cat /tmp/baga_lp6_out.txt; exit 1; }
grep -A4 "lp6_sendblk_full_bad:" /tmp/baga_lp6_out.txt | grep -q "протокол (wait-for цикъл: send върху пълен буфер без consumer): ОБРОЧЕНО" \
	&& echo "OK: LP6 lp6_sendblk_full_bad — send върху пълен cap-1 буфер без consumer е оброчен, не ДОКАЗАНО (M24)" \
	|| { echo "FAIL: LP6 lp6_sendblk_full_bad — очаквах ОБРОЧЕНО"; cat /tmp/baga_lp6_out.txt; exit 1; }
grep -A3 "lp6_sendblk_worker_bad:" /tmp/baga_lp6_out.txt | grep -q "протокол (wait-for цикъл: worker send върху пълен буфер): ОБРОЧЕНО" \
	&& echo "OK: LP6 lp6_sendblk_worker_bad — join на worker с over-send е оброчен, не ДОКАЗАНО (M24)" \
	|| { echo "FAIL: LP6 lp6_sendblk_worker_bad — очаквах ОБРОЧЕНО"; cat /tmp/baga_lp6_out.txt; exit 1; }
