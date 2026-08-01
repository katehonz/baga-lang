// Cranelift JIT backend за Бага — Rust FFI мост.
//
// C генерира сериализиран стеков bytecode (виж baga_clif_rt.h); този модул го
// интерпретира в Cranelift IR и го JIT-ва in-process. Runtime helper-ите
// (print, arg, spec_fail) са native `extern "C"` функции, които JIT-ът импортира
// по име — те викат libc printf/fprintf с БАЙТ-същите формати като codegen_c.c.
//
// Конвенции (keep in sync с baga_clif_rt.h):
//   opcode-и, binop кодове, типови кодове, RT_* fn_id-та.

use std::os::raw::{c_char, c_int};
use std::ptr;

use cranelift_codegen::ir::{
    types, AbiParam, Block, InstBuilder, Signature, StackSlot, StackSlotData, StackSlotKind,
    Type, Value,
};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{DataDescription, DataId, FuncId, FuncOrDataId, Linkage, Module};

// ---- keep in sync: baga_clif_rt.h opcode-и ----
const OP_ICONST: u8 = 1;
const OP_FCONST: u8 = 2;
const OP_BCONST: u8 = 3;
const OP_SCONST: u8 = 4;
const OP_LOAD: u8 = 5;
const OP_STORE: u8 = 6;
const OP_ALLOCA: u8 = 7;
const OP_BINOP: u8 = 8;
const OP_AND: u8 = 9;
const OP_OR: u8 = 10;
const OP_NOT: u8 = 11;
const OP_NEG: u8 = 12;
const OP_PROMOTE: u8 = 13;
const OP_CALL: u8 = 14;
const OP_RET: u8 = 15;
const OP_RET_VOID: u8 = 16;
const OP_BR: u8 = 17;
const OP_BR_FALSE: u8 = 18;
const OP_LABEL: u8 = 19;
const OP_DROP: u8 = 20;

// ---- keep in sync: binop кодове ----
const B_ADD_I: u8 = 0;
const B_SUB_I: u8 = 1;
const B_MUL_I: u8 = 2;
const B_DIV_I: u8 = 3;
const B_MOD_I: u8 = 4;
const B_ADD_F: u8 = 5;
const B_SUB_F: u8 = 6;
const B_MUL_F: u8 = 7;
const B_DIV_F: u8 = 8;
const B_MOD_F: u8 = 9;
const B_EQ_I: u8 = 10;
const B_NEQ_I: u8 = 11;
const B_LT_I: u8 = 12;
const B_GT_I: u8 = 13;
const B_LE_I: u8 = 14;
const B_GE_I: u8 = 15;
const B_EQ_F: u8 = 16;
const B_NEQ_F: u8 = 17;
const B_LT_F: u8 = 18;
const B_GT_F: u8 = 19;
const B_LE_F: u8 = 20;
const B_GE_F: u8 = 21;
const B_BAND_I: u8 = 22;
const B_BOR_I: u8 = 23;
const B_BXOR_I: u8 = 24;
const B_SHL_I: u8 = 25;
const B_SHR_I: u8 = 26;

// ---- keep in sync: типови кодове ----
const TY_VOID: u8 = 0;
const TY_I64: u8 = 1;
const TY_I32: u8 = 2;
const TY_F64: u8 = 3;
const TY_BOOL: u8 = 4;
const TY_PTR: u8 = 5;

// ---- keep in sync: RT_* fn_id-та ----
const RT_PRINT_I64: u32 = 0;
const RT_PRINT_F64: u32 = 1;
const RT_PRINT_BOOL: u32 = 2;
const RT_PRINT_STR: u32 = 3;
const RT_WRITE_STR: u32 = 4;
const RT_PRINT_NL: u32 = 5;
const RT_ARG: u32 = 6;
const RT_ARG_COUNT: u32 = 7;
const RT_SPEC_FAIL: u32 = 8;
const RT_COUNT: u32 = 9;

// ============================================================
//  Runtime helpers — native функции, JIT-ът ги импортира по име.
//  Форматите са БАЙТ-същите като codegen_c.c.
// ============================================================

extern "C" {
    fn printf(fmt: *const u8, ...) -> c_int;
}

const FMT_LLD: &[u8] = b"%lld\n\0";
const FMT_G: &[u8] = b"%g\n\0";
const FMT_S_NL: &[u8] = b"%s\n\0";
const FMT_S: &[u8] = b"%s\0";
const FMT_NL: &[u8] = b"\n\0";
const STR_TRUE: &[u8] = b"true\0";
const STR_FALSE: &[u8] = b"false\0";

#[no_mangle]
pub extern "C" fn baga_rt_print_i64(v: i64) {
    unsafe { printf(FMT_LLD.as_ptr(), v) };
}
#[no_mangle]
pub extern "C" fn baga_rt_print_f64(v: f64) {
    unsafe { printf(FMT_G.as_ptr(), v) };
}
#[no_mangle]
pub extern "C" fn baga_rt_print_str(s: *const u8) {
    unsafe { printf(FMT_S_NL.as_ptr(), s) };
}
#[no_mangle]
pub extern "C" fn baga_rt_write_str(s: *const u8) {
    unsafe { printf(FMT_S.as_ptr(), s) };
}
#[no_mangle]
pub extern "C" fn baga_rt_print_nl() {
    unsafe { printf(FMT_NL.as_ptr()) };
}
#[no_mangle]
pub extern "C" fn baga_rt_print_bool(b: i64) {
    unsafe { printf(FMT_S_NL.as_ptr(), if b != 0 { STR_TRUE.as_ptr() } else { STR_FALSE.as_ptr() }) };
}

// argc/argv — задават се от baga_jit_run_main преди изпълнение.
static mut ARGC: c_int = 0;
static mut ARGV: *mut *mut c_char = ptr::null_mut();
const EMPTY_STR: &[u8] = b"\0";

#[no_mangle]
pub extern "C" fn baga_rt_arg_count() -> i64 {
    unsafe { if ARGC > 0 { (ARGC - 1) as i64 } else { 0 } }
}
#[no_mangle]
pub extern "C" fn baga_rt_arg(i: i64) -> *const u8 {
    unsafe {
        if i + 1 < ARGC as i64 {
            *ARGV.offset((i + 1) as isize) as *const u8
        } else {
            EMPTY_STR.as_ptr()
        }
    }
}

#[no_mangle]
pub extern "C" fn baga_rt_spec_fail(spec: *const c_char, kind: *const c_char, idx: i64, expr: *const c_char) {
    let spec = unsafe { std::ffi::CStr::from_ptr(spec) }.to_string_lossy();
    let kind = unsafe { std::ffi::CStr::from_ptr(kind) }.to_string_lossy();
    let expr = unsafe { std::ffi::CStr::from_ptr(expr) }.to_string_lossy();
    if kind == "requires" {
        eprintln!("spec '{}': requires #{} нарушено: {}", spec, idx, expr);
    } else {
        eprintln!("spec '{}': ensures #{} нарушена: {}", spec, idx, expr);
    }
    std::process::exit(1);
}

// ============================================================
//  JIT модул
// ============================================================

struct Jit {
    module: JITModule,
    strings: Vec<DataId>,          // str_id -> DataId
    user_funcs: Vec<FuncId>,       // user_index -> FuncId
    user_sigs: Vec<Signature>,     // user_index -> signature
}

fn build_module() -> JITModule {
    let mut fb = settings::builder();
    fb.set("use_colocated_libcalls", "false").unwrap();
    fb.set("is_pic", "false").unwrap();
    let isa = cranelift_native::builder()
        .expect("host ISA")
        .finish(settings::Flags::new(fb))
        .unwrap();
    let mut builder = JITBuilder::with_isa(isa, cranelift_module::default_libcall_names());
    // Регистрирай runtime helper-ите във вътрешната символна таблица — dlsym
    // не ги намира (статично линкнати са), затова ги задаваме по pointer.
    builder.symbol("baga_rt_print_i64", baga_rt_print_i64 as *const u8);
    builder.symbol("baga_rt_print_f64", baga_rt_print_f64 as *const u8);
    builder.symbol("baga_rt_print_bool", baga_rt_print_bool as *const u8);
    builder.symbol("baga_rt_print_str", baga_rt_print_str as *const u8);
    builder.symbol("baga_rt_write_str", baga_rt_write_str as *const u8);
    builder.symbol("baga_rt_print_nl", baga_rt_print_nl as *const u8);
    builder.symbol("baga_rt_arg", baga_rt_arg as *const u8);
    builder.symbol("baga_rt_arg_count", baga_rt_arg_count as *const u8);
    builder.symbol("baga_rt_spec_fail", baga_rt_spec_fail as *const u8);
    JITModule::new(builder)
}

fn ty_from_code(module: &JITModule, code: u8) -> Type {
    match code {
        TY_I64 => types::I64,
        TY_I32 => types::I32,
        TY_F64 => types::F64,
        TY_BOOL => types::I8,
        TY_PTR => module.target_config().pointer_type(),
        _ => types::I64, // TY_VOID не се ползва като стойност
    }
}

// Име + сигнатура на runtime helper по RT_* id.
fn rt_info(module: &JITModule, rt: u32) -> (&'static str, Signature) {
    let ptr_ty = module.target_config().pointer_type();
    let mut s = module.make_signature();
    let name = match rt {
        RT_PRINT_I64 => { s.params.push(AbiParam::new(types::I64)); "baga_rt_print_i64" }
        RT_PRINT_F64 => { s.params.push(AbiParam::new(types::F64)); "baga_rt_print_f64" }
        RT_PRINT_BOOL => { s.params.push(AbiParam::new(types::I64)); "baga_rt_print_bool" }
        RT_PRINT_STR => { s.params.push(AbiParam::new(ptr_ty)); "baga_rt_print_str" }
        RT_WRITE_STR => { s.params.push(AbiParam::new(ptr_ty)); "baga_rt_write_str" }
        RT_PRINT_NL => "baga_rt_print_nl",
        RT_ARG => { s.params.push(AbiParam::new(types::I64)); s.returns.push(AbiParam::new(ptr_ty)); "baga_rt_arg" }
        RT_ARG_COUNT => { s.returns.push(AbiParam::new(types::I64)); "baga_rt_arg_count" }
        RT_SPEC_FAIL => {
            s.params.push(AbiParam::new(ptr_ty));
            s.params.push(AbiParam::new(ptr_ty));
            s.params.push(AbiParam::new(types::I64));
            s.params.push(AbiParam::new(ptr_ty));
            "baga_rt_spec_fail"
        }
        _ => panic!("невалиден RT id {}", rt),
    };
    (name, s)
}

// ============================================================
//  FFI: жизнен цикъл
// ============================================================

#[no_mangle]
pub extern "C" fn baga_jit_new() -> *mut Jit {
    Box::into_raw(Box::new(Jit {
        module: build_module(),
        strings: Vec::new(),
        user_funcs: Vec::new(),
        user_sigs: Vec::new(),
    }))
}

#[no_mangle]
pub extern "C" fn baga_jit_free(p: *mut Jit) {
    if !p.is_null() {
        unsafe { drop(Box::from_raw(p)); }
    }
}

#[no_mangle]
pub extern "C" fn baga_jit_intern_str(jit: *mut Jit, bytes: *const u8, len: usize) -> c_int {
    let j = unsafe { &mut *jit };
    let mut desc = DataDescription::new();
    let slice = unsafe { std::slice::from_raw_parts(bytes, len) };
    let mut v = slice.to_vec();
    v.push(0); // trailing NUL за printf("%s")
    desc.define(v.into_boxed_slice());
    let name = format!("str{}", j.strings.len());
    let id = j.module.declare_data(&name, Linkage::Local, false, false).unwrap();
    j.module.define_data(id, &desc).unwrap();
    j.strings.push(id);
    (j.strings.len() - 1) as c_int
}

#[no_mangle]
pub extern "C" fn baga_jit_declare(
    jit: *mut Jit,
    name: *const c_char,
    ret_ty: c_int,
    param_tys: *const c_int,
    nparams: c_int,
) -> c_int {
    let j = unsafe { &mut *jit };
    let name = unsafe { std::ffi::CStr::from_ptr(name) }.to_str().unwrap();
    let mut sig = j.module.make_signature();
    for i in 0..nparams as isize {
        let code = unsafe { *param_tys.offset(i) } as u8;
        sig.params.push(AbiParam::new(ty_from_code(&j.module, code)));
    }
    if ret_ty as u8 != TY_VOID {
        sig.returns.push(AbiParam::new(ty_from_code(&j.module, ret_ty as u8)));
    }
    let id = j.module.declare_function(name, Linkage::Local, &sig).unwrap();
    j.user_funcs.push(id);
    j.user_sigs.push(sig);
    (j.user_funcs.len() - 1) as c_int
}

#[no_mangle]
pub extern "C" fn baga_jit_define(
    jit: *mut Jit,
    user_index: c_int,
    code: *const u8,
    code_len: usize,
) -> c_int {
    let j = unsafe { &mut *jit };
    let code = unsafe { std::slice::from_raw_parts(code, code_len) };
    let idx = user_index as usize;
    let func_id = j.user_funcs[idx];
    let sig = j.user_sigs[idx].clone();
    let strings = j.strings.clone();
    let user_funcs = j.user_funcs.clone();
    let user_sigs = j.user_sigs.clone();
    let module = &mut j.module;
    define_function(module, func_id, &sig, &strings, &user_funcs, &user_sigs, code);
    0
}

#[no_mangle]
pub extern "C" fn baga_jit_run_main(jit: *mut Jit, argc: c_int, argv: *mut *mut c_char) -> c_int {
    let j = unsafe { &mut *jit };
    unsafe {
        ARGC = argc;
        ARGV = argv;
    }
    let main_id = j.module.get_name("main");
    j.module.finalize_definitions().unwrap();
    match main_id {
        Some(FuncOrDataId::Func(id)) => {
            let code = j.module.get_finalized_function(id);
            let f: unsafe extern "C" fn() = unsafe { std::mem::transmute(code) };
            unsafe { f() };
            0
        }
        _ => {
            eprintln!("baga: Cranelift backend: няма функция main");
            1
        }
    }
}

// ============================================================
//  Интерпретатор bytecode -> Cranelift
// ============================================================

struct Reader<'a> {
    code: &'a [u8],
    pos: usize,
}
impl<'a> Reader<'a> {
    fn u8(&mut self) -> u8 {
        let v = self.code[self.pos];
        self.pos += 1;
        v
    }
    fn u16(&mut self) -> u16 {
        let v = u16::from_le_bytes([self.code[self.pos], self.code[self.pos + 1]]);
        self.pos += 2;
        v
    }
    fn u32(&mut self) -> u32 {
        let mut b = [0u8; 4];
        b.copy_from_slice(&self.code[self.pos..self.pos + 4]);
        self.pos += 4;
        u32::from_le_bytes(b)
    }
    fn i64(&mut self) -> i64 {
        let mut b = [0u8; 8];
        b.copy_from_slice(&self.code[self.pos..self.pos + 8]);
        self.pos += 8;
        i64::from_le_bytes(b)
    }
    fn f64(&mut self) -> f64 {
        let mut b = [0u8; 8];
        b.copy_from_slice(&self.code[self.pos..self.pos + 8]);
        self.pos += 8;
        f64::from_le_bytes(b)
    }
}

fn skip_operands(r: &mut Reader, op: u8) {
    match op {
        OP_ICONST => { r.i64(); }
        OP_FCONST => { r.f64(); }
        OP_BCONST => { r.u8(); }
        OP_SCONST => { r.u32(); }
        OP_LOAD | OP_STORE => { r.u16(); }
        OP_ALLOCA => { r.u16(); r.u8(); }
        OP_BINOP => { r.u8(); }
        OP_NEG => { r.u8(); }
        OP_CALL => { r.u32(); r.u16(); }
        OP_BR | OP_BR_FALSE | OP_LABEL => { r.u32(); }
        _ => {}
    }
}

fn define_function(
    module: &mut JITModule,
    func_id: FuncId,
    sig: &Signature,
    strings: &[DataId],
    user_funcs: &[FuncId],
    user_sigs: &[Signature],
    code: &[u8],
) {
    let ptr_ty = module.target_config().pointer_type();
    let mut ctx = module.make_context();
    ctx.func.signature = sig.clone();

    let mut fbctx = FunctionBuilderContext::new();
    let mut bcx = FunctionBuilder::new(&mut ctx.func, &mut fbctx);

    // Pass 1: създай блокове за всички LABEL-и.
    let mut labels: std::collections::HashMap<u32, Block> = std::collections::HashMap::new();
    {
        let mut r = Reader { code, pos: 0 };
        while r.pos < r.code.len() {
            let op = r.u8();
            if op == OP_LABEL {
                let k = r.u32();
                labels.entry(k).or_insert_with(|| bcx.create_block());
            } else {
                skip_operands(&mut r, op);
            }
        }
    }

    // fallthrough: за BR_FALSE на позиция p -> label-ът на следващия LABEL.
    let mut fallthrough: std::collections::HashMap<usize, u32> = std::collections::HashMap::new();
    {
        let mut r = Reader { code, pos: 0 };
        while r.pos < r.code.len() {
            let start = r.pos;
            let op = r.u8();
            if op == OP_BR_FALSE {
                r.u32();
                let mut scan = Reader { code, pos: r.pos };
                while scan.pos < scan.code.len() {
                    let o2 = scan.u8();
                    if o2 == OP_LABEL {
                        fallthrough.insert(start, scan.u32());
                        break;
                    }
                    skip_operands(&mut scan, o2);
                }
            } else {
                skip_operands(&mut r, op);
            }
        }
    }

    // Stack slots за локалите (slot -> (StackSlot, Type)).
    let mut slots: std::collections::HashMap<u16, (StackSlot, Type)> =
        std::collections::HashMap::new();
    let mut stack: Vec<(Value, Type)> = Vec::new();
    let mut entry_done = false;

    let mut r = Reader { code, pos: 0 };
    while r.pos < r.code.len() {
        let instr_start = r.pos;
        let op = r.u8();
        match op {
            OP_LABEL => {
                let k = r.u32();
                let blk = labels[&k];
                bcx.switch_to_block(blk);
                if !entry_done {
                    bcx.append_block_params_for_function_params(blk);
                    let params = bcx.block_params(blk).to_vec();
                    for (i, pv) in params.iter().enumerate() {
                        let ty = sig.params[i].value_type;
                        let slot = bcx.func.create_sized_stack_slot(StackSlotData::new(
                            StackSlotKind::ExplicitSlot, 8, 0));
                        bcx.ins().stack_store(ptr_ty, *pv, slot, 0);
                        slots.insert(i as u16, (slot, ty));
                    }
                    entry_done = true;
                }
            }
            OP_ICONST => {
                let val = bcx.ins().iconst(types::I64, r.i64());
                stack.push((val, types::I64));
            }
            OP_FCONST => {
                let val = bcx.ins().f64const(r.f64());
                stack.push((val, types::F64));
            }
            OP_BCONST => {
                let val = bcx.ins().iconst(types::I8, r.u8() as i64);
                stack.push((val, types::I8));
            }
            OP_SCONST => {
                let id = r.u32() as usize;
                let gv = module.declare_data_in_func(strings[id], &mut bcx.func);
                let val = bcx.ins().symbol_value(ptr_ty, gv);
                stack.push((val, ptr_ty));
            }
            OP_ALLOCA => {
                let slot_idx = r.u16();
                let ty = ty_from_code(module, r.u8());
                let slot = bcx.func.create_sized_stack_slot(StackSlotData::new(
                    StackSlotKind::ExplicitSlot, 8, 0));
                slots.insert(slot_idx, (slot, ty));
            }
            OP_LOAD => {
                let (slot, ty) = slots[&r.u16()];
                let val = bcx.ins().stack_load(ptr_ty, ty, slot, 0);
                stack.push((val, ty));
            }
            OP_STORE => {
                let (slot, _) = slots[&r.u16()];
                let (v, _) = stack.pop().unwrap();
                bcx.ins().stack_store(ptr_ty, v, slot, 0);
            }
            OP_BINOP => {
                let bop = r.u8();
                let (rb, tb) = stack.pop().unwrap();
                let (ra, ta) = stack.pop().unwrap();
                if bop == B_MOD_F {
                    // f64 % — libc fmod
                    let mut fsig = module.make_signature();
                    fsig.params.push(AbiParam::new(types::F64));
                    fsig.params.push(AbiParam::new(types::F64));
                    fsig.returns.push(AbiParam::new(types::F64));
                    let fid = module.declare_function("fmod", Linkage::Import, &fsig).unwrap();
                    let fr = module.declare_func_in_func(fid, &mut bcx.func);
                    let call = bcx.ins().call(fr, &[ra, rb]);
                    let res = bcx.inst_results(call).to_vec()[0];
                    stack.push((res, types::F64));
                } else {
                    let (res, ty) = emit_binop(&mut bcx, bop, ra, ta, rb, tb);
                    stack.push((res, ty));
                }
            }
            OP_AND => {
                let (rb, _) = stack.pop().unwrap();
                let (ra, _) = stack.pop().unwrap();
                let res = bcx.ins().band(ra, rb);
                stack.push((res, types::I8));
            }
            OP_OR => {
                let (rb, _) = stack.pop().unwrap();
                let (ra, _) = stack.pop().unwrap();
                let res = bcx.ins().bor(ra, rb);
                stack.push((res, types::I8));
            }
            OP_NOT => {
                let (ra, _) = stack.pop().unwrap();
                let zero = bcx.ins().iconst(types::I8, 0);
                let res = bcx.ins().icmp(cranelift_codegen::ir::condcodes::IntCC::Equal, ra, zero);
                stack.push((res, types::I8));
            }
            OP_NEG => {
                let ty_code = r.u8();
                let (ra, ta) = stack.pop().unwrap();
                let res = if ty_code == TY_F64 || ta == types::F64 {
                    bcx.ins().fneg(ra)
                } else {
                    let zero = bcx.ins().iconst(ta, 0);
                    bcx.ins().isub(zero, ra)
                };
                stack.push((res, ta));
            }
            OP_PROMOTE => {
                let (ra, _) = stack.pop().unwrap();
                let res = bcx.ins().fcvt_from_sint(types::F64, ra);
                stack.push((res, types::F64));
            }
            OP_CALL => {
                let fn_id = r.u32();
                let nargs = r.u16() as usize;
                let mut args: Vec<(Value, Type)> = Vec::with_capacity(nargs);
                for _ in 0..nargs {
                    args.push(stack.pop().unwrap());
                }
                args.reverse();
                let (func_ref, callee_sig) = if fn_id < RT_COUNT {
                    let (name, s) = rt_info(module, fn_id);
                    let fid = module.declare_function(name, Linkage::Import, &s).unwrap();
                    let fr = module.declare_func_in_func(fid, &mut bcx.func);
                    (fr, s)
                } else {
                    let ui = (fn_id - RT_COUNT) as usize;
                    let fid = user_funcs[ui];
                    let s = user_sigs[ui].clone();
                    let fr = module.declare_func_in_func(fid, &mut bcx.func);
                    (fr, s)
                };
                let mut coerced: Vec<Value> = Vec::with_capacity(nargs);
                for (i, (av, at)) in args.iter().enumerate() {
                    let want = callee_sig.params[i].value_type;
                    coerced.push(coerce(&mut bcx, *av, *at, want));
                }
                let call = bcx.ins().call(func_ref, &coerced);
                let results = bcx.inst_results(call).to_vec();
                if !results.is_empty() {
                    let rt = callee_sig.returns[0].value_type;
                    stack.push((results[0], rt));
                }
            }
            OP_RET => {
                let (v, _) = stack.pop().unwrap();
                bcx.ins().return_(&[v]);
            }
            OP_RET_VOID => {
                bcx.ins().return_(&[]);
            }
            OP_BR => {
                let blk = labels[&r.u32()];
                bcx.ins().jump(blk, &[]);
            }
            OP_BR_FALSE => {
                let lbl_f = r.u32();
                let (cond, _) = stack.pop().unwrap();
                let blk_f = labels[&lbl_f];
                let lbl_t = fallthrough[&instr_start];
                let blk_t = labels[&lbl_t];
                bcx.ins().brif(cond, blk_t, &[], blk_f, &[]);
            }
            OP_DROP => {
                stack.pop();
            }
            _ => panic!("непознат opcode {}", op),
        }
    }

    // seal на всички блокове (всички predecessors са известни).
    let all_blocks: Vec<Block> = labels.values().copied().collect();
    for b in all_blocks {
        bcx.seal_block(b);
    }

    bcx.finalize(module.target_config());
    module.define_function(func_id, &mut ctx).unwrap();
    module.clear_context(&mut ctx);
}

fn emit_binop(
    bcx: &mut FunctionBuilder,
    bop: u8,
    ra: Value,
    ta: Type,
    rb: Value,
    tb: Type,
) -> (Value, Type) {
    use cranelift_codegen::ir::condcodes::{FloatCC, IntCC};
    let _ = tb;
    match bop {
        B_ADD_I => (bcx.ins().iadd(ra, rb), ta),
        B_SUB_I => (bcx.ins().isub(ra, rb), ta),
        B_MUL_I => (bcx.ins().imul(ra, rb), ta),
        B_DIV_I => (bcx.ins().sdiv(ra, rb), ta),
        B_MOD_I => (bcx.ins().srem(ra, rb), ta),
        B_ADD_F => (bcx.ins().fadd(ra, rb), types::F64),
        B_SUB_F => (bcx.ins().fsub(ra, rb), types::F64),
        B_MUL_F => (bcx.ins().fmul(ra, rb), types::F64),
        B_DIV_F => (bcx.ins().fdiv(ra, rb), types::F64),
        B_EQ_I => (bcx.ins().icmp(IntCC::Equal, ra, rb), types::I8),
        B_NEQ_I => (bcx.ins().icmp(IntCC::NotEqual, ra, rb), types::I8),
        B_LT_I => (bcx.ins().icmp(IntCC::SignedLessThan, ra, rb), types::I8),
        B_GT_I => (bcx.ins().icmp(IntCC::SignedGreaterThan, ra, rb), types::I8),
        B_LE_I => (bcx.ins().icmp(IntCC::SignedLessThanOrEqual, ra, rb), types::I8),
        B_GE_I => (bcx.ins().icmp(IntCC::SignedGreaterThanOrEqual, ra, rb), types::I8),
        B_EQ_F => (bcx.ins().fcmp(FloatCC::Equal, ra, rb), types::I8),
        B_NEQ_F => (bcx.ins().fcmp(FloatCC::NotEqual, ra, rb), types::I8),
        B_LT_F => (bcx.ins().fcmp(FloatCC::LessThan, ra, rb), types::I8),
        B_GT_F => (bcx.ins().fcmp(FloatCC::GreaterThan, ra, rb), types::I8),
        B_LE_F => (bcx.ins().fcmp(FloatCC::LessThanOrEqual, ra, rb), types::I8),
        B_GE_F => (bcx.ins().fcmp(FloatCC::GreaterThanOrEqual, ra, rb), types::I8),
        B_BAND_I => (bcx.ins().band(ra, rb), ta),
        B_BOR_I => (bcx.ins().bor(ra, rb), ta),
        B_BXOR_I => (bcx.ins().bxor(ra, rb), ta),
        B_SHL_I => (bcx.ins().ishl(ra, rb), ta),
        B_SHR_I => (bcx.ins().sshr(ra, rb), ta),
        _ => panic!("непознат binop {}", bop),
    }
}

// Привеждане на стойност `v` (тип `from`) към целеви тип `to`.
fn coerce(bcx: &mut FunctionBuilder, v: Value, from: Type, to: Type) -> Value {
    if from == to {
        return v;
    }
    if from == types::F64 && to.is_int() {
        return bcx.ins().fcvt_to_sint(to, v);
    }
    if from.is_int() && to == types::F64 {
        return bcx.ins().fcvt_from_sint(to, v);
    }
    if from.is_int() && to.is_int() {
        if from.bits() < to.bits() {
            return bcx.ins().sextend(to, v);
        } else if from.bits() > to.bits() {
            return bcx.ins().ireduce(to, v);
        }
    }
    v
}

// ============================================================
//  Тестове
// ============================================================

#[cfg(test)]
mod tests {
    use super::*;

    // fn main() -> void { print_i64(40 + 2) } — очакваме "42" на stdout, exit 0.
    #[test]
    fn jit_print_add() {
        let mut code: Vec<u8> = Vec::new();
        code.push(OP_LABEL); code.extend(&0u32.to_le_bytes());
        code.push(OP_ICONST); code.extend(&40i64.to_le_bytes());
        code.push(OP_ICONST); code.extend(&2i64.to_le_bytes());
        code.push(OP_BINOP); code.push(B_ADD_I);
        code.push(OP_CALL); code.extend(&RT_PRINT_I64.to_le_bytes()); code.extend(&1u16.to_le_bytes());
        code.push(OP_RET_VOID);

        let jit = baga_jit_new();
        let name = std::ffi::CString::new("main").unwrap();
        let ui = baga_jit_declare(jit, name.as_ptr(), TY_VOID as c_int, std::ptr::null(), 0);
        baga_jit_define(jit, ui, code.as_ptr(), code.len());
        let rc = baga_jit_run_main(jit, 0, std::ptr::null_mut());
        assert_eq!(rc, 0);
        baga_jit_free(jit);
    }
}
