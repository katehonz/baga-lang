# RC + drop паритет в LLVM бекенда — имплементационен план

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `./baga-llvm --emit-llvm --rc` да дава същото RC поведение като C бекенда и `drop(x)` да работи в LLVM бекенда.

**Architecture:** Огледален порт на C RC модела (spec: `docs/superpowers/specs/2026-08-21-llvm-rc-parity-design.md`). Под `--rc` LLVM heap алокациите получават 32 B `baga_Hdr { magic, pe, rc, an }` пред payload; retain/release helper-ите се емитират като lazy LLVM IR функции (същия `baga_rt`/`h_begin` механизъм, който LLVM бекендът вече ползва); нов scope стек в `codegen_llvm.c` следи heap локали и release-ва при изход от scope.

**Tech Stack:** C99, LLVM 14 C API (`llvm-config-14`), `lli-14` за изпълнение на IR, bash тестови скриптове.

## Global Constraints

- Без `--rc` LLVM изходът (IR текст и поведение) трябва да е **побайтово непроменен** — `tests/llvm_oracle.sh` минава преди и след всяка задача.
- Header layout: 32 B `{ uint64_t magic; uint64_t pe; uint64_t rc; uint64_t an; }`, magic = `0xBA6A4D454D344848ULL` (като `src/codegen_c.c:5284,5291`). `pe = epoch<<1|persist`; в LLVM v1 epoch=0, persist=0 винаги (няма arena/mem_rewind — те остават `unsupported`).
- rc=1 при конструкция; rc=0 → free на базата (payload − 32).
- Повторен drop / release на dead binding → чиста rc-underflow грешка на stderr + `exit(1)`, не UB.
- Не-цели: RC2/RC3 move elision, RC4 temp регистър, closure capture retain, цикли (няма weak refs) — същите или по-тесни граници от C; temp-овете под LLVM `--rc` текат (като C преди RC4), документира се.
- `mem_mark`/`mem_rewind`/`mem_persist_*` остават `llvm_unsupported` (`src/codegen_llvm.c:4161-4165`).
- Коментарите в кода — на български, в стила на файла.
- Build: `make llvm` (създава `baga-llvm` + `lib/libbaga_par.so`). Пълен gate след всяка задача: `make test` и `make test-llvm` зелени.
- Пускане на LLVM тест: `./baga-llvm --emit-llvm --rc FILE.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll`.

## Ключови факти за codebase-а (прочетени, не ги търси наново)

- `src/codegen_llvm.c` (5235 реда): глобално състояние `static LLVMCodegen lg;` (`:65`); `llvm_unsupported()` прави честен compile-time отказ с exit (`:69`); symbol таблица `LLVMSymtab lg_st` с `st_push/st_pop/st_define/st_lookup` (`:321-346`); lazy IR helper-и чрез `baga_rt(name)` + `h_begin/h_call` (`:395-448`); libc декларации `rt_malloc/rt_free/...` (`:412-435`); `drop` guard на `:4156-4158`; statement emission `emit_stmt_llvm` (`:4593`) и `emit_block_llvm` (`:4579`, прави `st_push/st_pop`); `codegen_llvm()` entry на `:5017`.
- 23 `rt_malloc()` сайта в `codegen_llvm.c` — част са за heap стойности (str/bytes/Vec/Map/enum box/closure env), част вътрешни (map buckets). Под `--rc` само първите минават през `baga_rc_alloc`.
- C RC референция: header+alloc `src/codegen_c.c:5284-5390`; retain/release ядро `:5399-5418+`; `baga_rc_release_str/bytes` `:5553`, `baga_rc_release_vec` `:5635`, `baga_rc_release_map` след него; scope машинерия `:560-606`; `drop` emission `:2141-2243`; `baga_drop_*` без rc `:6188-6214`; per-struct helper-и `emit_rc_struct_helpers` `:4558`, per-enum `emit_rc_enum_helpers` `:4657`; тагове `rc_type_tag` `:97` (1=str,2=bytes,3=Vec,4=Map), `rc_heap_tag` `:208` (+5=struct-с-heap, +6=enum-с-heap).
- `include/baga.h`: `RcLocal`/`RcScope` `:546-555`; декларация `void codegen_llvm(Node *program, const char *output_path, Checker *chk);` `:688`.
- `src/main.c:355-365`: LLVM пътят вика `codegen_llvm(program, NULL, &checker)` и НЕ подава `rc`; C пътят подава `cg.rc = rc` на `:371,388`.
- Тест runner-и: `tests/llvm_oracle.sh` (C↔LLVM паритет над `examples/*.baga` чрез `lli-14 -load lib/libbaga_par.so`); RC тестовете са `tests/rc_test.baga`, `temp_test.baga`, `move_test.baga`, `borrow_test.baga`, `cmove_test.baga`, `struct_rc_test.baga`, `enum_rc_test.baga`, `enum_box_rc_test.baga`, `vecvec_rc_test.baga`, `nested_assign_rc_test.baga`, `calltemp_rc_test.baga`, `owned_ret_rc_test.baga`, `match_temp_rc_test.baga`; под C се пускат `./baga --rc -I . -I app-product tests/rc_test.baga`. Семантични са — минават и без `--rc`.
- **Page-guard тънкост** (разлика от C): C има range guard над arena блокове преди magic check. В LLVM няма arena; `baga_rc_hdr(p)` чете `p - 32` само ако `(uintptr_t)p & 0xFFF) >= 32` — иначе връща NULL (immortal). Baga payload под rc е `malloc_base + 32`, т.е. page offset ≥ 32 освен в редкия случай base offset > 4064 — тогава стойността се третира като immortal (тих leak, безопасна посока). Документирай това в коментар над `baga_rc_hdr`.

---

### Task 1: CLI plumbing — `--rc` достига LLVM бекенда

**Files:**
- Modify: `include/baga.h:688`
- Modify: `src/codegen_llvm.c:31-63` (struct LLVMCodegen), `:5017` (entry)
- Modify: `src/main.c:357`

**Interfaces:**
- Consumes: съществуващия `rc` флаг в `main.c` (парсва се вече; ползва се на `:371,388`).
- Produces: `void codegen_llvm(Node *program, const char *output_path, Checker *chk, int rc);` и поле `lg.rc` (int, 0/1), което всички следващи задачи четат.

- [ ] **Step 1: Failing test**

```bash
grep -n "int rc;" src/codegen_llvm.c   # очаквай: нищо (няма rc поле)
```

- [ ] **Step 2: Добави `int rc;` в `LLVMCodegen`**

В `src/codegen_llvm.c` struct-а (`:31-63`), след `Checker *chk;`:

```c
    Checker *chk;
    int   rc;             /* --rc: RC паметов модел (паритет с C бекенда) */
```

- [ ] **Step 3: Смени сигнатурата и init-а**

`src/codegen_llvm.c:5017`:

```c
void codegen_llvm(Node *program, const char *output_path, Checker *chk, int rc) {
```

и в тялото след `lg.chk = chk;` добави `lg.rc = rc;`.

`include/baga.h:688`:

```c
void codegen_llvm(Node *program, const char *output_path, Checker *chk, int rc);
```

`src/main.c:357`:

```c
        codegen_llvm(program, NULL, &checker, rc);
```

- [ ] **Step 4: Build + паритет без поведенческа промяна**

```bash
make llvm && make test-llvm
```

Expected: build OK; оракулът зелен (`--- оракулът е доволен`). `./baga-llvm --emit-llvm --rc examples/closures.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll` дава същия изход като без `--rc` (флагът още не променя emission).

- [ ] **Step 5: Commit**

```bash
git add include/baga.h src/codegen_llvm.c src/main.c
git commit -m "llvm: plumb --rc flag into LLVM backend (no behavior change)"
```

---

### Task 2: RC header, `baga_rc_alloc`, retain/release за str/bytes

**Files:**
- Modify: `src/codegen_llvm.c` — нова секция след M20 payload runtime (около `:460`); str/bytes malloc сайтове (audit на 23-те `rt_malloc()` сайта, `:568,597,635,824,865,909,1004,1011,1250,1257,1405,1542,1691,1771,1823,1925,2315,2322,2469,2575,2786,2959,4534`).

**Interfaces:**
- Produces (lazy IR функции, достъпни навсякъде във файла чрез `baga_rt`):
  - `baga_rc_alloc(i64 n) -> i8*` — malloc(n+32), пише header, връща base+32.
  - `baga_rc_retain(i8* p) -> i8*` — rc++ ако hdr валиден; връща p.
  - `baga_rc_release_str(i8* p) -> void`, `baga_rc_release_bytes(i8* p) -> void` — rc--; при 0 → free(base); при underflow → stderr + exit(1).
  - C-функция `static LLVMValueRef rc_alloc_call(LLVMValueRef nbytes, const char *name)` — emit-ва call към `baga_rc_alloc` или към `rt_malloc()` според `lg.rc`. Всички следващи задачи и сайтове ползват нея.
  - C-предикат `static int lrc_type_tag(Type *t)` — порт на `rc_type_tag` (`codegen_c.c:97`): 1=str, 2=bytes, 3=Vec, 4=Map, 0=не-heap.

- [ ] **Step 1: Failing test**

```bash
make llvm
./baga-llvm --emit-llvm --rc examples/str.baga > /tmp/t.ll
grep -c baga_rc_alloc /tmp/t.ll   # очаквай: 0 (още не съществува)
```

- [ ] **Step 2: `baga_rc_hdr` + `baga_rc_alloc` IR helper-и**

Нова секция в `src/codegen_llvm.c` (след M20 runtime, ~`:460`). Първо C-обвивка за lazy регистрация, следвайки съществуващия pattern: функция `static LLVMValueRef baga_rt(const char *name)` връща съществуваща или създава IR функция по име; новите тела се пишат с `h_begin(fn)` + builder, като builder позицията се запазва/възстановява (виж как `baga_rt` прави това за съществуващите helper-и около `:395-448` и една конкретна helper emission, напр. `:560-640`).

`baga_rc_hdr(i8* p) -> i8*` (връща base = p−32 или NULL):

```c
/* baga_rc_hdr: page guard + magic check (LLVM няма arena range guard).
 * Четем p-32 само при page offset >= 32; иначе NULL (immortal). */
```

IR логика (в helper тялото, с `h_call`/`LLVMBuild*`):

```c
// off = ptrtoint(p) & 4095; if (off < 32) ret NULL
// base = p - 32; magic = load i64, base
// if (magic != 0xBA6A4D454D344848ULL) ret NULL; else ret base
```

`baga_rc_alloc(i64 n) -> i8*`:

```c
// base = call malloc(n + 32)
// store i64 MAGIC, base; store i64 0, base+8 (pe); store i64 1, base+16 (rc);
// store i64 n, base+24 (an)
// ret base + 32
```

`baga_rc_retain(i8* p) -> i8*`: `h = baga_rc_hdr(p); if (h) { rc = load h+16; store rc+1 } ret p`.

`baga_rc_release_str/bytes(i8* p)`:

```c
// h = baga_rc_hdr(p); if (!h) ret
// rc = load h+16
// if (rc == 0) { fprintf(stderr, "baga: rc underflow\n"); exit(1); }
// store rc-1, h+16; if (rc-1 == 0) free(h)
```

- [ ] **Step 3: `rc_alloc_call` + audit и превключване на str/bytes malloc сайтовете**

```c
static LLVMValueRef rc_alloc_call(LLVMValueRef nbytes, const char *name) {
    if (lg.rc) return h_call(baga_rt("baga_rc_alloc"),
                             (LLVMValueRef[]){ coerce(nbytes, lg.i64_ty) }, 1, name);
    return h_call(rt_malloc(), (LLVMValueRef[]){ nbytes }, 1, name);
}
```

Мини през всички 23 сайта; превключи на `rc_alloc_call` САМО тези, чийто резултат е жива бага стойност тип str/bytes (напр. dup/concat/slice/bytes_new — `:568,597,635,865,1405,1691,1771,1823,1925` и под.). Vec/Map/enum box/closure env сайтовете остават за Task 5/6; вътрешните (map buckets `:2315,2322,2469,2575`, временни буфери които се free-ват в същия helper) остават `rt_malloc()`. При всяко превключване: payload offset-ите в helper-а не се променят (header е ПРЕД върнатия указател).

Внимание: realloc сайтове (vec/map grow) не се пипат в тази задача.

- [ ] **Step 4: Тест — IR съдържа rc helper-ите, изходът е непроменен**

```bash
make llvm
./baga-llvm --emit-llvm --rc examples/str.baga > /tmp/t.ll && grep -c baga_rc_alloc /tmp/t.ll   # > 0
lli-14 -load lib/libbaga_par.so /tmp/t.ll > /tmp/llvm_out.txt
./baga examples/str.baga > /tmp/c_out.txt
diff /tmp/c_out.txt /tmp/llvm_out.txt   # празно
./baga-llvm --emit-llvm --rc examples/bytes.baga > /tmp/t2.ll && lli-14 -load lib/libbaga_par.so /tmp/t2.ll
make test-llvm   # без --rc: побайтово същото
```

Expected: всички diff-ове празни, оракулът зелен.

- [ ] **Step 5: Commit**

```bash
git add src/codegen_llvm.c
git commit -m "llvm rc: header + rc_alloc + retain/release for str/bytes"
```

---

### Task 3: Scope tracking — release при изход от scope, retain при alias, move при return

**Files:**
- Modify: `src/codegen_llvm.c` — нова RC секция (след Task 2 helper-ите); `emit_block_llvm` (`:4579-4591`); `emit_stmt_llvm` случаи `NODE_LET` (`:4597-4612`), `NODE_RETURN` (`:4614-4625`), `NODE_BREAK`/`NODE_CONTINUE` (`:4739-4747`), `NODE_ASSIGN` в `emit_expr_llvm` (`:4197-4212`); fn emission (където се дефинират параметрите чрез `st_define` — потърси `st_define` сайтовете с `grep -n st_define src/codegen_llvm.c`).

**Interfaces:**
- Consumes: `lrc_type_tag`, `baga_rc_retain`, `baga_rc_release_str/bytes` (Task 2).
- Produces:
  - `typedef struct { const char *name; int tag; Type *type; int is_param; int dead; } LLRcLocal;` + `typedef struct { int top; int is_loop; } LLRcScope;` + полета в `LLVMCodegen`: `LLRcLocal lrc_locals[256]; int lrc_count; LLRcScope lrc_scopes[64]; int lrc_depth; int lrc_fn_base;`
  - `static void lrc_push_scope(int is_loop)`, `static void lrc_pop_scope(void)` (emit-ва release за локалите над `top` в обратен ред, само ако текущият block няма terminator), `static void lrc_track(const char *name, Type *t, int is_param)`, `static void lrc_release_all(void)` (за return; пропуска returned стойността — move), `static void lrc_release_to_loop(void)` (за break/continue), `static void lrc_emit_release(LLRcLocal *l)` (switch по tag: str/bytes → съответния helper; Vec/Map/struct/enum — stub `llvm_unsupported` до Task 5/6).

- [ ] **Step 1: Failing test**

```bash
./baga-llvm --emit-llvm --rc tests/rc_test.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll
# очаквай: минава (тестът е семантичен), НО:
grep -c baga_rc_release /tmp/t.ll   # 0 — никой не release-ва
```

- [ ] **Step 2: Структурите и scope функциите**

```c
/* RC-LLVM: scope tracking — огледало на RcLocal/RcScope (baga.h:546-555).
 * Активно само при lg.rc. Статични масиви, както LLVMSymtab. */
typedef struct { const char *name; int tag; Type *type; int is_param; int dead; } LLRcLocal;
typedef struct { int top; int is_loop; } LLRcScope;
```

В `LLVMCodegen`: полетата от Interfaces. `lrc_push_scope(is_loop)` = `lrc_scopes[lrc_depth++] = (LLRcScope){ lrc_count, is_loop }`. `lrc_pop_scope`:

```c
static void lrc_pop_scope(void) {
    if (!lg.rc) { return; }
    LLRcScope sc = lg.lrc_scopes[--lg.lrc_depth];
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        for (int i = lg.lrc_count - 1; i >= sc.top; i--) {
            LLRcLocal *l = &lg.lrc_locals[i];
            if (!l->is_param && !l->dead) lrc_emit_release(l);
        }
    }
    lg.lrc_count = sc.top;
}
```

`lrc_emit_release`: зарежда стойността от alloca-та (`st_lookup(l->name)` → `LLVMBuildLoad2`) и вика `baga_rc_release_<tag>`; за tag ≥ 3 засега `llvm_unsupported("rc release за Vec/Map/struct — следваща задача")`.

- [ ] **Step 3: Закачане в emit пътя**

- `emit_block_llvm` (`:4579`): след `st_push();` → `lrc_push_scope(0);`; преди `st_pop();` → `lrc_pop_scope();`. В `NODE_FOR` (`:4692-4727`) и `NODE_WHILE` телата минават през `emit_block_llvm` — цикълният scope трябва `is_loop=1`: най-просто, в `NODE_WHILE`/`NODE_FOR` смени директното `emit_block_llvm(n->while_body, ...)` с обвивка, която пушва loop scope: добави `static int lrc_loop_next;` флаг, който `NODE_WHILE`/`NODE_FOR` вдигат преди извикването, а `emit_block_llvm` чете (`lrc_push_scope(lrc_loop_next); lrc_loop_next = 0;`).
- `NODE_LET` (`:4610`): след `st_define(n->let_name, alloca);` → `lrc_track(n->let_name, n->let_init ? n->let_init->type : NULL, 0)`. Ако init е `NODE_IDENT` с heap tag (alias `let x = y`) → emit `baga_rc_retain` върху стойността преди store. Ако init е свежа конструкция (tag > 0, не IDENT) → rc=1 от alloc, без retain.
- Fn параметри: на всяко `st_define` за параметър → `lrc_track(name, type, /*is_param=*/1)`. В началото на fn emission: `lg.lrc_fn_base = lg.lrc_depth;`.
- `NODE_RETURN` (`:4614`): преди `LLVMBuildRet` → ако `n->ret_val` е IDENT с heap tag: маркирай неговия запис dead (move), после `lrc_release_all()` (release-ва всичко над `lrc_fn_base`, пропускайки dead/params). Без ret_val → `lrc_release_all()`.
- `NODE_BREAK`/`NODE_CONTINUE` (`:4739-4747`): преди `LLVMBuildBr` → `lrc_release_to_loop()` (pop-ва scope-ове до най-близкия `is_loop=1`, release-вайки локалите им, без да променя `lrc_depth`/`lrc_count` — само emission, защото LLVM продължава линейно; записите се попват реално при изход от блока, но там блокът вече има terminator и `lrc_pop_scope` няма да дублира).
- `NODE_ASSIGN` (`:4197-4212`): ако target е heap-tag локал: emit release на старата стойност (load преди store), после store, после retain на новата ако `assign_val` е IDENT.

- [ ] **Step 4: Тест**

```bash
make llvm
./baga-llvm --emit-llvm --rc tests/rc_test.baga > /tmp/t.ll && grep -c baga_rc_release /tmp/t.ll   # > 0
lli-14 -load lib/libbaga_par.so /tmp/t.ll   # същия изход като ./baga --rc tests/rc_test.baga
for t in move_test borrow_test; do
  ./baga-llvm --emit-llvm --rc tests/$t.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll > /tmp/l.txt || exit 1
  ./baga --rc tests/$t.baga > /tmp/c.txt; diff /tmp/c.txt /tmp/l.txt || exit 1
done
make test && make test-llvm
```

Expected: release повиквания в IR; изходи идентични с C; оракул зелен.

- [ ] **Step 5: Commit**

```bash
git add src/codegen_llvm.c
git commit -m "llvm rc: scope tracking, retain on alias, move on return"
```

---

### Task 4: `drop` в LLVM бекенда

**Files:**
- Modify: `src/codegen_llvm.c:4156-4158` (guard), нова drop emission (до RC секцията).

**Interfaces:**
- Consumes: `lrc_emit_release`, `lg.lrc_locals` dead маркерите (Task 3), `lrc_type_tag` (Task 2).
- Produces: `static void lrc_emit_drop(Node *call_node)` — обработва `drop(x)`; user fn с име `drop` запазва приоритет (тя се resolve-ва преди guard-а, `:4151-4155`).

- [ ] **Step 1: Failing test**

Създай `tests/drop_llvm_test.baga`:

```baga
fn main() {
    let s = "жив".str + ""
    let v = Vec.new()
    v.push(1)
    drop(s)
    drop(v)
    print("ok drop")
}
```

(синтаксисът е по образец на `tests/rc_test.baga` — провери го и го дръж минимален; целта е drop върху str и Vec.)

```bash
./baga-llvm --emit-llvm tests/drop_llvm_test.baga
# очаквай: „неподдържан конструкт 'drop — само C бекенда...'" → FAIL
```

- [ ] **Step 2: Имплементация**

Замени guard-а на `:4156-4158` с:

```c
            if (!fn && strcmp(n->callee->name, "drop") == 0) {
                lrc_emit_drop(n);
                return NULL;  /* drop е statement-израз без стойност */
            }
```

(Провери как `emit_expr_llvm` третира NULL резултати от call — `NODE_EXPR_STMT` (`:4731`) го понася; ако `drop` се ползва в израз, `llvm_unsupported("drop в израз")`.)

`lrc_emit_drop`:

```c
static void lrc_emit_drop(Node *n) {
    /* drop(x): x трябва да е IDENT (като C бекенда, codegen_c.c:2141-2243) */
    if (n->args.len != 1 || n->args.data[0]->kind != NODE_IDENT)
        llvm_unsupported("drop приема единствено име на локална");
    Node *id = n->args.data[0];
    if (lg.rc) {
        /* намери записа; dead → честна underflow грешка става в release helper-а */
        for (int i = lg.lrc_count - 1; i >= 0; i--) {
            LLRcLocal *l = &lg.lrc_locals[i];
            if (strcmp(l->name, id->name) == 0) {
                lrc_emit_release(l);
                l->dead = 1;
                return;
            }
        }
        llvm_unsupported("drop на неизвестно име");
    }
    /* без --rc: baga_drop_* (free) — порт на codegen_c.c:6188-6214:
     * str/bytes → free чрез baga_rc_hdr-еквивалент не съществува; plain
     * free(p). Vec/Map → deep free helper baga_drop_vec/baga_drop_map. */
    ... /* emit h_call(baga_rt("baga_drop_vec"), ...) по типа на id->type */
}
```

За не-RC пътя добави lazy IR helper-и `baga_drop_str/bytes` (= `free(p)` при не-NULL) и `baga_drop_vec`/`baga_drop_map` (порт на C дефинициите `:6188-6214` — free на data/buckets + struct-а).

- [ ] **Step 3: Тест**

```bash
make llvm
./baga-llvm --emit-llvm tests/drop_llvm_test.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll   # „ok drop"
./baga-llvm --emit-llvm --rc tests/drop_llvm_test.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll   # „ok drop"
./baga tests/drop_llvm_test.baga > /tmp/c.txt && diff /tmp/c.txt <(lli-14 -load lib/libbaga_par.so /tmp/t.ll)
make test && make test-llvm
```

- [ ] **Step 4: Commit**

```bash
git add src/codegen_llvm.c tests/drop_llvm_test.baga
git commit -m "llvm: drop support (free without rc, rc release with --rc)"
```

---

### Task 5: RC за Vec/Map

**Files:**
- Modify: `src/codegen_llvm.c` — Vec/Map helper-и (vec_new `:998-1016`, vec grow около `:1250`, map `:2315+`); RC секция.

**Interfaces:**
- Consumes: `rc_alloc_call`, `lrc_type_tag`, scope машинерията (Task 2/3).
- Produces: `baga_rc_release_vec(i8* v, i64 elem_kind, i64 elem_size)` и `baga_rc_release_map(i8* m, i64 key_kind, i64 val_kind)` IR helper-и (порт на `codegen_c.c:5635+`: при rc=0 — release на елементите с heap tag, после free на data/buckets + struct). `lrc_emit_release` вече не прави `llvm_unsupported` за tag 3/4.

- [ ] **Step 1: Failing test**

```bash
./baga-llvm --emit-llvm --rc tests/rc_test.baga > /tmp/t.ll
# очаквай FAIL: „rc release за Vec/Map/struct — следваща задача" (rc_test ползва Vec/Map)
```

- [ ] **Step 2: Vec/Map алокациите през `rc_alloc_call`**

Сайтове `:1004,1011` (vec struct + data), `:1250,1257` (grow — внимание: grow ползва realloc? ако да, под rc трябва rc-aware grow: malloc нов чрез `baga_rc_alloc` + memcpy + старият блок се free-ва само от release; най-просто — при lg.rc използвай alloc+memcpy варианта винаги), `:2315,2322` (map struct + buckets). Вътрешните bucket/entry блокове НЕ носят собствен rc header — те умират с контейнера (като в C: `baga_rc_release_vec` free-ва data директно).

- [ ] **Step 3: release helper-ите**

`baga_rc_release_vec(v, elem_kind, elem_size)`:

```c
// h = baga_rc_hdr(v); if (!h) ret; rc--; underflow check
// if (rc-1 > 0) ret
// if (elem_kind == 1) for i in 0..len: baga_rc_release_str(data[i])
// if (elem_kind == 2) същото с bytes
// free(data_base) — data-та под rc има собствен header: baga_rc_hdr(data) → free(base)
// free(h)
```

`baga_rc_release_map` — аналог по образец на C (`codegen_c.c` след `:5635`): обходи bucket веригите, release-вай key/val според kind, free на entries/buckets/struct.

- [ ] **Step 4: `lrc_emit_release` — включи tag 3/4**

```c
case 3: /* Vec */ elem kind/size от l->type->elem (или type_node резерва,
         както RcLocal.type_node в C) → baga_rc_release_vec
case 4: /* Map */ key/val kinds → baga_rc_release_map
```

- [ ] **Step 5: Тест**

```bash
make llvm
for t in rc_test move_test borrow_test; do
  ./baga-llvm --emit-llvm --rc tests/$t.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll > /tmp/l.txt
  ./baga --rc tests/$t.baga > /tmp/c.txt; diff /tmp/c.txt /tmp/l.txt || exit 1
done
make test && make test-llvm
```

- [ ] **Step 6: Commit**

```bash
git add src/codegen_llvm.c
git commit -m "llvm rc: Vec/Map alloc + release with element recursion"
```

---

### Task 6: RC за struct/enum (RC5 порт)

**Files:**
- Modify: `src/codegen_llvm.c` — RC секция; enum box alloc сайтове (`:1542,2786,2959`).

**Interfaces:**
- Consumes: всичко дотук; `lg.chk` за struct field lookup (както C ползва `Codegen.chk` в `rc_heap_tag`, `codegen_c.c:208`).
- Produces: `static int lrc_heap_tag(Type *t)` (порт на `rc_heap_tag`: добавя 5=struct-с-heap, 6=enum-с-heap, транзитивно с depth guard 32); per-struct IR helper-и `baga_rc_retain_<cname>` / `baga_rc_release_<cname>` и per-enum `switch(tag)` variant-и (порт на `emit_rc_struct_helpers` `codegen_c.c:4558` и `emit_rc_enum_helpers` `:4657`); `lrc_emit_release` покрива tag 5/6; `lrc_track` ползва `lrc_heap_tag` вместо `lrc_type_tag`.

- [ ] **Step 1: Failing test**

```bash
./baga-llvm --emit-llvm --rc tests/struct_rc_test.baga > /tmp/t.ll
# очаквай FAIL: unsupported tag 5
```

- [ ] **Step 2: `lrc_heap_tag` + struct field обход**

Порт на `codegen_c.c:208-234`: struct е heap ако има поле с heap tag (транзитивно, depth ≤ 32); enum — ако някой variant payload е heap. Имената на struct типовете в LLVM са mangle-нати чрез `llvm_struct_cname`/`user_struct_ty` (`:85-95`) — helper функциите се кръщават `baga_rc_release_<cname>`.

- [ ] **Step 3: Per-struct/enum helper emission**

Lazy, при първа нужда (както останалите `baga_rt`): тяло, което за всяко heap поле emit-ва release (отложено: retain аналогично). Enum: `switch(tag)` по variant-ите, release на heap payload-ите (образец: сумарните enum типове от нулевия проход, `:5073-5080`, и `emit_rc_enum_helpers` `:4657`). Enum box alloc сайтове `:1542,2786,2959` минават през `rc_alloc_call`.

- [ ] **Step 4: Тест**

```bash
make llvm
for t in struct_rc_test enum_rc_test enum_box_rc_test nested_assign_rc_test owned_ret_rc_test rc_test; do
  ./baga-llvm --emit-llvm --rc tests/$t.baga > /tmp/t.ll && lli-14 -load lib/libbaga_par.so /tmp/t.ll > /tmp/l.txt
  ./baga --rc tests/$t.baga > /tmp/c.txt; diff /tmp/c.txt /tmp/l.txt || exit 1
done
make test && make test-llvm
```

- [ ] **Step 5: Commit**

```bash
git add src/codegen_llvm.c
git commit -m "llvm rc: struct/enum transitive release helpers (RC5 parity)"
```

---

### Task 7: Тестова инфраструктура, документация, CHANGELOG

**Files:**
- Create: `tests/llvm_rc.sh`
- Modify: `Makefile` (target `test-llvm-rc`)
- Modify: `docs/language-bg.md:1019-1022`, `docs/language-en.md` (съответния пасаж), `docs/memory-rc-bg.md:52`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: работещите `--emit-llvm --rc` от Task 1-6.

- [ ] **Step 1: `tests/llvm_rc.sh`**

```bash
#!/bin/bash
# RC батерия под LLVM бекенда: всички *_rc_test + move/borrow/cmove/temp тестове.
# Оракул: изходът на ./baga --rc (C) трябва да съвпада с LLVM+lli.
cd "$(dirname "$0")/.."
FAIL=0
PAR_SO=lib/libbaga_par.so
[ -f "$PAR_SO" ] || { mkdir -p lib; gcc -O2 -fPIC -shared -o "$PAR_SO" src/baga_par_rt.c -pthread || exit 1; }
TESTS="tests/rc_test.baga tests/temp_test.baga tests/move_test.baga tests/borrow_test.baga \
tests/cmove_test.baga tests/struct_rc_test.baga tests/enum_rc_test.baga tests/enum_box_rc_test.baga \
tests/nested_assign_rc_test.baga tests/calltemp_rc_test.baga tests/owned_ret_rc_test.baga \
tests/match_temp_rc_test.baga tests/drop_llvm_test.baga"
for f in $TESTS; do
    [ -f "$f" ] || { echo "SKIP  $f (липсва)"; continue; }
    if ! ./baga --rc -I . -I app-product "$f" > /tmp/rc_c.txt 2>&1; then
        echo "SKIP  $f (C --rc не минава — не е LLVM проблем)"; continue
    fi
    if ! ./baga-llvm --emit-llvm --rc -I . -I app-product "$f" > /tmp/rc.ll 2>/tmp/rc_err.txt; then
        echo "FAIL  $f (LLVM compile)"; cat /tmp/rc_err.txt; FAIL=1; continue
    fi
    lli-14 -load "$PAR_SO" /tmp/rc.ll > /tmp/rc_l.txt 2>&1
    if diff -q /tmp/rc_c.txt /tmp/rc_l.txt > /dev/null; then echo "OK    $f"; else
        echo "MISMATCH $f"; diff -u /tmp/rc_c.txt /tmp/rc_l.txt | head -20; FAIL=1
    fi
done
[ $FAIL -eq 0 ] && echo "--- rc оракулът е доволен ⚔️"
exit $FAIL
```

`chmod +x tests/llvm_rc.sh`.

- [ ] **Step 2: Makefile target**

```make
test-llvm-rc: llvm
	./tests/llvm_rc.sh
```

(добави към съществуващия Makefile след `test-llvm` target-а; добави и в `.PHONY` ако има такъв ред)

- [ ] **Step 3: Пусни всичко**

```bash
make test-llvm-rc   # очаквай: всички OK, „rc оракулът е доволен"
make test && make test-llvm
```

- [ ] **Step 4: Документация**

- `docs/language-bg.md:1019-1022`: махни твърдението, че `drop` е unsupported в LLVM; запиши: под `--emit-llvm --rc` RC моделът е същият като C, с изключения: temp-ове на statement ниво не се release-ват (RC4 е C-only засега), closure captures не се retain-ват, цикли текат (няма weak). Запиши и page-guard тънкостта на `baga_rc_hdr` (едно изречение).
- Същото в `docs/language-en.md` (намери съответния пасаж с `grep -n "drop" docs/language-en.md`).
- `docs/memory-rc-bg.md:52`: промени „LLVM backend — не поддържа" на описание на LLVM паритета + двете изключения по-горе.
- `CHANGELOG.md`: запис под Unreleased/нова версия по съществуващия формат на файла: „LLVM бекенд: `drop` + `--rc` refcounting паритет с C бекенда (scope release, retain при alias, move при return, Vec/Map/struct/enum транзитивен release); `make test-llvm-rc`."

- [ ] **Step 5: Commit**

```bash
git add tests/llvm_rc.sh Makefile docs/language-bg.md docs/language-en.md docs/memory-rc-bg.md CHANGELOG.md
git commit -m "llvm rc: test harness (make test-llvm-rc) + docs"
```

---

## Self-review бележки (попълнени при писането)

- Spec покритие: header (T2), IR helper-и (T2/T5/T6), scope tracking (T3), drop (T4), CLI (T1), тестове (всяка задача + T7), не-целите са в Global Constraints. ✓
- `vecvec_rc_test.baga` (Vec<Vec>) е съзнателно извън T5/T6 списъка — изисква вложени release helper-и (`baga_rc_relv_`); ако T6 завърши чисто, добавя се в `tests/llvm_rc.sh` като следваща стъпка; не блокира плана.
- Имената `lrc_*`/`LLRc*` са консистентни между задачите; `baga_rc_*` IR имената съвпадат с C за четимост на IR.
