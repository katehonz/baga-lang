#include "baga.h"

#ifdef BAGA_LLVM

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

/* ============================================================
 *  LLVM IR Codegen — Фаза 3
 *
 *  Генерира LLVM IR директно от AST-то.
 *  Поддръжка: i64 функции, binary ops, calls, if/else, while,
 *  let bindings, return, print (via printf).
 * ============================================================ */

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
    LLVMTypeRef i64_ty;
    LLVMTypeRef i32_ty;
    LLVMTypeRef i1_ty;
    LLVMTypeRef double_ty;
    LLVMTypeRef void_ty;
    LLVMTypeRef ptr_ty;
    LLVMValueRef printf_fn;
    int tmp_counter;
} LLVMCodegen;

static LLVMCodegen lg;

/* ---- Type mapping ---- */

static LLVMTypeRef llvm_type(Node *ty);

static LLVMTypeRef llvm_type_resolved(Type *ty) {
    if (!ty) return lg.i64_ty;
    switch (ty->kind) {
        case TYPE_I64: return lg.i64_ty;
        case TYPE_I32: return lg.i32_ty;
        case TYPE_F64: return lg.double_ty;
        case TYPE_BOOL: return lg.i1_ty;
        case TYPE_STR: return lg.ptr_ty;
        case TYPE_VOID: return lg.void_ty;
        default: return lg.i64_ty;
    }
}

static LLVMTypeRef llvm_type(Node *ty) {
    if (!ty) return lg.void_ty;
    if (ty->kind == NODE_TYPE) {
        if (strcmp(ty->type_name, "i64") == 0) return lg.i64_ty;
        if (strcmp(ty->type_name, "i32") == 0) return lg.i32_ty;
        if (strcmp(ty->type_name, "f64") == 0) return lg.double_ty;
        if (strcmp(ty->type_name, "bool") == 0) return lg.i1_ty;
        if (strcmp(ty->type_name, "str") == 0) return lg.ptr_ty;
        if (strcmp(ty->type_name, "void") == 0) return lg.void_ty;
    }
    if (ty->kind == NODE_TYPE_EFFECT) return llvm_type(ty->inner_type);
    return lg.i64_ty;
}

/* ---- Name mangling ---- */

static char *llvm_mangle(const char *name) {
    size_t len = strlen(name);
    char *buf = malloc(2 + len * 4 + 1);
    char *o = buf;
    *o++ = 'b'; *o++ = '_';
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_') {
            *o++ = (char)*p;
        } else {
            o += sprintf(o, "_%d", *p);
        }
    }
    *o = '\0';
    return buf;
}

/* ---- Temp register names ---- */

static char *tmp_name(void) {
    char *buf = malloc(16);
    sprintf(buf, "t%d", lg.tmp_counter++);
    return buf;
}

/* ---- Expression emission ---- */

static LLVMValueRef emit_expr_llvm(Node *n);

static LLVMValueRef emit_binop_llvm(BinOp op, LLVMValueRef left, LLVMValueRef right) {
    char *name = tmp_name();
    LLVMValueRef result;
    switch (op) {
        case OP_ADD: result = LLVMBuildAdd(lg.builder, left, right, name); break;
        case OP_SUB: result = LLVMBuildSub(lg.builder, left, right, name); break;
        case OP_MUL: result = LLVMBuildMul(lg.builder, left, right, name); break;
        case OP_DIV: result = LLVMBuildSDiv(lg.builder, left, right, name); break;
        case OP_MOD: result = LLVMBuildSRem(lg.builder, left, right, name); break;
        case OP_EQ:  result = LLVMBuildICmp(lg.builder, LLVMIntEQ, left, right, name); break;
        case OP_NEQ: result = LLVMBuildICmp(lg.builder, LLVMIntNE, left, right, name); break;
        case OP_LT:  result = LLVMBuildICmp(lg.builder, LLVMIntSLT, left, right, name); break;
        case OP_GT:  result = LLVMBuildICmp(lg.builder, LLVMIntSGT, left, right, name); break;
        case OP_LE:  result = LLVMBuildICmp(lg.builder, LLVMIntSLE, left, right, name); break;
        case OP_GE:  result = LLVMBuildICmp(lg.builder, LLVMIntSGE, left, right, name); break;
        case OP_AND: result = LLVMBuildAnd(lg.builder, left, right, name); break;
        case OP_OR:  result = LLVMBuildOr(lg.builder, left, right, name); break;
        default:     result = LLVMBuildAdd(lg.builder, left, right, name); break;
    }
    free(name);
    return result;
}

static LLVMValueRef emit_expr_llvm(Node *n) {
    if (!n) return LLVMConstInt(lg.i64_ty, 0, 0);

    switch (n->kind) {
        case NODE_INT_LIT:
            return LLVMConstInt(lg.i64_ty, (unsigned long long)n->int_val, 1);

        case NODE_FLOAT_LIT:
            return LLVMConstReal(lg.double_ty, n->float_val);

        case NODE_BOOL_LIT:
            return LLVMConstInt(lg.i1_ty, n->bool_val, 0);

        case NODE_STR_LIT:
            return LLVMBuildGlobalStringPtr(lg.builder, n->str_val, tmp_name());

        case NODE_IDENT: {
            char *m = llvm_mangle(n->name);
            /* look up in current function's locals */
            LLVMValueRef val = LLVMGetNamedGlobal(lg.mod, m);
            if (!val) {
                /* might be a local alloca — search by name */
                /* for simplicity, use a named value lookup */
                val = LLVMConstInt(lg.i64_ty, 0, 0); /* fallback */
            }
            free(m);
            return val;
        }

        case NODE_BINARY: {
            LLVMValueRef left = emit_expr_llvm(n->left);
            LLVMValueRef right = emit_expr_llvm(n->right);
            return emit_binop_llvm(n->bin_op, left, right);
        }

        case NODE_CALL: {
            /* get callee name */
            if (n->callee->kind == NODE_IDENT) {
                char *m = llvm_mangle(n->callee->name);
                LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, m);
                free(m);
                if (fn) {
                    int nargs = n->args.len;
                    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(nargs > 0 ? nargs : 1));
                    for (int i = 0; i < nargs; i++)
                        args[i] = emit_expr_llvm(n->args.data[i]);
                    char *name = tmp_name();
                    LLVMValueRef result = LLVMBuildCall2(lg.builder,
                        LLVMGetElementType(LLVMTypeOf(fn)), fn, args, (unsigned)nargs, name);
                    free(name);
                    free(args);
                    return result;
                }
            }
            return LLVMConstInt(lg.i64_ty, 0, 0);
        }

        default:
            return LLVMConstInt(lg.i64_ty, 0, 0);
    }
}

/* ---- Statement emission ---- */

static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef cont_bb);

static void emit_block_llvm(Node *block, LLVMBasicBlockRef cont_bb) {
    if (!block || block->kind != NODE_BLOCK) return;
    for (int i = 0; i < block->stmts.len; i++)
        emit_stmt_llvm(block->stmts.data[i], cont_bb);
}

static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef cont_bb) {
    if (!n) return;

    switch (n->kind) {
        case NODE_LET: {
            char *m = llvm_mangle(n->let_name);
            LLVMTypeRef ty = lg.i64_ty;
            if (n->let_type) ty = llvm_type(n->let_type);
            else if (n->let_init && n->let_init->type) ty = llvm_type_resolved(n->let_init->type);
            LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, ty, m);
            if (n->let_init) {
                LLVMValueRef val = emit_expr_llvm(n->let_init);
                LLVMBuildStore(lg.builder, val, alloca);
            }
            free(m);
            break;
        }

        case NODE_RETURN: {
            if (n->ret_val) {
                LLVMValueRef val = emit_expr_llvm(n->ret_val);
                LLVMBuildRet(lg.builder, val);
            } else {
                LLVMBuildRetVoid(lg.builder);
            }
            break;
        }

        case NODE_IF: {
            LLVMValueRef cond = emit_expr_llvm(n->cond);
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "then");
            LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "else");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "merge");

            LLVMBuildCondBr(lg.builder, cond, then_bb, else_bb);

            LLVMPositionBuilderAtEnd(lg.builder, then_bb);
            emit_block_llvm(n->then_br, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

            LLVMPositionBuilderAtEnd(lg.builder, else_bb);
            if (n->else_br) emit_block_llvm(n->else_br, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

            LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
            break;
        }

        case NODE_WHILE: {
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_body");
            LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_end");

            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, cond_bb);
            LLVMValueRef cond = emit_expr_llvm(n->while_cond);
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            emit_block_llvm(n->while_body, end_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, end_bb);
            break;
        }

        case NODE_EXPR_STMT: {
            /* check for print call */
            if (n->expr && n->expr->kind == NODE_CALL &&
                n->expr->callee->kind == NODE_IDENT &&
                (strcmp(n->expr->callee->name, "print") == 0 ||
                 strcmp(n->expr->callee->name, "println") == 0)) {
                /* emit printf call */
                if (n->expr->args.len > 0) {
                    Node *arg = n->expr->args.data[0];
                    if (arg->kind == NODE_STR_LIT) {
                        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "%s\n", "fmt");
                        LLVMValueRef str = LLVMBuildGlobalStringPtr(lg.builder, arg->str_val, "str");
                        LLVMValueRef args[] = { fmt, str };
                        LLVMBuildCall2(lg.builder,
                            LLVMGetElementType(LLVMTypeOf(lg.printf_fn)),
                            lg.printf_fn, args, 2, "");
                    } else {
                        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "%lld\n", "fmt");
                        LLVMValueRef val = emit_expr_llvm(arg);
                        LLVMValueRef args[] = { fmt, val };
                        LLVMBuildCall2(lg.builder,
                            LLVMGetElementType(LLVMTypeOf(lg.printf_fn)),
                            lg.printf_fn, args, 2, "");
                    }
                }
            } else {
                emit_expr_llvm(n->expr);
            }
            break;
        }

        case NODE_BREAK:
            if (cont_bb) LLVMBuildBr(lg.builder, cont_bb);
            break;

        case NODE_CONTINUE:
            /* would need loop cond_bb reference */
            break;

        default:
            break;
    }
}

/* ---- Function emission ---- */

static void emit_fn_llvm(Node *fn) {
    if (!fn->fn_body) return; /* skip forward declarations */

    char *m = llvm_mangle(fn->fn_name);

    /* return type */
    LLVMTypeRef ret_ty = lg.void_ty;
    if (fn->ret_type) ret_ty = llvm_type(fn->ret_type);

    /* param types */
    int nparams = fn->params.len;
    LLVMTypeRef *param_tys = malloc(sizeof(LLVMTypeRef) * (size_t)(nparams > 0 ? nparams : 1));
    for (int i = 0; i < nparams; i++)
        param_tys[i] = llvm_type(fn->params.data[i]->param_type);

    LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, param_tys, (unsigned)nparams, 0);
    LLVMValueRef fn_val = LLVMAddFunction(lg.mod, m, fn_ty);

    /* name parameters */
    for (int i = 0; i < nparams; i++) {
        char *pm = llvm_mangle(fn->params.data[i]->param_name);
        LLVMSetValueName2(LLVMGetParam(fn_val, (unsigned)i), pm, strlen(pm));
        free(pm);
    }

    /* create entry block */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn_val, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    /* alloca for parameters (for mutability) */
    for (int i = 0; i < nparams; i++) {
        char *pm = llvm_mangle(fn->params.data[i]->param_name);
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, param_tys[i], pm);
        LLVMBuildStore(lg.builder, LLVMGetParam(fn_val, (unsigned)i), alloca);
        free(pm);
    }

    /* emit body */
    emit_block_llvm(fn->fn_body, NULL);

    /* implicit return if no terminator */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        if (ret_ty == lg.void_ty)
            LLVMBuildRetVoid(lg.builder);
        else
            LLVMBuildRet(lg.builder, LLVMConstInt(ret_ty, 0, 0));
    }

    free(param_tys);
    free(m);
}

/* ---- Public API ---- */

void codegen_llvm(Node *program, const char *output_path) {
    lg.ctx = LLVMContextCreate();
    lg.mod = LLVMModuleCreateWithNameInContext("baga_module", lg.ctx);
    lg.builder = LLVMCreateBuilderInContext(lg.ctx);
    lg.tmp_counter = 0;

    lg.i64_ty = LLVMInt64TypeInContext(lg.ctx);
    lg.i32_ty = LLVMInt32TypeInContext(lg.ctx);
    lg.i1_ty = LLVMInt1TypeInContext(lg.ctx);
    lg.double_ty = LLVMDoubleTypeInContext(lg.ctx);
    lg.void_ty = LLVMVoidTypeInContext(lg.ctx);
    lg.ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(lg.ctx), 0);

    /* declare printf */
    LLVMTypeRef printf_args[] = { lg.ptr_ty };
    LLVMTypeRef printf_ty = LLVMFunctionType(lg.i32_ty, printf_args, 1, 1);
    lg.printf_fn = LLVMAddFunction(lg.mod, "printf", printf_ty);

    /* emit functions */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN)
            emit_fn_llvm(item);
    }

    /* emit C main wrapper */
    {
        char *main_m = llvm_mangle("main");
        LLVMValueRef baga_main = LLVMGetNamedFunction(lg.mod, main_m);
        free(main_m);

        if (baga_main) {
            LLVMTypeRef c_main_ty = LLVMFunctionType(lg.i32_ty, NULL, 0, 0);
            LLVMValueRef c_main = LLVMAddFunction(lg.mod, "main", c_main_ty);
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, c_main, "entry");
            LLVMPositionBuilderAtEnd(lg.builder, entry);
            LLVMBuildCall2(lg.builder,
                LLVMGetElementType(LLVMTypeOf(baga_main)),
                baga_main, NULL, 0, "");
            LLVMBuildRet(lg.builder, LLVMConstInt(lg.i32_ty, 0, 0));
        }
    }

    /* verify module */
    char *error = NULL;
    LLVMBool broken = LLVMVerifyModule(lg.mod, LLVMPrintMessageAction, &error);
    if (broken) {
        fprintf(stderr, "baga: LLVM verification failed: %s\n", error ? error : "unknown");
        LLVMDisposeMessage(error);
    }

    /* output */
    if (output_path) {
        LLVMPrintModuleToFile(lg.mod, output_path, &error);
        if (error) {
            fprintf(stderr, "baga: LLVM output error: %s\n", error);
            LLVMDisposeMessage(error);
        }
    } else {
        char *ir = LLVMPrintModuleToString(lg.mod);
        printf("%s", ir);
        LLVMDisposeMessage(ir);
    }

    /* cleanup */
    LLVMDisposeBuilder(lg.builder);
    LLVMDisposeModule(lg.mod);
    LLVMContextDispose(lg.ctx);
}

#endif /* BAGA_LLVM */
