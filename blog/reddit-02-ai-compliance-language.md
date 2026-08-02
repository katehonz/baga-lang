# Microsoft just validated the spec-first thesis for AI coding. Baga is what it looks like when the spec is a language construct and the compiler enforces compliance — statically, with counterexamples.

A follow-up to [my earlier post about Baga](https://git.bara-lang.org/baga-lang-ai/baga-lang-ai) — the language where the compiler statically proves AI-written code against specs. This one is about why the timing stopped being a matter of opinion.

## The industry converged on the diagnosis

Microsoft now officially promotes **Spec-Driven Development (SDD)** as *the foundation of AI-native engineering*. The argument, from [Apoorv Gupta, Principal Software Engineer at Microsoft](https://developer.microsoft.com/blog/spec-driven-development-ai-native-engineering/):

- The core problem of AI-native development is the **loss of intent** — between needs, requirements, architecture, implementation, and validation.
- The fix is to make the specification the **shared source of truth** for humans and AI: "align first" instead of "prompt first, fix later."
- Around this, Microsoft ships **GitHub Spec Kit** (open source): Constitution → Specify → Clarify → Plan → Tasks → Implement → Validate.

When a principal engineer at Microsoft writes the same thing that is pillar #1 of your language, the thesis no longer needs defending. It needs executing.

## But SDD and Baga solve the problem at different levels

SDD attacks the intent-loss problem at the level of **process and AI tooling**: the spec is a *document*, and conformance is checked by tests and human review in a Validate step. That fixes the workflow around the agents.

Baga makes the stronger move: the spec is a **language construct**, and conformance is a **compile-time judgement**.

```baga
spec sum_to {
    input:
        n: i64
    output: i64
    requires:
        n >= 0
    ensures:
        0 <= output
    decreases:
        n
}
```

The human writes this. The AI writes the implementation. The compiler **proves or refutes it, statically, before anything runs**:

```
verify sum_to:
  ensures #1 (0 <= output): ДОКАЗАНО          # PROVEN
  (терминация: доказана чрез decreases — пълна коректност)
```

And when the AI gets it wrong, it doesn't get a failing test or a code-review comment three hours later. It gets a **refutation with a concrete counterexample, at compile time**:

```
verify bad_abs:
  ensures #1 (output >= 0): ОБРОЧЕНО           # REFUTED
    контрапример: x = -1
```

In the SDD spectrum — **spec-first → spec-anchored → spec-as-source** — Baga is the far end: *spec-as-source*. The specification is the thing the code is judged against, mechanically.

## The compliance technology

That word — compliance — is the point. Every AI-coding stack today has the same shape: an LLM generates code, and then something checks whether the code complies with what was intended. The "something" is usually:

- tests (incomplete by construction — they sample the input space),
- another LLM (an LLM judging an LLM — circular),
- a human (the bottleneck we were trying to remove).

Baga's answer is a small, auditable verifier: Fourier–Motzkin elimination over the rationals + symbolic execution + Hoare rules, **sound by construction**. The only path to PROVEN is showing the negated obligation is unsatisfiable even over the rationals, which implies unsatisfiable over the integers. Anything outside the fragment is honestly reported UNKNOWN, never falsely proven. Every reported counterexample is re-checked by direct evaluation, and passes a **conclusiveness gate**: the reported inputs must violate the contract for *every* value of the verifier's internal abstract variables — otherwise the answer is UNKNOWN, not a false alarm.

And because the consumer is an agent, the judge has a machine API:

```
$ ./baga --verify --json bad_abs.baga
{"functions": [{"name": "bad_abs", "ensures": [{"text": "output >= 0",
  "result": "refuted", "counterexample": [{"name": "x", "value": -1}]}], ...}]}
```

The compliance loop writes itself: agent emits code → `baga --verify --json` → refuted with counterexample → agent fixes → PROVEN. Deterministic, fast, no LLM in the judging seat.

This isn't an SMT black box either. The verifier covers linear arithmetic (integer-exact — `n > 0 ⇒ n >= 1` proves), `while` loops with invariants, array bounds, element invariants, recursion via assume–guarantee, full correctness via `decreases`, and products of linear forms (`x*x >= 0`; `fa >= 1 ∧ fb >= 1 ⇒ fa*fb >= 1`). The flagship is **factorial fully proven** — recursive, non-linear, with termination, no SMT solver anywhere:

```baga
spec fact {
    input: n: i64
    output: i64
    requires: n >= 0
    ensures: output >= 1
    decreases: n
}

fn fact(n: i64) -> i64 {
    if n <= 0 { return 1 }
    let r = fact(n - 1)   // induction hypothesis: r >= 1
    return n * r          // n >= 1, r >= 1 ⇒ n * r >= 1
}
```

## Why "the first language for AI" is a claim about architecture, not marketing

Every mainstream language was designed for a human writer and a human reader. AI broke that assumption: the writer is now a machine, and the scarce resource is **trust**. A language for this era needs:

1. **Specs as first-class citizens** — the intent lives in the code, not in a wiki page that drifts.
2. **A mechanical judge** — the compiler proves or refutes compliance, statically, with witnesses.
3. **Errors visible in the type** — effects (`str !IO !NotFound` is a different type from `str`) so the failure surface is part of the signature, not a runtime surprise.
4. **Machine-readable verdicts** — `--json` so the agent closes the loop itself.

Baga has all four. SDD gives you (1) as a process discipline. Baga gives you (1)–(4) as a compilation.

## Honest status

Working prototype, not a production language. The verifier's fragment is deliberately small and says UNKNOWN rather than guessing; general non-linear arithmetic is the remaining staircase. Effects are compile-time only (erased in codegen). The trust story is engineered in, not hoped for: the compiler self-hosts with a byte-for-byte fixed point (`make self`), the LLVM backend is diffed against the C backend on every example, and `--test-specs` property-tests every contract the static verifier calls PROVEN. All of it is one command and wired into CI.

Testable in five minutes:

```
make && ./baga --verify examples/verify/fact_full.baga
```

## Links

- Source: [git.bara-lang.org](https://git.bara-lang.org/baga-lang-ai/baga-lang-ai)
- The SDD reference: [Spec-Driven Development: the foundation of AI-native engineering](https://developer.microsoft.com/blog/spec-driven-development-ai-native-engineering/) (Apoorv Gupta, Microsoft)
- Docs: theory, language reference, compiler architecture — English and Bulgarian
- License: MIT

---

*The industry agreed on the diagnosis: the spec is the center. The open question is whether conformance stays a human ritual in a Validate step — or becomes a compile-time judgement. That's the difference between anchoring code to a spec and making the anchor mechanical.*

🐆
