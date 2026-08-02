# Evaluation results

Generated: 2026-08-02T18:53:19+03:00 on Linux 6.1.0-51-amd64 x86_64

Verdicts: **ensures** = spec contracts; **arith** = M15/M18 overflow safety; **protocol** = M14 handle protocols.

| Task | Category | Baga ensures | Baga arith | Baga protocol | ms | Expected | CBMC | ms |
|---|---|---|---|---|---|---|---|---|
| abs_val | overflow | proven | refuted | — | 2 | proven / refuted (abs INT64_MIN) | (install cbmc) | — |
| ovf_add | overflow | proven | refuted | — | 2 | proven / bounded proven, unbounded refuted | (install cbmc) | — |
| ovf_mul | overflow | proven | refuted | — | 2 | proven / bounded proven, unbounded refuted | (install cbmc) | — |
| div_zero | division | refuted | refuted | — | 2 | safe proven, unsafe refuted | (install cbmc) | — |
| sum | loops | proven | unknown | — | 184 | proven / unknown (unbounded growth) | (install cbmc) | — |
| fact_full | recursion | proven | unknown | — | 2 | proven+termination / unknown (n*r abstract) | (install cbmc) | — |
| square | nonlinear | proven | refuted | — | 2 | proven / refuted (n*n overflow) | (install cbmc) | — |
| par_join | concurrency | proven | refuted | — | 6 | proven / arith mixed (x+1, a+b) | — | — |
| par_detach_bad | concurrency | unknown | — | refuted | 1 | protocol refuted | — | — |
| par_chan | concurrency | unknown | unknown | — | 2 | close-then-send proven, recv unknown | — | — |
| chan_inv | channels | proven | — | — | 2 | proven | — | — |
| chan_inv_par | channels | proven | — | — | 2 | proven (cross-thread) | — | — |
| pair_recv2 | channels | proven | — | — | 2 | proven (ok-flag discipline) | — | — |
| pair_go | concurrency | unknown | — | — | 2 | worker unknown (opaque), boss proven | — | — |
| loop_havoc | soundness | unknown | unknown | — | 8 | honest unknown (no false proof) | — | — |
