# Предусловия в spec (`requires:`) — Дизайн

> Дата: 2026-07-31. Статус: одобрен за планиране (auto mode).
> Продължение на `docs/superpowers/specs/2026-07-31-executable-spec-ensures-design.md`.
> След като `ensures:` (пост-условия) вече работи, `requires:` затваря договора:
> ensures = какво обещава функцията; requires = какво се изисква от викащия.

## Синтаксис

```baga
spec корен {
    input:
        x: i64
    output: i64
    requires:               # предусловия — проверяват се ПРЕДИ тялото
        x >= 0
    ensures:                # пост-условия — след тялото (вече съществува)
        output >= 0
}
```

- Същите правила като `ensures:`: булеви изрази на Бага, разделени със запетаи,
  излишна запетая в края е позволена, секциите са в произволен ред.
- Scope на изразите: **само spec input имената** (`output` не съществува преди
  повикването — използването му дава `недефинирана променлива 'output'`).
- За разлика от `ensures`, `requires` е позволен и върху void функции.

## Семантика

### Компилация (checker, pass 2)
- Всеки requires израз се тип-проверява в scope от spec input имената;
  резултатът трябва да е `bool`, иначе:
  `spec '<име>': requires #N е <тип>, очаквах bool`.

### Изпълнение (codegen C)
- Wrapper-ът (същият механизъм от ensures) проверява requires **преди**
  повикването на impl, после ensures след него. Wrapper се генерира, когато
  spec-ът има requires ИЛИЛИ ensures (за void функции — само requires).
- `baga_spec_fail` получава параметър `kind`:

```c
static void baga_spec_fail(const char *spec, const char *kind, int64_t idx, const char *expr) {
    if (strcmp(kind, "requires") == 0)
        fprintf(stderr, "spec '%s': requires #%lld нарушено: %s\n", spec, (long long)idx, expr);
    else
        fprintf(stderr, "spec '%s': ensures #%lld нарушена: %s\n", spec, (long long)idx, expr);
    exit(1);
}
```

(Съществуващото ensures съобщение остава същото — Makefile тестът grep-ва
`ensures #1 нарушена`.)

### `--proofs` / `--specs`
- requires редовете се печатат като `requires: <текст>`; статус
  `RUNTIME-CHECKED` важи, когато има requires ИЛИ ensures.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `include/baga.h` | `NodeVec spec_requires;` в NODE_SPEC (преизползва NODE_ENSURE възли) |
| `src/parser.c` | секция `requires:` (същия кодов път като ensures); „requires" в stop-листите на input-секцията и guarantees gobbler-а; node_free; AST printer |
| `src/checker.c` | pass 2: тип-проверка на requires в scope само с input имената |
| `src/codegen_c.c` | `baga_spec_fail` с `kind`; `find_ensures_spec` → обобщен `find_contract_spec`; wrapper: requires преди impl, void-съвместим |
| `src/proofs.c`, `src/main.c` | печат на requires; RUNTIME-CHECKED |
| `examples/spec_ensures.baga` | + `requires: n >= 0` |
| `examples/spec_requires_fail.baga` | нов: нарушено предусловие → runtime грешка |
| `Makefile` | test: прогон за requires failure |
| `docs/language-bg.md`, `docs/language-en.md` | §14.4 + ред в таблицата с грешки |

## Извън обхвата

- Статично доказване на requires във викащия (само runtime проверка).
- Invariant-и на struct-ове; subtyping на contracts.
- LLVM backend и self-hosted компилатор — както досега.

## Тестване

- Позитивен: `spec_ensures.baga` с `requires: n >= 0` → работи.
- Негативен (runtime): `spec_requires_fail.baga` →
  `spec 'корен': requires #1 нарушено: x >= 0`, exit 1, преди изпълнение на тялото.
- Негативен (compile-time): не-булев requires израз → грешка от checker-а.
- Регресия: `make test` (включително стария ensures probe — съобщението е непроменено).
