# План: Поправка на бъгове от сондиране (2026-08-02)

## Приоритет 1 — Crash (💥)
1. **Деление/modulo на 0 (i64)** → runtime грешка вместо SIGILL
   - codegen_c.c: guard за `/` и `%` с `if (b == 0) { fprintf(stderr, ...); exit(1); }`
2. **join() с невалиден handle** → runtime грешка вместо segfault
   - codegen_c.c / baga_par_rt.c: NULL check в baga_join

## Приоритет 2 — Memory safety (🔴)
3. **vec_get/vec_set bounds check** → runtime грешка при OOB
   - codegen_c.c: `if (i < 0 || i >= v->len)` guard
4. **char_at bounds check** → runtime грешка при OOB
5. **substr bounds check** → clamp или грешка при OOB start/end
6. **bytes_at bounds check** → runtime грешка при OOB

## Приоритет 3 — Type checker (🟡)
7. **Struct поле грешен тип** → checker.c: проверка на field type в literal
8. **Struct липсващо/extra поле** → checker.c: проверка на field names
9. **void като стойност** → checker.c: забрана let x = f() когато f връща void
10. **let mutability** → checker.c: enforce is_mut при assignment

## Приоритет 4 — Дизайн (🔵) — отделен milestone
11. chr/ord Unicode, memory leaks, mutex/chan диагностика — не е за сега
