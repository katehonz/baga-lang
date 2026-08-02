#!/bin/bash
# Evaluation harness — Baga --verify vs CBMC (when installed).
# Usage: bench/run_eval.sh        → fills bench/RESULTS.md
#        sudo apt install cbmc    → adds the CBMC column on the next run.
cd "$(dirname "$0")/.."
BAGA=./baga
OUT=bench/RESULTS.md
HAVE_CBMC=$(command -v cbmc || true)

# name | fixture | category | expected (text) | cbmc twin | cbmc flags
TASKS="
abs_val       | examples/verify/abs_val.baga        | overflow   | proven / refuted (abs INT64_MIN) | bench/cbmc/abs_val.c       | --signed-overflow-check
ovf_add       | examples/verify/ovf_add.baga        | overflow   | proven / bounded proven, unbounded refuted | bench/cbmc/inc_bounded.c | --signed-overflow-check
ovf_mul       | examples/verify/ovf_mul.baga        | overflow   | proven / bounded proven, unbounded refuted | bench/cbmc/mul_bounded.c | --signed-overflow-check
div_zero      | examples/verify/div_zero.baga       | division   | safe proven, unsafe refuted    | bench/cbmc/div_safe.c      | --div-by-zero-check
sum           | examples/verify/sum.baga            | loops      | proven / unknown (unbounded growth) | bench/cbmc/sum_loop.c   | --signed-overflow-check --unwind 8
fact_full     | examples/verify/fact_full.baga      | recursion  | proven+termination / unknown (n*r abstract) | bench/cbmc/fact.c       | --signed-overflow-check --unwind 24
square        | examples/verify/square.baga         | nonlinear  | proven / refuted (n*n overflow) | bench/cbmc/square.c       | --signed-overflow-check
par_join      | examples/verify/par_join.baga       | concurrency| proven / arith mixed (x+1, a+b)    | -                          |
par_detach_bad| examples/verify/par_detach_bad.baga | concurrency| protocol refuted               | -                          |
par_chan      | examples/verify/par_chan.baga       | concurrency| close-then-send proven, recv unknown | -                          |
chan_inv      | examples/verify/chan_inv.baga       | channels   | proven                         | -                          |
chan_inv_par  | examples/verify/chan_inv_par.baga   | channels   | proven (cross-thread)          | -                          |
pair_recv2    | examples/verify/pair_recv2.baga     | channels   | proven (ok-flag discipline)    | -                          |
pair_go       | examples/verify/pair_go.baga        | concurrency| worker unknown (opaque), boss proven | -                          |
loop_havoc    | examples/verify/loop_havoc.baga     | soundness  | honest unknown (no false proof)| -                          |
"

ms() { local s=$(date +%s%N); "$@" > /tmp/eval_out.txt 2>&1; local rc=$?; local e=$(date +%s%N); echo "$(( (e - s) / 1000000 )) $rc"; }

vcol() { # $1=pattern-prefix (ensures|аритметика|протокол) $2=file → verdict word
    if grep -qE "^  $1.*ОБРОЧЕНО" "$2"; then echo "refuted";
    elif grep -qE "^  $1.*НЕ МОГА ДА РЕША" "$2"; then echo "unknown";
    elif grep -qE "^  $1.*ДОКАЗАНО" "$2"; then echo "proven";
    elif [ "$1" = "аритметика" ] && grep -q "операции доказано безопасни)$" "$2" && ! grep -q "идеализирания" "$2"; then echo "proven";
    else echo "—"; fi
}

{
echo "# Evaluation results"
echo
echo "Generated: $(date -Iseconds) on $(uname -srm)"
echo
echo "Verdicts: **ensures** = spec contracts; **arith** = M15/M18 overflow safety; **protocol** = M14 handle protocols."
echo
echo "| Task | Category | Baga ensures | Baga arith | Baga protocol | ms | Expected | CBMC | ms |"
echo "|---|---|---|---|---|---|---|---|---|"
} > "$OUT"

while IFS='|' read -r name fixture cat expected twin flags; do
    [ -z "$name" ] && continue
    name=$(echo $name); fixture=$(echo $fixture); cat=$(echo $cat); expected=$(echo $expected); twin=$(echo $twin); flags=$(echo $flags)
    read bms brc < <(ms $BAGA --verify "$fixture")
    cp /tmp/eval_out.txt /tmp/eval_baga.txt
    ev=$(vcol ensures /tmp/eval_baga.txt)
    av=$(vcol аритметика /tmp/eval_baga.txt)
    pv=$(vcol протокол /tmp/eval_baga.txt)
    if [ -n "$HAVE_CBMC" ] && [ "$twin" != "-" ]; then
        read cms crc < <(ms cbmc "$twin" $flags --verbosity 4)
        if grep -q "FAILED" /tmp/eval_out.txt; then cv="refuted";
        elif grep -q "SUCCESSFUL" /tmp/eval_out.txt; then cv="proven"; else cv="?"; fi
    elif [ "$twin" = "-" ]; then cv="—"; cms="—"
    else cv="(install cbmc)"; cms="—"; fi
    echo "| $name | $cat | $ev | $av | $pv | $bms | $expected | $cv | $cms |" >> "$OUT"
done <<< "$TASKS"

cat "$OUT"
