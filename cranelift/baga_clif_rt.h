/* baga_clif_rt.h — споделени константи C <-> Rust за Cranelift bytecode-а.
 *
 * ВАЖНО: стойностите тук и в cranelift/src/lib.rs ТРЯБВА да са идентични
 * (ръчно синхронизирани). Коментарът „keep in sync" бележи всяка група.
 * Opcode-ите ползват префикс CL_, за да не се сблъскват с BinOp (OP_*) в baga.h.
 */
#ifndef BAGA_CLIF_RT_H
#define BAGA_CLIF_RT_H

/* ---- opcode-и (u8) — keep in sync ---- */
enum {
    CL_ICONST   = 1,   /* + i64            push i64 константа            */
    CL_FCONST   = 2,   /* + f64 (8 байта)  push f64 константа            */
    CL_BCONST   = 3,   /* + u8 (0/1)       push bool                     */
    CL_SCONST   = 4,   /* + u32 str_id     push ptr към интерниран низ    */
    CL_LOAD     = 5,   /* + u16 slot       push стойността на локал       */
    CL_STORE    = 6,   /* + u16 slot       pop -> запис в локал           */
    CL_ALLOCA   = 7,   /* + u16 slot, u8 ty  задели локал (stack slot)    */
    CL_BINOP    = 8,   /* + u8 binop       pop 2, push резултат           */
    CL_AND      = 9,   /* pop 2 bool, push bool (без short-circuit)       */
    CL_OR       = 10,  /* pop 2 bool, push bool (без short-circuit)       */
    CL_NOT      = 11,  /* pop bool, push !bool                            */
    CL_NEG      = 12,  /* + u8 ty          pop, push -x                   */
    CL_PROMOTE  = 13,  /* pop i64, push f64 (sitofp)                      */
    CL_CALL     = 14,  /* + u32 fn_id, u16 nargs   pop nargs, push рез.   */
    CL_RET      = 15,  /* pop 1 -> return                                 */
    CL_RET_VOID = 16,  /* return void                                     */
    CL_BR       = 17,  /* + u32 label      безусловен скок                */
    CL_BR_FALSE = 18,  /* + u32 label      pop bool -> скок ако 0         */
    CL_LABEL    = 19,  /* + u32 label      граница на блок                */
    CL_DROP     = 20,  /* pop 1 (без използване)                          */
};

/* ---- binop кодове (u8) — keep in sync ---- */
enum {
    B_ADD_I = 0, B_SUB_I = 1, B_MUL_I = 2, B_DIV_I = 3, B_MOD_I = 4,
    B_ADD_F = 5, B_SUB_F = 6, B_MUL_F = 7, B_DIV_F = 8, B_MOD_F = 9,
    B_EQ_I  = 10, B_NEQ_I = 11, B_LT_I = 12, B_GT_I = 13, B_LE_I = 14, B_GE_I = 15,
    B_EQ_F  = 16, B_NEQ_F = 17, B_LT_F = 18, B_GT_F = 19, B_LE_F = 20, B_GE_F = 21,
    B_BAND_I = 22, B_BOR_I = 23, B_BXOR_I = 24, B_SHL_I = 25, B_SHR_I = 26,
};

/* ---- типови кодове (u8) — keep in sync ---- */
enum {
    TY_VOID = 0,
    TY_I64  = 1,
    TY_I32  = 2,
    TY_F64  = 3,
    TY_BOOL = 4,
    TY_PTR  = 5,   /* str / raw pointer */
};

/* ---- runtime helper fn_id-та (u32) — keep in sync ----
 * fn_id < RT_COUNT => runtime helper; иначе потребителска функция с
 * индекс (fn_id - RT_COUNT). */
enum {
    RT_PRINT_I64 = 0,   /* (i64) -> void   printf("%lld\n")          */
    RT_PRINT_F64 = 1,   /* (f64) -> void   printf("%g\n")            */
    RT_PRINT_BOOL = 2,  /* (bool) -> void  printf("%s\n", true/false) */
    RT_PRINT_STR = 3,   /* (ptr) -> void   printf("%s\n")            */
    RT_WRITE_STR = 4,   /* (ptr) -> void   printf("%s")              */
    RT_PRINT_NL  = 5,   /* () -> void      printf("\n")              */
    RT_ARG       = 6,   /* (i64) -> ptr                              */
    RT_ARG_COUNT = 7,   /* () -> i64                                 */
    RT_SPEC_FAIL = 8,   /* (ptr spec, ptr kind, i64 idx, ptr expr) -> void; exit(1) */
    RT_COUNT     = 9,
};

#endif /* BAGA_CLIF_RT_H */
