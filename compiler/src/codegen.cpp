// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * codegen.cpp -- emit LLVM IR for parsed C-specs.
 *
 * High-level structure of the generated @main:
 *
 *   entry:
 *       (nothing to init in Phase 2 -- indicators are already zero)
 *       br %first_spec_block
 *   spec1:
 *       [condition tests]  -> br %do1 or %spec2
 *   do1:                  ; the actual operation
 *       <op IR>
 *       br %spec2
 *   spec2: ...
 *   ...
 *   exit:
 *       ret i32 <exit value>
 *
 * The exit value in Phase 2 is the low byte of the LR indicator's slot so a
 * test can observe SETON LR. We also expose the first general indicator (01)
 * as an alternative observable. For a no-op program (no specs) we return 0.
 * ========================================================================== */
#include "codegen.h"
#include "diagnostics.h"
#include "symbols.h"
#include "program.h"
#include "fspec.h"
#include "ispec.h"
#include "ospec.h"
#include "espec.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

namespace rpgc {

namespace {

/* Upper-case a copy of `s` (program-linkage names are matched case-
 * insensitively; the values here are already column-trimmed by the parser). */
std::string upper_trim(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return std::toupper(ch); });
    return s;
}

/* ------------------------------------------------------------------------- *
 * IndicatorEmitter -- owns the [100 x i1] indicator array and the LR latch,
 * and provides typed load/store helpers. Slots 1..99 are addressable; LR is a
 * separate i1 global.
 * ------------------------------------------------------------------------- */
class IndicatorEmitter {
public:
    IndicatorEmitter(llvm::Module &m, llvm::IRBuilder<> &b)
        : ctx_(m.getContext()), mod_(m), builder_(b) {
        using namespace llvm;
        auto *arrTy = ArrayType::get(Type::getInt1Ty(ctx_), 100);
        in_ = new GlobalVariable(
            mod_, arrTy, /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantAggregateZero::get(arrTy), "rpg_in");
        lr_ = new GlobalVariable(
            mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantInt::getFalse(ctx_), "rpg_lr");
        // Control-level indicators L1-L9 (one i1 global each).
        for (int i = 1; i <= 9; ++i) {
            ctl_[i] = new GlobalVariable(
                mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
                GlobalValue::InternalLinkage,
                ConstantInt::getFalse(ctx_),
                "rpg_l" + std::to_string(i));
        }
        // First-page indicator 1P: on at program start, off after the heading
        // pass (Section D, D12).
        first_page_ = new GlobalVariable(
            mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantInt::getTrue(ctx_), "rpg_1p");
        // Matching-record indicator MR (Section F, F20).
        mr_ = new GlobalVariable(
            mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantInt::getFalse(ctx_), "rpg_mr");
        // Overflow indicators OA-OG (Section F, F22).
        for (int i = 0; i < 7; ++i) {
            ov_[i] = new GlobalVariable(
                mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
                GlobalValue::InternalLinkage,
                ConstantInt::getFalse(ctx_),
                "rpg_o" + std::string(1, 'A' + (char)i));   // rpg_oa .. rpg_og
        }
        // Overflow indicator OV (Section F, F22).
        ovv_ = new GlobalVariable(
            mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantInt::getFalse(ctx_), "rpg_ov");
        // External indicators U1-U8 (A4): real backing so a calc conditioned
        // solely on one of these evaluates false-by-default rather than being
        // dropped from its AND-group and running unconditionally.
        for (int i = 0; i < 8; ++i) {
            ext_[i] = new GlobalVariable(
                mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
                GlobalValue::InternalLinkage,
                ConstantInt::getFalse(ctx_), "rpg_u" + std::to_string(i + 1));
        }
        // Halt indicators H1-H9 (A4).
        for (int i = 0; i < 9; ++i) {
            halt_[i] = new GlobalVariable(
                mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
                GlobalValue::InternalLinkage,
                ConstantInt::getFalse(ctx_), "rpg_h" + std::to_string(i + 1));
        }
        // Function-key indicators KA-KY (A4).
        for (int i = 0; i < 25; ++i) {
            key_[i] = new GlobalVariable(
                mod_, Type::getInt1Ty(ctx_), /*isConstant=*/false,
                GlobalValue::InternalLinkage,
                ConstantInt::getFalse(ctx_),
                "rpg_k" + std::string(1, (char)('a' + i)));
        }
    }

    /* GEP to indicator slot i (1..99). */
    llvm::Value *addr(int i) {
        using namespace llvm;
        assert(i >= 1 && i <= 99);
        Value *idx[] = {
            ConstantInt::get(Type::getInt32Ty(ctx_), 0),
            ConstantInt::get(Type::getInt32Ty(ctx_), (unsigned)i),
        };
        return builder_.CreateInBoundsGEP(
            ArrayType::get(Type::getInt1Ty(ctx_), 100), in_, idx,
            "ind" + std::to_string(i) + "_p");
    }

    llvm::Value *load(int i) {
        return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), addr(i),
                                   "ind" + std::to_string(i));
    }

    void store(int i, llvm::Value *v) {
        builder_.CreateStore(v, addr(i));
    }

    /* Resolve an indicator index from a parsed token:
     *   -1      => LR
     *   -2..-10 => L1..L9
     *   -11     => 1P (first-page)
     *   -12     => MR (matching record)
     *   -13..-19=> OA..OG (overflow)
     *   -20     => OV (overflow)
     *   -21..-28=> U1..U8 (external)
     *   -29..-37=> H1..H9 (halt)
     *   -38..-62=> KA..KY (function key)
     *   1..99   => general indicator slot
     */
    llvm::Value *load_resolved(int idx) {
        if (idx == -1)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), lr_, "lr");
        if (idx == -11)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), first_page_, "p1");
        if (idx == -12)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), mr_, "mr");
        if (idx <= -13 && idx >= -19) {   // OA..OG
            int i = -13 - idx;            // OA(0) .. OG(6)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), ov_[i],
                                       "o" + std::string(1, 'A' + i));
        }
        if (idx == -20)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), ovv_, "ov");
        if (idx <= -21 && idx >= -28) {   // U1..U8
            int i = -21 - idx;
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), ext_[i],
                                       "u" + std::to_string(i + 1));
        }
        if (idx <= -29 && idx >= -37) {   // H1..H9
            int i = -29 - idx;
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), halt_[i],
                                       "h" + std::to_string(i + 1));
        }
        if (idx <= -38 && idx >= -62) {   // KA..KY
            int i = -38 - idx;
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), key_[i],
                                       "k" + std::string(1, 'a' + i));
        }
        if (idx <= -2 && idx >= -10)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_),
                                       ctl_[-idx - 1], "l" + std::to_string(-idx-1));
        return load(idx);
    }
    void store_resolved(int idx, llvm::Value *v) {
        if (idx == -1) { builder_.CreateStore(v, lr_); return; }
        if (idx == -11) { builder_.CreateStore(v, first_page_); return; }
        if (idx == -12) { builder_.CreateStore(v, mr_); return; }
        if (idx <= -13 && idx >= -19) {
            builder_.CreateStore(v, ov_[-13 - idx]); return;
        }
        if (idx == -20) { builder_.CreateStore(v, ovv_); return; }
        if (idx <= -21 && idx >= -28) { builder_.CreateStore(v, ext_[-21 - idx]); return; }
        if (idx <= -29 && idx >= -37) { builder_.CreateStore(v, halt_[-29 - idx]); return; }
        if (idx <= -38 && idx >= -62) { builder_.CreateStore(v, key_[-38 - idx]); return; }
        if (idx <= -2 && idx >= -10) {
            builder_.CreateStore(v, ctl_[-idx - 1]); return;
        }
        store(idx, v);
    }

    /* Turn off all control-level indicators L1-L9 (cycle step 6). */
    void reset_control_levels() {
        using namespace llvm;
        auto *f = ConstantInt::getFalse(ctx_);
        for (int i = 1; i <= 9; ++i) builder_.CreateStore(f, ctl_[i]);
    }
    /* Set on L1..Ln (cascade) — used when level n breaks. */
    void cascade_on(int n) {
        using namespace llvm;
        auto *t = ConstantInt::getTrue(ctx_);
        for (int i = 1; i <= n; ++i) builder_.CreateStore(t, ctl_[i]);
    }
    /* Set on L1..L9 and LR — used at last record. */
    void set_all_for_lr() {
        using namespace llvm;
        auto *t = ConstantInt::getTrue(ctx_);
        for (int i = 1; i <= 9; ++i) builder_.CreateStore(t, ctl_[i]);
        builder_.CreateStore(t, lr_);
    }
    /* Turn off 1P after the heading pass (Section D, D12). */
    void clear_first_page() {
        builder_.CreateStore(llvm::ConstantInt::getFalse(ctx_), first_page_);
    }

    llvm::GlobalVariable *lr() { return lr_; }
    llvm::GlobalVariable *in() { return in_; }

private:
    llvm::LLVMContext  &ctx_;
    llvm::Module       &mod_;
    llvm::IRBuilder<>  &builder_;
    llvm::GlobalVariable *in_ = nullptr;
    llvm::GlobalVariable *lr_ = nullptr;
    llvm::GlobalVariable *first_page_ = nullptr;   // 1P
    llvm::GlobalVariable *mr_ = nullptr;           // MR (matching record)
    llvm::GlobalVariable *ov_[7] = {};             // OA..OG
    llvm::GlobalVariable *ovv_ = nullptr;          // OV
    llvm::GlobalVariable *ext_[8] = {};            // U1..U8
    llvm::GlobalVariable *halt_[9] = {};           // H1..H9
    llvm::GlobalVariable *key_[25] = {};           // KA..KY
    llvm::GlobalVariable *ctl_[10] = {};   // ctl_[1..9] = L1..L9
};

/* Evaluate the AND-chain of conditioning indicators. Returns an i1 value in
 * the current block that is true iff every condition holds. Empty condition
 * list => true. */
llvm::Value *eval_conditions(IndicatorEmitter &ie,
                             llvm::IRBuilder<> &b,
                             const std::vector<CondInd> &conds) {
    using namespace llvm;
    if (conds.empty())
        return ConstantInt::getTrue(b.getContext());

    Value *acc = nullptr;
    for (const auto &c : conds) {
        Value *raw = ie.load_resolved(c.indicator);
        Value *v   = c.negate ? b.CreateNot(raw, "nind") : raw;
        acc = acc ? b.CreateAnd(acc, v, "andc") : v;
    }
    return acc;
}

/* F1: evaluate O-spec record-line conditioning as OR-of-AND groups (group 0
 * is the record line's own 3 slots; each AND continuation line extends the
 * current group, each OR continuation starts a new one). Empty group list
 * => true (unconditional), matching eval_conditions' empty-list convention;
 * an individual empty group (a bare record line with no indicators at all)
 * is also true, so its OR contributes an unconditional true as expected. */
llvm::Value *eval_conditions_grouped(
        IndicatorEmitter &ie, llvm::IRBuilder<> &b,
        const std::vector<std::vector<CondInd>> &groups) {
    using namespace llvm;
    if (groups.empty())
        return ConstantInt::getTrue(b.getContext());
    Value *acc = nullptr;
    for (const auto &g : groups) {
        Value *gval = eval_conditions(ie, b, g);
        acc = acc ? b.CreateOr(acc, gval, "orc") : gval;
    }
    return acc;
}

/* Emit the result-field sign tests for arithmetic ops (HI/LO/EQ).
 * Per the manual: HI on positive, LO on negative, EQ on zero. We turn OFF any
 * slot not triggered and ON the slot that matches, matching RPG latch
 * semantics for the three indicator positions of an arithmetic op. */
void emit_arith_result_indicators(IndicatorEmitter &ie,
                                  llvm::IRBuilder<> &b,
                                  const CSpec &c,
                                  llvm::Value *result) {
    using namespace llvm;
    auto *zero = ConstantInt::get(result->getType(), 0, true);

    // We turn the relevant slot ON and the others OFF for this op. Use select.
    auto setslot = [&](ResultInd slot, ICmpInst::Predicate yes) {
        if (slot.indicator == 0) return;
        Value *cmp = b.CreateICmp(yes, result, zero, "res_cmp");
        ie.store_resolved(slot.indicator, cmp);
    };
    // Careful: sign tests on signed integer via ICmp with signed predicates.
    setslot(c.hi, ICmpInst::ICMP_SGT); // positive
    setslot(c.lo, ICmpInst::ICMP_SLT); // negative
    setslot(c.eq, ICmpInst::ICMP_EQ ); // zero
}

class CodeGen {
public:
    CodeGen(llvm::LLVMContext &ctx, const std::string &name,
            bool is_top_level = true)
        : mod_(std::make_unique<llvm::Module>(name, ctx)),
          builder_(ctx),
          syms_(*mod_, builder_),
          inds_(*mod_, builder_),
          is_top_level_(is_top_level) {}

    llvm::Module *module() { return mod_.get(); }
    std::unique_ptr<llvm::Module> take_module() { return std::move(mod_); }

    /* Top-level: generate either the cycle (if a primary input file exists)
     * or the linear form (C-specs once). */
    bool generate(const Program &prog) {
        // D1: H-spec col 18 currency symbol and cols 75-80 program
        // identification (manual Ch. 18). Currency feeds the floating-
        // currency-symbol detection in emit_edit_word_field; program-id, when
        // actually specified, renames the LLVM module (visible in --emit-ir
        // output) instead of the default source-filename-derived identifier
        // -- left alone when blank so the ~70 existing tests without an
        // H-spec see no behavior change.
        currency_symbol_ = prog.hspec.currency_symbol;
        if (!prog.hspec.program_id.empty())
            mod_->setModuleIdentifier(prog.hspec.program_id);
        // Program linkage: the registry key / entry symbol name is the
        // H-spec program-id if given, else the module name (input file
        // stem), upper-cased either way so CALL/FREE name matching is
        // case-insensitive regardless of source style.
        program_name_ = upper_trim(!prog.hspec.program_id.empty()
                                        ? prog.hspec.program_id
                                        : mod_->getModuleIdentifier());
        param_lists_ = &prog.param_lists;
        exit_decls_  = &prog.exit_decls;
        declare_runtime();
        // Create globals for every E-spec array/table, numeric or alphameric
        // (A9). Tables (TAB-prefixed) additionally get a current-element
        // shadow index. An alternating partner (cols 46-51) becomes its own
        // array/table global.
        for (const auto &a : prog.arrays) {
            if (a.decimals >= 0)
                syms_.get_or_create_array(a.name, a.entries, a.init_data,
                                          is_table_name(a.name));
            else
                syms_.get_or_create_char_array(a.name, a.entries, a.entry_len,
                                               a.init_str, is_table_name(a.name));
            if (!a.alt_name.empty()) {
                if (a.alt_decimals >= 0)
                    syms_.get_or_create_array(a.alt_name, a.entries, a.alt_init_data,
                                              is_table_name(a.alt_name));
                else
                    syms_.get_or_create_char_array(a.alt_name, a.entries,
                                                   a.alt_entry_len, a.alt_init_str,
                                                   is_table_name(a.alt_name));
            }
        }
        arrays_ = &prog.arrays;
        // Stash the O-spec records so EXCPT can reach them from calc time.
        outputs_ = &prog.outputs;
        // Stash F-specs and I-fields for calc-time file operations (Section G).
        files_ = &prog.files;
        in_fields_ = &prog.in_fields;
        // D2: pre-declare every ordinary I-spec field's storage (idempotent;
        // the extract loop's own get_or_create_field/get_or_create_char_field
        // calls just find these already made) so data-structure redefinition
        // below can find the field it's redefining regardless of source
        // order, and so a subroutine that references a field before the
        // cycle's extract loop runs sees the right kind (Numeric/Character)
        // instead of falling into resolve_operand's numeric-by-default
        // auto-create fallback.
        declare_all_input_fields(prog);
        // D2: data structures. Must run after declare_all_input_fields (so a
        // DS that redefines an input field finds it) and before
        // generate_subroutines (so calc code can reference DS subfields).
        declare_data_structures(prog);
        // Compile subroutines first so EXSR calls in main can resolve.
        generate_subroutines(prog.calcs);
        if (!secondary_inputs(prog.files).empty() && find_primary_input(prog.files)) {
            // Multifile processing: one primary + one or more secondary input
            // files, with optional M1 matching (Section F, F20/F21).
            return generate_multifile_cycle(prog);
        }
        if (find_primary_input(prog.files)) {
            return generate_cycle(prog);
        }
        return generate_linear(prog);
    }

    /* Program linkage: the function generated
     * for this program's body, plus (for a non-top-level "library" program)
     * the incoming parameter-block arguments. */
    struct EntryInfo {
        llvm::Function *fn = nullptr;
        llvm::Value *parm_ptrs  = nullptr;  // ptr;  null for a top-level program
        llvm::Value *parm_count = nullptr;  // i32;  null for a top-level program
        llvm::Value *first_call = nullptr;  // i32;  null for a top-level program
    };

    /* Create this program's entry function and, for a "library" program
     * (is_top_level_ == false), register it into the runtime program
     * registry via a global constructor. A top-level program keeps today's
     * exact shape (`i32 @main()`) so single-file compiles are byte-for-byte
     * unchanged; a library program instead gets a uniquely-named, uniformly-
     * shaped rpg_entry_fn (see rpg_runtime.h) that CALL dispatches to
     * through the registry rather than a direct LLVM call. */
    EntryInfo create_entry_function() {
        using namespace llvm;
        auto &c = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        EntryInfo ei;
        if (is_top_level_) {
            FunctionType *ft = FunctionType::get(i32, false);
            ei.fn = Function::Create(ft, Function::ExternalLinkage, "main", mod_.get());
            return ei;
        }
        auto *ptrTy = PointerType::get(c, 0);
        FunctionType *ft = FunctionType::get(i32, {ptrTy, i32, i32}, false);
        ei.fn = Function::Create(ft, Function::ExternalLinkage,
                                 "rpg_prog_" + program_name_, mod_.get());
        auto argit = ei.fn->arg_begin();
        ei.parm_ptrs = &*argit++;  ei.parm_ptrs->setName("parm_ptrs");
        ei.parm_count = &*argit++; ei.parm_count->setName("parm_count");
        ei.first_call = &*argit++; ei.first_call->setName("first_call");

        // Self-register: emit a constructor that calls
        // rpg_rt_register_program(program_name_, &this_function) at process
        // startup, priority 65535 (default/unspecified) same as any other
        // global constructor.
        auto *voidTy = Type::getVoidTy(c);
        FunctionType *ctorTy = FunctionType::get(voidTy, false);
        Function *ctor = Function::Create(ctorTy, Function::InternalLinkage,
                                          "rpg_register_" + program_name_, mod_.get());
        BasicBlock *cbb = BasicBlock::Create(c, "entry", ctor);
        IRBuilder<> cb(cbb);
        Value *nameStr = cb.CreateGlobalStringPtr(program_name_, "prog_name");
        cb.CreateCall(register_program_fn_, {nameStr, ei.fn});
        cb.CreateRetVoid();
        appendToGlobalCtors(*mod_, ctor, 65535);
        return ei;
    }

    /* The value this program's entry function returns, for the natural
     * (non-RETRN) end of its body: for a top-level program, the existing
     * process-exit-code convention (RPGRET or the LR bit) is unchanged; for
     * a library program (only reachable via CALL), it's the status
     * rpg_rt_call decodes into the caller's resulting indicators: 2 if any
     * halt indicator (H1-H9) is on, else 1 if LR is on, else 0. */
    llvm::Value *emit_entry_return_value() {
        using namespace llvm;
        if (is_top_level_) return emit_exit_value();
        auto &c = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        Value *halt = ConstantInt::getFalse(c);
        for (int i = 0; i < 9; ++i)
            halt = builder_.CreateOr(halt, inds_.load_resolved(-29 - i), "halt_any");
        Value *lr = inds_.load_resolved(-1);
        Value *st = builder_.CreateSelect(lr, ConstantInt::get(i32, 1),
                                          ConstantInt::get(i32, 0), "st_lr");
        st = builder_.CreateSelect(halt, ConstantInt::get(i32, 2), st, "st");
        return st;
    }

    /* L4: for a library program with an `*ENTRY PLIST`, copy each incoming
     * parameter's current value from the caller's address (parm_ptrs[i])
     * into this program's own local field of the same name, gated so a
     * short parameter list (fewer args than declared) leaves the unfilled
     * trailing fields at their zero/blank default instead of reading past
     * the caller's parm_ptrs array. Numeric parameters are always 4 bytes
     * (every numeric field is an i32 in this compiler, regardless of
     * declared digits); character parameters use the PARM's own declared
     * length (cols 49-51), defaulting to 1.
     *
     * This is a documented simplification of "pass by address" (manual
     * 123591-123613): rather than making the parameter field a live alias
     * of the caller's storage for the whole call, it copies in here and
     * copies out in emit_entry_plist_copyout() right before every return.
     * Observationally identical to true aliasing for a synchronous,
     * non-reentrant call where each caller field is passed at most once per
     * call (the overwhelmingly common case) -- a documented simplification. */
    void emit_entry_plist_prologue(const EntryInfo &ei) {
        if (is_top_level_ || !param_lists_) return;
        const ParamList *entry_pl = find_entry_plist();
        if (!entry_pl) return;
        using namespace llvm;
        auto &c = mod_->getContext();
        auto *ptrTy = PointerType::get(c, 0);
        auto *i32 = Type::getInt32Ty(c);

        for (size_t i = 0; i < entry_pl->parms.size(); ++i) {
            const ParmDecl &pd = entry_pl->parms[i];
            if (pd.name.empty()) continue;

            Function *fn = ei.fn;
            BasicBlock *haveBB = BasicBlock::Create(c, "parm_have", fn);
            BasicBlock *afterBB = BasicBlock::Create(c, "parm_after", fn);
            Value *inRange = builder_.CreateICmpSLT(
                ConstantInt::get(i32, (int)i), ei.parm_count, "parm_in_range");
            builder_.CreateCondBr(inRange, haveBB, afterBB);

            builder_.SetInsertPoint(haveBB);
            Value *slot = builder_.CreateGEP(ptrTy, ei.parm_ptrs,
                {ConstantInt::get(i32, (int)i)}, "parm_slot");
            Value *argAddr = builder_.CreateLoad(ptrTy, slot, "parm_addr");
            if (pd.dec >= 0) {
                Value *dst = syms_.get_or_create_field(pd.name);
                syms_.set_numeric_attrs(pd.name, pd.dec);
                Value *v = builder_.CreateLoad(i32, argAddr, pd.name + "_in");
                builder_.CreateStore(v, dst);
            } else {
                int len = pd.len > 0 ? pd.len : 1;
                Value *dst = syms_.get_or_create_char_field(pd.name, len);
                builder_.CreateMemCpy(dst, Align(1), argAddr, Align(1), (uint64_t)len);
            }
            builder_.CreateBr(afterBB);

            builder_.SetInsertPoint(afterBB);
            // Extra shim: PARM's own factor1, if present, overwrites the
            // just-copied-in value (run unconditionally, matching the
            // manual's four-shim-points rule).
            if (!pd.factor1.empty()) copy_param_shim(pd.name, pd.factor1);
        }
    }

    /* L4: the callee-side half of the copy-in/copy-out shim, emitted right
     * before every return from a library program's entry function (the
     * natural end and every RETRN). Copies each *ENTRY PLIST parameter's
     * current local value back out through the caller's address, so the
     * caller's own field sees the mutation once CALL returns. */
    void emit_entry_plist_copyout() {
        if (is_top_level_ || !param_lists_) return;
        const ParamList *entry_pl = find_entry_plist();
        if (!entry_pl) return;
        using namespace llvm;
        auto &c = mod_->getContext();
        auto *ptrTy = PointerType::get(c, 0);
        auto *i32 = Type::getInt32Ty(c);
        // The entry function's own args aren't reachable here by name; look
        // them up off the current function.
        llvm::Function *fn = builder_.GetInsertBlock()->getParent();
        auto argit = fn->arg_begin();
        Value *parm_ptrs = &*argit++;
        Value *parm_count = &*argit++;

        for (size_t i = 0; i < entry_pl->parms.size(); ++i) {
            const ParmDecl &pd = entry_pl->parms[i];
            if (pd.name.empty() || !syms_.has_field(pd.name)) continue;
            if (!pd.factor2.empty()) copy_param_shim(pd.factor2, pd.name);

            BasicBlock *haveBB = BasicBlock::Create(c, "parmout_have", fn);
            BasicBlock *afterBB = BasicBlock::Create(c, "parmout_after", fn);
            Value *inRange = builder_.CreateICmpSLT(
                ConstantInt::get(i32, (int)i), parm_count, "parmout_in_range");
            builder_.CreateCondBr(inRange, haveBB, afterBB);

            builder_.SetInsertPoint(haveBB);
            Value *slot = builder_.CreateGEP(ptrTy, parm_ptrs,
                {ConstantInt::get(i32, (int)i)}, "parmout_slot");
            Value *argAddr = builder_.CreateLoad(ptrTy, slot, "parmout_addr");
            if (pd.dec >= 0) {
                Value *v = builder_.CreateLoad(i32, syms_.get_or_create_field(pd.name),
                                               pd.name + "_out");
                builder_.CreateStore(v, argAddr);
            } else {
                int len = pd.len > 0 ? pd.len : 1;
                Value *src = syms_.get_or_create_char_field(pd.name, len);
                builder_.CreateMemCpy(argAddr, Align(1), src, Align(1), (uint64_t)len);
            }
            builder_.CreateBr(afterBB);
            builder_.SetInsertPoint(afterBB);
        }
    }

    /* L2/L5: a library program's 1P (first-page) indicator already behaves
     * correctly across repeat CALLs with no extra work -- @rpg_1p is a
     * plain global that defaults true and is latched false by
     * clear_first_page() after the first heading pass, so it naturally
     * stays false on a second CALL in the same process. FREE needs it to
     * come back on for the *next* CALL, though, and rpg_rt_free() only
     * clears the runtime registry's own bookkeeping (it has no reach into
     * this program's LLVM globals) -- so the callee side has to notice
     * `first_call` (true on the first CALL, and again on the first CALL
     * after a FREE) and force 1P back on itself. Only ever forces it TRUE;
     * never overwrites an already-correct false. */
    void emit_entry_first_call_reset(const EntryInfo &ei) {
        if (is_top_level_ || !ei.first_call) return;
        using namespace llvm;
        auto &c = mod_->getContext();
        Value *isFirst = builder_.CreateICmpNE(
            ei.first_call, ConstantInt::get(Type::getInt32Ty(c), 0), "is_first_call");
        Value *cur = inds_.load_resolved(-11);
        inds_.store_resolved(-11, builder_.CreateOr(cur, isFirst, "p1_reset"));
    }

    const ParamList *find_entry_plist() const {
        if (!param_lists_) return nullptr;
        for (const auto &p : *param_lists_) if (p.is_entry) return &p;
        return nullptr;
    }

    /* Copy `src`'s current value into `dst` (PARM copy-in/copy-out shim,
     * used for factor1/factor2 and reused for CALL's caller-side shim). A
     * best-effort, length-clamped raw copy: real programs keep the shim
     * field's attributes matching the parameter's (manual 123561-123591),
     * and this compiler stores every numeric field as a plain i32 regardless
     * of declared digits, so no decimal rescale is attempted. */
    void copy_param_shim(const std::string &dst, const std::string &src) {
        using namespace llvm;
        if (dst.empty() || src.empty()) return;
        if (syms_.is_char_field(dst)) {
            Value *sp; int slen; Value *dp; int dlen;
            if (syms_.resolve_char_operand(src, sp, slen) &&
                syms_.resolve_char_operand(dst, dp, dlen)) {
                int n = std::min(slen, dlen);
                builder_.CreateMemCpy(dp, Align(1), sp, Align(1), (uint64_t)n);
            }
            return;
        }
        Value *v = syms_.resolve_operand(src);
        if (v) builder_.CreateStore(v, syms_.get_or_create_field(dst));
    }

    /* The raw address of `name`'s storage, auto-declaring it as a numeric
     * field (matching resolve_operand's existing auto-create convention) if
     * it isn't already declared. Used for CALL's by-address parameters. */
    llvm::Value *field_address(const std::string &name) {
        const FieldInfo *fi = syms_.info(name);
        if (fi) return fi->gv;
        return syms_.get_or_create_field(name);
    }

    /* One shared `void*[n]` global per named PLIST, reused (overwritten) on
     * every CALL through it -- RPG II is single-threaded and non-reentrant
     * within one program (self/ancestor CALLs are rejected at runtime), so
     * there is no concurrent use to race on. */
    llvm::GlobalVariable *get_or_create_parm_buf(const std::string &plist_name,
                                                 size_t n) {
        auto it = parm_bufs_.find(plist_name);
        if (it != parm_bufs_.end()) return it->second;
        using namespace llvm;
        auto &c = mod_->getContext();
        auto *ptrTy = PointerType::get(c, 0);
        auto *arrTy = ArrayType::get(ptrTy, (unsigned)(n > 0 ? n : 1));
        auto *gv = new GlobalVariable(*mod_, arrTy, /*isConstant=*/false,
            GlobalValue::InternalLinkage, ConstantAggregateZero::get(arrTy),
            "parm_buf_" + plist_name);
        parm_bufs_[plist_name] = gv;
        return gv;
    }

    /* Blank-trim/upper-case/NUL-terminate `fp` (`flen` raw bytes) at runtime
     * via rpg_rt_field_to_cstr into the shared name-scratch buffer, and
     * return that buffer's pointer. Shared by resolve_program_name_arg's
     * two dynamic forms (a plain character field, or a character array/
     * table element). */
    llvm::Value *emit_field_to_name_cstr(llvm::Value *fp, int flen) {
        using namespace llvm;
        auto &cctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(cctx);
        auto *arrTy = ArrayType::get(Type::getInt8Ty(cctx), kNameScratchLen);
        if (!name_scratch_) {
            name_scratch_ = new GlobalVariable(*mod_, arrTy, /*isConstant=*/false,
                GlobalValue::InternalLinkage, ConstantAggregateZero::get(arrTy),
                "call_name_scratch");
        }
        Value *bufPtr = builder_.CreateConstInBoundsGEP2_32(
            arrTy, name_scratch_, 0, 0, "name_buf");
        builder_.CreateCall(field_to_cstr_fn_,
            {fp, ConstantInt::get(i32, flen), bufPtr,
             ConstantInt::get(i32, (int)kNameScratchLen)});
        return bufPtr;
    }

    /* CALL/FREE's target-name token (factor 2) is one of three forms,
     * resolved in this order:
     *   1. a declared character field holding the name (e.g. `CALL PGMNM`)
     *   2. a character array/table element holding the name (e.g.
     *      `CALL PGMARR,2`)
     *   3. a literal program name -- optionally `LIB/PGM` form, library
     *      segment ignored, same precedent as WRKSTN_PLAN.md §2's
     *      LIBRARY,MEMBER handling
     * The two dynamic forms are resolved at runtime: their current bytes
     * are blank-trimmed, upper-cased, and NUL-terminated into a shared
     * scratch buffer via rpg_rt_field_to_cstr. All three are matched
     * case-insensitively against the registry, since create_entry_
     * function() registers every program under its upper-cased H-spec
     * program-id. */
    llvm::Value *resolve_program_name_arg(const std::string &token) {
        using namespace llvm;

        if (syms_.has_field(token) && syms_.is_char_field(token)) {
            Value *fp; int flen;
            if (syms_.resolve_char_operand(token, fp, flen))
                return emit_field_to_name_cstr(fp, flen);
        }
        Value *fp; int flen;
        if (syms_.resolve_char_array_element(token, fp, flen))
            return emit_field_to_name_cstr(fp, flen);

        std::string raw = token;
        auto slash = raw.find_last_of('/');
        std::string pname = upper_trim(slash == std::string::npos
                                           ? raw : raw.substr(slash + 1));
        return builder_.CreateGlobalStringPtr(pname, "callee_name");
    }

    /* CALL (L2): the result field, if present, names a declared
     * (non-*ENTRY) PLIST supplying the parameters. Resulting indicators:
     * 56-57 (c.lo) = error, 58-59 (c.eq) = the callee's LR indicator. */
    void emit_call(const CSpec &c) {
        using namespace llvm;
        auto &cctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(cctx);
        auto *ptrTy = PointerType::get(cctx, 0);

        if (c.factor2.empty()) return;  // reported in cspec.cpp
        Value *nameStr = resolve_program_name_arg(c.factor2);

        const ParamList *pl = nullptr;
        if (!c.result.empty()) {
            std::string want = upper_trim(c.result);
            if (param_lists_) {
                for (const auto &p : *param_lists_) {
                    if (!p.is_entry && upper_trim(p.name) == want) { pl = &p; break; }
                }
            }
            if (!pl) {
                report("input", c.lineno, 43, DiagKind::Error,
                       "CALL result field '" + c.result +
                       "' does not name a declared PLIST");
            }
        }

        Value *parmPtrsArg = ConstantPointerNull::get(cast<PointerType>(ptrTy));
        Value *countArg = ConstantInt::get(i32, 0);
        if (pl && !pl->parms.empty()) {
            // Caller-side copy-in shim (step 1).
            for (const auto &pd : pl->parms)
                if (!pd.factor1.empty()) copy_param_shim(pd.name, pd.factor1);

            size_t n = pl->parms.size();
            GlobalVariable *buf = get_or_create_parm_buf(pl->name, n);
            auto *arrTy = ArrayType::get(ptrTy, (unsigned)n);
            for (size_t i = 0; i < n; ++i) {
                Value *addr = field_address(pl->parms[i].name);
                Value *slot = builder_.CreateConstInBoundsGEP2_32(
                    arrTy, buf, 0, (unsigned)i);
                builder_.CreateStore(addr, slot);
            }
            parmPtrsArg = builder_.CreateConstInBoundsGEP2_32(arrTy, buf, 0, 0);
            countArg = ConstantInt::get(i32, (uint32_t)n);
        }

        if (!call_err_slot_) {
            call_err_slot_ = new GlobalVariable(*mod_, i32, /*isConstant=*/false,
                GlobalValue::InternalLinkage, ConstantInt::get(i32, 0), "call_err");
            call_lr_slot_ = new GlobalVariable(*mod_, i32, /*isConstant=*/false,
                GlobalValue::InternalLinkage, ConstantInt::get(i32, 0), "call_lr");
        }
        builder_.CreateCall(call_fn_,
            {nameStr, parmPtrsArg, countArg, call_err_slot_, call_lr_slot_});

        // Caller-side copy-out shim (step 4), after the callee has returned.
        if (pl) {
            for (const auto &pd : pl->parms)
                if (!pd.factor2.empty()) copy_param_shim(pd.factor2, pd.name);
        }

        Value *errv = builder_.CreateICmpNE(
            builder_.CreateLoad(i32, call_err_slot_, "call_err_v"),
            ConstantInt::get(i32, 0), "call_err_b");
        Value *lrv = builder_.CreateICmpNE(
            builder_.CreateLoad(i32, call_lr_slot_, "call_lr_v"),
            ConstantInt::get(i32, 0), "call_lr_b");
        if (c.lo.indicator) inds_.store_resolved(c.lo.indicator, errv);
        if (c.eq.indicator) inds_.store_resolved(c.eq.indicator, lrv);
    }

    /* FREE (L5): clear a called program's "initialized" registry flag so its
     * next CALL re-runs one-time init. Optional 56-57 resulting indicator
     * (c.lo) = "not successful" (name never registered, or never CALLed). */
    void emit_free(const CSpec &c) {
        using namespace llvm;
        if (c.factor2.empty()) return;  // reported in cspec.cpp
        Value *nameStr = resolve_program_name_arg(c.factor2);
        auto &cctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(cctx);
        Value *status = builder_.CreateCall(free_fn_, {nameStr}, "free_st");
        if (c.lo.indicator) {
            Value *notOk = builder_.CreateICmpNE(status, ConstantInt::get(i32, 0),
                                                 "free_notok");
            inds_.store_resolved(c.lo.indicator, notOk);
        }
    }

    /* EXIT/RLABL (L3): call an external, non-RPG subroutine (see
     * runtime/rpg_ext_subr.h for the C ABI it's linked against). Both the
     * parameter-address array and the attribute-descriptor array are fully
     * compile-time-known (every field's address is a global, and its
     * type/length/decimals/count are known from the symbol table and the
     * RLABL declaration), so both are emitted as constant globals rather
     * than built at runtime -- emit_exit_op itself only emits the one call. */
    void emit_exit_op(const CSpec &c) {
        using namespace llvm;
        if (!exit_decls_ || c.factor2.empty()) return;  // reported in cspec.cpp
        std::string want = upper_trim(c.factor2);
        const ExitDecl *ed = nullptr;
        for (const auto &e : *exit_decls_) if (e.subr_name == want) { ed = &e; break; }
        if (!ed) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "EXIT '" + c.factor2 + "' has no following RLABL declarations");
            return;
        }

        auto &cctx = mod_->getContext();
        auto *i8  = Type::getInt8Ty(cctx);
        auto *i32 = Type::getInt32Ty(cctx);
        auto *ptrTy = PointerType::get(cctx, 0);
        auto *attrTy = StructType::get(cctx, {i8, i32, i32, i32});

        size_t n = ed->labels.size();
        std::vector<Constant *> parmInits, attrInits;
        parmInits.reserve(n > 0 ? n : 1);
        attrInits.reserve(n > 0 ? n : 1);
        for (const auto &rd : ed->labels) {
            const FieldInfo *fi = syms_.info(rd.name);
            char type = 'Z';
            int length = rd.len;
            int decimals = rd.dec >= 0 ? rd.dec : 0;
            int count = 1;
            Constant *addr;
            if (fi && fi->kind == FieldKind::Array) {
                type = fi->is_char_array ? 'C' : 'Z';
                count = fi->array_count;
                if (length <= 0) length = fi->is_char_array ? fi->length : 0;
                addr = fi->gv;
            } else if (fi && fi->kind == FieldKind::Character) {
                type = 'C';
                if (length <= 0) length = fi->length;
                addr = fi->gv;
            } else {
                type = 'Z';
                if (rd.dec < 0 && fi) decimals = fi->decimals < 0 ? 0 : fi->decimals;
                addr = cast<GlobalVariable>(syms_.get_or_create_field(rd.name));
            }
            parmInits.push_back(addr);
            attrInits.push_back(ConstantStruct::get(attrTy,
                {ConstantInt::get(i8, (uint8_t)type),
                 ConstantInt::get(i32, (uint32_t)length, true),
                 ConstantInt::get(i32, (uint32_t)decimals, true),
                 ConstantInt::get(i32, (uint32_t)count, true)}));
        }
        if (parmInits.empty()) {
            parmInits.push_back(ConstantPointerNull::get(cast<PointerType>(ptrTy)));
            attrInits.push_back(ConstantStruct::get(attrTy,
                {ConstantInt::get(i8, 0), ConstantInt::get(i32, 0),
                 ConstantInt::get(i32, 0), ConstantInt::get(i32, 0)}));
        }
        auto *parmArrTy = ArrayType::get(ptrTy, (unsigned)parmInits.size());
        auto *attrArrTy = ArrayType::get(attrTy, (unsigned)attrInits.size());
        auto *parmGV = new GlobalVariable(*mod_, parmArrTy, /*isConstant=*/true,
            GlobalValue::PrivateLinkage, ConstantArray::get(parmArrTy, parmInits),
            "exit_parms_" + ed->subr_name);
        auto *attrGV = new GlobalVariable(*mod_, attrArrTy, /*isConstant=*/true,
            GlobalValue::PrivateLinkage, ConstantArray::get(attrArrTy, attrInits),
            "exit_attrs_" + ed->subr_name);

        Function *subrFn = mod_->getFunction(ed->subr_name);
        if (!subrFn) {
            subrFn = Function::Create(
                FunctionType::get(i32, {ptrTy, ptrTy, i32}, false),
                Function::ExternalLinkage, ed->subr_name, mod_.get());
        }
        Value *parmsArg = builder_.CreateConstInBoundsGEP2_32(parmArrTy, parmGV, 0, 0);
        Value *attrsArg = builder_.CreateConstInBoundsGEP2_32(attrArrTy, attrGV, 0, 0);
        builder_.CreateCall(subrFn,
            {parmsArg, attrsArg, ConstantInt::get(i32, (uint32_t)n, true)});
    }

    /* Emit prerun-time array/table loads at the current insert point. Each
     * PreRunTime numeric array opens its `from_file` and reads `entries`
     * fixed-width fields into its global; an alternating partner (cols 46-51)
     * is loaded from the same records. Section B. */
    void emit_prerun_loads() {
        using namespace llvm;
        if (!arrays_) return;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *ptr = PointerType::get(c, 0);
        for (const auto &a : *arrays_) {
            if (a.load != ArrayLoad::PreRunTime) continue;
            const FieldInfo *fi = syms_.info(a.name);
            if (!fi) continue;
            Value *path = builder_.CreateGlobalStringPtr(a.from_file, "afn");
            if (a.decimals < 0) {
                // Alphameric (A9): raw fixed-width byte loader, no decimal
                // parse. [count x [entry_len x i8]] storage is already flat
                // contiguous bytes, so a plain i8* into element 0 works as
                // the char loader's output pointer.
                int lenA = a.entry_len > 0 ? a.entry_len : 1;
                auto *elemTyA = ArrayType::get(Type::getInt8Ty(c), (unsigned)lenA);
                auto *arrTyA = ArrayType::get(elemTyA, (unsigned)fi->array_count);
                Value *outA = builder_.CreateConstInBoundsGEP2_32(
                    arrTyA, cast<GlobalVariable>(fi->gv), 0, 0, a.name+"_lp");
                Value *outB = ConstantPointerNull::get(static_cast<PointerType*>(ptr));
                int lenB = 0;
                if (!a.alt_name.empty() && a.alt_decimals < 0) {
                    const FieldInfo *fb = syms_.info(a.alt_name);
                    if (fb) {
                        lenB = a.alt_entry_len > 0 ? a.alt_entry_len : lenA;
                        auto *elemTyB = ArrayType::get(Type::getInt8Ty(c), (unsigned)lenB);
                        auto *arrTyB = ArrayType::get(elemTyB, (unsigned)fb->array_count);
                        outB = builder_.CreateConstInBoundsGEP2_32(
                            arrTyB, cast<GlobalVariable>(fb->gv), 0, 0, a.alt_name+"_lp");
                    }
                }
                builder_.CreateCall(load_char_arrays_,
                    {path, ConstantInt::get(i32, lenA, true),
                     ConstantInt::get(i32, lenB, true),
                     ConstantInt::get(i32, a.entries, true), outA, outB},
                    a.name+"_ld");
                continue;
            }
            auto *arrTy = ArrayType::get(i32, (unsigned)fi->array_count);
            Value *outA = builder_.CreateConstInBoundsGEP2_32(
                arrTy, cast<GlobalVariable>(fi->gv), 0, 0, a.name+"_lp");
            Value *outB = ConstantPointerNull::get(static_cast<PointerType*>(ptr));
            int lenB = 0;
            if (!a.alt_name.empty() && a.alt_decimals >= 0) {
                const FieldInfo *fb = syms_.info(a.alt_name);
                if (fb) {
                    auto *bTy = ArrayType::get(i32, (unsigned)fb->array_count);
                    outB = builder_.CreateConstInBoundsGEP2_32(
                        bTy, cast<GlobalVariable>(fb->gv), 0, 0, a.alt_name+"_lp");
                    lenB = a.alt_entry_len > 0 ? a.alt_entry_len : a.entry_len;
                }
            }
            int lenA = a.entry_len > 0 ? a.entry_len : 1;
            // E7: E-spec col 43/55 packed/binary format (0=zoned default).
            auto fmt_code = [](char f) -> int {
                return f == 'P' ? 1 : f == 'B' ? 2 : 0;
            };
            builder_.CreateCall(load_arrays_,
                {path, ConstantInt::get(i32, lenA, true),
                 ConstantInt::get(i32, lenB, true),
                 ConstantInt::get(i32, a.entries, true), outA, outB,
                 ConstantInt::get(i32, fmt_code(a.data_format), true),
                 ConstantInt::get(i32, fmt_code(a.alt_data_format), true)},
                a.name+"_ld");
        }
    }

    /* Linear form: C-specs run once, in order (Phase 2 behaviour). */
    bool generate_linear(const Program &prog) {
        using namespace llvm;
        const std::vector<CSpec> &specs = prog.calcs;
        const std::vector<ORecord> &outputs = prog.outputs;

        // Build main (or, for a non-top-level library program, its
        // rpg_entry_fn -- see create_entry_function()).
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        EntryInfo ei = create_entry_function();
        Function *main = ei.fn;
        BasicBlock *entry = BasicBlock::Create(mod_->getContext(), "entry", main);
        builder_.SetInsertPoint(entry);
        emit_entry_first_call_reset(ei);
        emit_entry_plist_prologue(ei);
        entry = builder_.GetInsertBlock();  // prologue may have added blocks

        // Load any prerun-time arrays/tables before the program runs.
        emit_prerun_loads();

        // Open keyed/random-access input files first (Sections G/G25), then
        // register any update files as their own write targets so the O-spec
        // open doesn't open a second, separate stream for the same file.
        // Non-static: each compiled program (possibly several per process in
        // a multi-file CALL build) needs its own map --
        // a function-local `static` here would leak stale, cross-module
        // Value pointers into every program compiled after the first.
        std::unordered_map<std::string, llvm::Value *> empty_ids;
        out_ids_ = &empty_ids;
        open_input_files(prog);
        for (const auto &f : prog.files) {
            if (f.type == FileType::Update) {
                auto it = in_ids_.find(f.name);
                if (it != in_ids_.end()) empty_ids[f.name] = it->second;
            }
        }
        open_output_files(prog, empty_ids);
        BasicBlock *afterHead = emit_heading_pass(main, entry, outputs);

        // Route through emit_spec_chain so structured ops (IF/ELSE/END/DOW/DOU)
        // and GOTO/TAG work in linear programs too. Level "" picks up all specs
        // whose control_level is blank.
        BasicBlock *prev = emit_spec_chain(main, afterHead, specs, "");
        // Emit detail output (e.g. ADD/UPDATE/DEL disk records, Section G/G25).
        prev = emit_output(main, prev, outputs, OType::Detail);

        BasicBlock *exitbb = BasicBlock::Create(mod_->getContext(), "exit", main);
        builder_.SetInsertPoint(prev);
        builder_.CreateBr(exitbb);
        builder_.SetInsertPoint(exitbb);
        builder_.CreateCall(close_all_);

        Value *ret = specs.empty() ? (Value*)ConstantInt::get(i32, 0)
                                   : emit_entry_return_value();
        emit_entry_plist_copyout();
        builder_.CreateRet(ret);
        return true;
    }

private:
    /* Declare the runtime symbols we may call. */
    void declare_runtime() {
        using namespace llvm;
        auto &c = mod_->getContext();
        auto *voidTy = Type::getVoidTy(c);
        auto *i32    = Type::getInt32Ty(c);
        auto *i64    = Type::getInt64Ty(c);
        auto *ptr    = PointerType::get(c, 0);

        close_all_ = Function::Create(FunctionType::get(voidTy, false),
            Function::ExternalLinkage, "rpg_rt_close_all", mod_.get());

        open_input_ = Function::Create(
            FunctionType::get(i32, {ptr}, false),
            Function::ExternalLinkage, "rpg_rt_open_input", mod_.get());

        set_reclen_ = Function::Create(
            FunctionType::get(voidTy, {i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_set_reclen", mod_.get());

        read_next_ = Function::Create(
            FunctionType::get(i32, {i32, ptr, i64}, false),
            Function::ExternalLinkage, "rpg_rt_read_next", mod_.get());

        // Section E (E19): look-ahead peek.
        peek_next_ = Function::Create(
            FunctionType::get(i32, {i32, ptr, i64}, false),
            Function::ExternalLinkage, "rpg_rt_peek_next", mod_.get());

        // Section G (G24): keyed / random file access.
        set_key_ = Function::Create(
            FunctionType::get(voidTy, {i32, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_set_key", mod_.get());
        // Section G (G25): update files (open r+, write/update/delete records).
        open_update_ = Function::Create(
            FunctionType::get(i32, {ptr}, false),
            Function::ExternalLinkage, "rpg_rt_open_update", mod_.get());
        write_rec_ = Function::Create(
            FunctionType::get(voidTy, {i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_write_rec", mod_.get());
        update_rec_ = Function::Create(
            FunctionType::get(voidTy, {i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_update_rec", mod_.get());
        delete_rec_ = Function::Create(
            FunctionType::get(voidTy, {i32}, false),
            Function::ExternalLinkage, "rpg_rt_delete_rec", mod_.get());
        flush_rec_ = Function::Create(
            FunctionType::get(voidTy, {i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_flush_rec", mod_.get());
        chain_ = Function::Create(
            FunctionType::get(i32, {i32, ptr, i32, ptr, i64}, false),
            Function::ExternalLinkage, "rpg_rt_chain", mod_.get());
        setll_ = Function::Create(
            FunctionType::get(i32, {i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_setll", mod_.get());
        read_op_ = Function::Create(
            FunctionType::get(i32, {i32, ptr, i64}, false),
            Function::ExternalLinkage, "rpg_rt_read", mod_.get());
        reade_ = Function::Create(
            FunctionType::get(i32, {i32, ptr, i32, ptr, i64}, false),
            Function::ExternalLinkage, "rpg_rt_reade", mod_.get());
        readp_ = Function::Create(
            FunctionType::get(i32, {i32, ptr, i64}, false),
            Function::ExternalLinkage, "rpg_rt_readp", mod_.get());

        // Group C: TIME (self-contained runtime call, no file/array args).
        time_fn_ = Function::Create(
            FunctionType::get(i64, false),
            Function::ExternalLinkage, "rpg_rt_time", mod_.get());

        get_decimal_ = Function::Create(
            FunctionType::get(i64, {ptr, i32, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_get_decimal", mod_.get());

        // Section C: packed-decimal / binary input decoders.
        get_packed_ = Function::Create(
            FunctionType::get(i64, {ptr, i32, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_get_packed", mod_.get());
        get_binary_ = Function::Create(
            FunctionType::get(i64, {ptr, i32, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_get_binary", mod_.get());

        // Section C (C10): sign-overpunch MOVE helpers.
        overpunch_in_ = Function::Create(
            FunctionType::get(i64, {ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_overpunch_in", mod_.get());
        overpunch_out_ = Function::Create(
            FunctionType::get(i32, {i64, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_overpunch_out", mod_.get());

        // Section D: skip/page output helpers (D13/D14).
        skip_fn_ = Function::Create(
            FunctionType::get(voidTy, {i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_skip", mod_.get());

        // Section F (F22): printer overflow configuration / polling.
        set_overflow_ = Function::Create(
            FunctionType::get(voidTy, {i32, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_set_overflow", mod_.get());
        take_overflow_ = Function::Create(
            FunctionType::get(i32, {i32}, false),
            Function::ExternalLinkage, "rpg_rt_take_overflow", mod_.get());

        // Phase 7 output functions.
        open_output_ = Function::Create(
            FunctionType::get(i32, {ptr}, false),
            Function::ExternalLinkage, "rpg_rt_open_output", mod_.get());
        line_begin_ = Function::Create(
            FunctionType::get(voidTy, {i32}, false),
            Function::ExternalLinkage, "rpg_rt_line_begin", mod_.get());
        line_put_str_ = Function::Create(
            FunctionType::get(voidTy, {ptr, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_line_put_str", mod_.get());
        line_put_num_ = Function::Create(
            FunctionType::get(voidTy, {i64, i32}, false),
            Function::ExternalLinkage, "rpg_rt_line_put_num", mod_.get());
        // Section C: decimal-aware output variants.
        line_put_num_dec_ = Function::Create(
            FunctionType::get(voidTy, {i64, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_line_put_num_dec", mod_.get());
        emit_line_ = Function::Create(
            FunctionType::get(voidTy, {i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_emit_line", mod_.get());

        // Phase 10 edit-code formatter (decimals=0 legacy entry point).
        edit_fn_ = Function::Create(
            FunctionType::get(i32, {i64, i32, i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_edit", mod_.get());
        // Section C: decimal-aware edit-code formatter. Last i32 before the
        // buffer is the A13 floating fill character (0 = none).
        edit_dec_fn_ = Function::Create(
            FunctionType::get(i32, {i64, i32, i32, i32, i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_edit_dec", mod_.get());

        // Section B: prerun-time array/table loader. Trailing i32 pair is
        // E7's fmt_a/fmt_b (0=zoned, 1=packed, 2=binary; E-spec cols 43/55).
        load_arrays_ = Function::Create(
            FunctionType::get(i32, {ptr, i32, i32, i32, ptr, ptr, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_load_arrays", mod_.get());
        // A9: alphameric prerun-time array/table loader.
        load_char_arrays_ = Function::Create(
            FunctionType::get(i32, {ptr, i32, i32, i32, ptr, ptr}, false),
            Function::ExternalLinkage, "rpg_rt_load_char_arrays", mod_.get());

        // Program linkage.
        register_program_fn_ = Function::Create(
            FunctionType::get(voidTy, {ptr, ptr}, false),
            Function::ExternalLinkage, "rpg_rt_register_program", mod_.get());
        call_fn_ = Function::Create(
            FunctionType::get(i32, {ptr, ptr, i32, ptr, ptr}, false),
            Function::ExternalLinkage, "rpg_rt_call", mod_.get());
        free_fn_ = Function::Create(
            FunctionType::get(i32, {ptr}, false),
            Function::ExternalLinkage, "rpg_rt_free", mod_.get());
        field_to_cstr_fn_ = Function::Create(
            FunctionType::get(i32, {ptr, i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_field_to_cstr", mod_.get());
    }

    /* D2: pre-declare every ordinary I-spec field's storage up front (see the
     * call site in generate() for why). Idempotent with the extract loop's
     * own get_or_create_field/get_or_create_char_field calls. */
    void declare_all_input_fields(const Program &prog) {
        auto declare_one = [&](const ISpecField &fld) {
            if (fld.name.empty()) return;
            if (fld.decimals < 0) {
                int len = fld.to - fld.from + 1;
                if (len < 1) len = 1;
                syms_.get_or_create_char_field(fld.name, len);
            } else {
                syms_.get_or_create_field(fld.name);
                syms_.set_numeric_attrs(fld.name, fld.decimals);
            }
        };
        for (const auto &fld : prog.in_fields)         declare_one(fld);
        for (const auto &fld : prog.lookahead_fields)   declare_one(fld);
    }

    /* D2: process I-spec data-structure statements and their subfields
     * (manual Ch. 15). A DS is a shared [len x i8] byte buffer; each
     * alphameric subfield is a byte-range GEP view into it (full read/write,
     * via SymbolTable::resolve_char_operand -- see symbols.cpp); each numeric
     * subfield decode-reads its bytes on every access and cannot be a calc
     * result field (see FieldInfo::is_ds_field in symbols.h for why).
     *
     * "The name of an input field ... being redefined in a data structure
     * must be the data structure name" (manual 61412-61416): if the DS's own
     * name (cols 7-12) matches an already-declared field, its storage is
     * REUSED (true physical aliasing) instead of allocating a fresh buffer --
     * this only works when that field is alphameric (Character kind), since
     * a numeric field in this compiler is a native i32 with no addressable
     * byte representation to alias into (manual's own "packed/binary numeric
     * fields convert to zoned when placed in a DS" rule has no analog here
     * either, for the same reason). */
    void declare_data_structures(const Program &prog) {
        using namespace llvm;
        for (size_t i = 0; i < prog.data_structures.size(); ++i) {
            const ISpecDS &ds = prog.data_structures[i];
            if (ds.is_lda) {
                rpgc::error("data structure '" + ds.name + "' (line " +
                            std::to_string(ds.lineno) + "): local data areas "
                            "(H-spec col 18 'U') are WORKSTN/display-station "
                            "specific and are not implemented");
                continue;
            }
            int max_to = 0;
            for (const auto &sf : prog.ds_subfields)
                if (sf.ds_index == (int)i) max_to = std::max(max_to, sf.to);
            if (max_to < 1) max_to = 1;

            GlobalVariable *ds_gv = nullptr;
            int ds_len = max_to;
            const FieldInfo *existing =
                ds.name.empty() ? nullptr : syms_.info(ds.name);
            if (existing) {
                if (existing->kind == FieldKind::Character) {
                    if (max_to > existing->length) {
                        rpgc::error("data structure '" + ds.name +
                                    "': subfields extend to position " +
                                    std::to_string(max_to) +
                                    ", beyond the redefined field's declared "
                                    "length (" + std::to_string(existing->length) + ")");
                    }
                    ds_gv = existing->gv;
                    ds_len = existing->length;
                } else {
                    rpgc::error("data structure '" + ds.name +
                                "' redefines a numeric field, which this "
                                "compiler cannot support (numeric fields "
                                "have no addressable byte storage)");
                    continue;
                }
            } else {
                std::string key = ds.name.empty()
                    ? ("$ds" + std::to_string(i)) : ds.name;
                ds_gv = cast<GlobalVariable>(
                    syms_.get_or_create_char_field(key, max_to));
                ds_len = max_to;
            }

            for (const auto &sf : prog.ds_subfields) {
                if (sf.ds_index != (int)i) continue;
                if (sf.name.empty()) continue;
                int len = sf.to - sf.from + 1;
                if (len < 1) continue;
                int offset = sf.from - 1;
                if (sf.decimals < 0)
                    syms_.declare_ds_char_subfield(sf.name, ds_gv, ds_len,
                                                   offset, len);
                else
                    syms_.declare_ds_numeric_subfield(sf.name, ds_gv, ds_len,
                                                      offset, len, sf.decimals);
            }
        }
    }

    /* Emit the implicit RPG program cycle. Phase 3 implements a simplified
     * cycle with one primary input file and no control levels:
     *
     *   entry:        open file, set reclen, init fields
     *   cycle.head:   read_next -> EOF?  lr.total  :  extract
     *   extract:      for each I-spec field, get_decimal -> store into global
     *                 -> detail.calcs
     *   detail.calcs: run C-specs whose control level is blank (detail) -> cycle.head
     *   lr.total:     set LR, run C-specs whose control level is LR -> exit
     *   exit:         close_all -> ret <exit value>
     *
     * (Control-level total calculations L1-L9/L0 arrive in a later phase.) */

    /* Decode one input field from the record buffer into its global. Field
     * indicators (E18) are NOT set here: manual steps 24-26 set MR and field
     * indicators only after total-time calc/output has run against the
     * *previous* record's state, so total-time conditioning on a field
     * indicator must not observe the record just extracted. Callers extract
     * at read time as before; emit_field_indicators (below) is called
     * separately once total time has completed, right before detail-time
     * calculations (B1). Extracted from the extract loop so the
     * field-record-relation guard (E17) can call it conditionally. */
    void emit_one_input_field(llvm::LLVMContext &c, llvm::Value *bufPtr,
                              const ISpecField &fld, int reclen) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(c);
        auto *i8  = Type::getInt8Ty(c);
        if (fld.decimals < 0) {
            // Character field: copy raw record bytes [from-1 .. to-1].
            int len = fld.to - fld.from + 1;
            if (len < 1) return;
            Value *dst = syms_.get_or_create_char_field(fld.name, len);
            auto *arrTy = ArrayType::get(i8, (unsigned)len);
            Value *dstp = builder_.CreateConstInBoundsGEP2_32(arrTy,
                cast<GlobalVariable>(dst), 0, 0, fld.name+"_dp");
            Value *srcp = builder_.CreateInBoundsGEP(i8, bufPtr,
                ConstantInt::get(i32, fld.from - 1, true), fld.name+"_sp");
            builder_.CreateMemCpy(dstp, MaybeAlign(), srcp, MaybeAlign(),
                                  (unsigned)len);
        } else {
            Function *dec = get_decimal_;
            if (fld.data_format == 'P')      dec = get_packed_;
            else if (fld.data_format == 'B') dec = get_binary_;
            Value *v = builder_.CreateCall(dec,
                {bufPtr, ConstantInt::get(i32, reclen, true),
                 ConstantInt::get(i32, fld.from, true),
                 ConstantInt::get(i32, fld.to, true)},
                fld.name + "_in");
            Value *v32 = builder_.CreateTrunc(v, i32, fld.name + "_i32");
            builder_.CreateStore(v32, syms_.get_or_create_field(fld.name));
        }
    }

    /* Set the field indicators (cols 65-70/69-70, E18) for one input field
     * from its already-extracted global storage. Called once total time has
     * completed (manual step 25), separately from extraction (B1) so
     * total-time conditioning on a field indicator sees the previous
     * record's state rather than the one just read. */
    void emit_field_indicators(const ISpecField &fld) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i8  = Type::getInt8Ty(c);
        if (fld.decimals < 0) {
            // Field indicator: on when an alphameric field is all blanks.
            if (!fld.zero_ind) return;
            int len = fld.to - fld.from + 1;
            if (len < 1) return;
            Value *ptr; int plen;
            if (!syms_.resolve_char_operand(fld.name, ptr, plen)) return;
            Value *allblank = ConstantInt::getTrue(c);
            for (int b = 0; b < len; ++b) {
                Value *bp = builder_.CreateInBoundsGEP(i8, ptr,
                    ConstantInt::get(i32, b, true));
                Value *ch = builder_.CreateLoad(i8, bp, fld.name+"_icb");
                Value *isSp = builder_.CreateICmpEQ(ch,
                    ConstantInt::get(i8, (uint8_t)' '), fld.name+"_isp");
                allblank = builder_.CreateAnd(allblank, isSp, fld.name+"_iab");
            }
            inds_.store_resolved(fld.zero_ind, allblank);
        } else {
            if (!fld.plus_ind && !fld.minus_ind && !fld.zero_ind) return;
            Value *v32 = syms_.load_field(fld.name);
            auto *zero = ConstantInt::get(i32, 0, true);
            if (fld.plus_ind)
                inds_.store_resolved(fld.plus_ind,
                    builder_.CreateICmpSGT(v32, zero, fld.name+"_ip"));
            if (fld.minus_ind)
                inds_.store_resolved(fld.minus_ind,
                    builder_.CreateICmpSLT(v32, zero, fld.name+"_im"));
            if (fld.zero_ind)
                inds_.store_resolved(fld.zero_ind,
                    builder_.CreateICmpEQ(v32, zero, fld.name+"_iz"));
        }
    }

    /* Run emit_field_indicators over every field in `fields` that carries a
     * field-indicator column, honouring the field-record-relation guard
     * (E17) exactly as extraction did -- the record-identifying indicator is
     * still set from this cycle's selection, so re-checking it here is safe. */
    void emit_field_indicators_for(const std::vector<ISpecField> &fields,
                                   const std::string *file_filter = nullptr) {
        for (const auto &fld : fields) {
            if (fld.name.empty()) continue;
            if (!fld.plus_ind && !fld.minus_ind && !fld.zero_ind) continue;
            if (file_filter && fld.file != *file_filter) continue;
            if (fld.record_id > 0) {
                llvm::Value *on = inds_.load_resolved(fld.record_id);
                llvm::Function *main = builder_.GetInsertBlock()->getParent();
                auto &c = mod_->getContext();
                llvm::BasicBlock *fdo = llvm::BasicBlock::Create(c, "find_do", main);
                llvm::BasicBlock *fafter = llvm::BasicBlock::Create(c, "find_after", main);
                builder_.CreateCondBr(on, fdo, fafter);
                builder_.SetInsertPoint(fdo);
                emit_field_indicators(fld);
                builder_.CreateBr(fafter);
                builder_.SetInsertPoint(fafter);
            } else {
                emit_field_indicators(fld);
            }
        }
    }

    /* Record-identification selection (E17). If any record type for the primary
     * file carries identification codes, match the current record (in bufPtr)
     * against each type's code-sets, set the matching record-identifying
     * indicator, and return a new block to continue field extraction from.
     * Records matching no type branch back to `head` (skipped). If no record
     * type has codes, returns `extract` unchanged (no-op). */
    /* B3: match fields (M1-M9) and control-level fields are compared "sign
     * blind" -- only the digit portion matters, so a -5 matches/equals a +5
     * (manual 52317-52319, 53885-53887). Decimal positions are already
     * ignored by construction (the decoded value is the raw digit magnitude,
     * unscaled), so only the sign needs stripping here. */
    llvm::Value *emit_abs_i32(llvm::Value *v) {
        using namespace llvm;
        auto &c = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        Value *isNeg = builder_.CreateICmpSLT(v, ConstantInt::get(i32, 0, true), "abs_neg");
        Value *negV  = builder_.CreateNeg(v, "abs_val");
        return builder_.CreateSelect(isNeg, negV, v, "abs");
    }
    /* Evaluate the AND of a single record type's identification code-sets
     * (E17) against bufPtr. Shared by emit_record_selection and the
     * per-record-type M1 dispatch (A8) so both agree on what "this record is
     * of type r" means. */
    llvm::Value *eval_record_code_match(const ISpecRec &r, llvm::Value *bufPtr) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i8  = Type::getInt8Ty(c);
        Value *match = ConstantInt::getTrue(c);
        for (const auto &cs : r.codes) {
            Value *bp = builder_.CreateInBoundsGEP(i8, bufPtr,
                ConstantInt::get(i32, cs.pos - 1, true), "rc_p");
            Value *b  = builder_.CreateLoad(i8, bp, "rc_b");
            Value *want = ConstantInt::get(i8, (uint8_t)cs.ch);
            Value *same;
            if (cs.czd == 'C') {
                same = builder_.CreateICmpEQ(b, want, "rc_eq");
            } else if (cs.czd == 'Z') {
                // high nibble (zone)
                Value *zn = builder_.CreateLShr(b,
                    ConstantInt::get(i8, 4), "rc_zn");
                Value *wz = builder_.CreateLShr(want,
                    ConstantInt::get(i8, 4), "rc_wz");
                same = builder_.CreateICmpEQ(zn, wz, "rc_zeq");
            } else { // 'D' low nibble (digit)
                Value *m = ConstantInt::get(i8, 0x0F);
                Value *dg = builder_.CreateAnd(b, m, "rc_dg");
                Value *wd = builder_.CreateAnd(want, m, "rc_wd");
                same = builder_.CreateICmpEQ(dg, wd, "rc_deq");
            }
            if (cs.negate) same = builder_.CreateNot(same, "rc_neg");
            match = builder_.CreateAnd(match, same, "rc_and");
        }
        return match;
    }

    llvm::BasicBlock *emit_record_selection(llvm::Function *main,
                                            llvm::BasicBlock *head,
                                            llvm::BasicBlock *extract,
                                            llvm::Value *bufPtr,
                                            const Program &prog) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        // Collect record types that carry codes.
        std::vector<const ISpecRec *> typed;
        for (const auto &r : prog.in_records)
            if (!r.codes.empty()) typed.push_back(&r);
        if (typed.empty()) return extract;   // single-record-type: no selection

        builder_.SetInsertPoint(extract);
        // First clear all record-identifying indicators that have codes, so
        // only the matching type is on this cycle.
        for (const auto *r : typed)
            if (r->rec_indicator > 0)
                inds_.store_resolved(r->rec_indicator,
                                     ConstantInt::getFalse(c));

        // For each typed record, evaluate AND of its code-sets and store the
        // result into its record-identifying indicator.
        Value *anyMatch = ConstantInt::getFalse(c);
        for (const auto *r : typed) {
            Value *match = eval_record_code_match(*r, bufPtr);
            if (r->rec_indicator > 0)
                inds_.store_resolved(r->rec_indicator, match);
            anyMatch = builder_.CreateOr(anyMatch, match, "rc_any");
        }
        // If no type matched, skip this record (loop back to head).
        BasicBlock *fieldEx = BasicBlock::Create(c, "rec_sel", main);
        builder_.CreateCondBr(anyMatch, fieldEx, head);
        builder_.SetInsertPoint(fieldEx);
        return fieldEx;
    }

    /* Open the output files referenced by O-specs into `out_ids`. For a PRINTER
     * file that has an overflow indicator (F-spec cols 33-34), configure the
     * runtime overflow line from the line-counter (L) spec, or the manual
     * defaults. Shared by both cycle paths. */
    void open_output_files(const Program &prog,
                           std::unordered_map<std::string, llvm::Value *> &out_ids) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        for (const auto &o : prog.outputs) {
            if (o.file.empty()) continue;
            if (out_ids.count(o.file)) continue;
            // Find the F-spec to confirm it's an output/printer file.
            const FSpec *ofs = nullptr;
            for (const auto &f : prog.files)
                if (f.name == o.file) { ofs = &f; break; }
            if (!ofs || ofs->type == FileType::Input) continue;
            Value *onm = builder_.CreateGlobalStringPtr(o.file, "oname");
            // Update files (type U) open r+ for in-place rewrite; output files
            // (type O) open w (truncate). (Section G, G25.)
            Value *fid = (ofs->type == FileType::Update)
                ? builder_.CreateCall(open_update_, {onm}, o.file + "_uid")
                : builder_.CreateCall(open_output_, {onm}, o.file + "_id");
            out_ids[o.file] = fid;
            // Overflow (F22): if the F-spec assigns an overflow indicator, tell
            // the runtime the page depth and overflow line for this file.
            if (ofs->has_overflow) {
                int lpp = 66, oline = 60;
                auto it = prog.line_counters.find(o.file);
                if (it != prog.line_counters.end()) {
                    lpp = it->second.lines_per_page;
                    oline = it->second.overflow_line;
                }
                builder_.CreateCall(set_overflow_, {
                    fid,
                    ConstantInt::get(i32, lpp, true),
                    ConstantInt::get(i32, oline, true)});
            }
        }
    }

    /* E1: F-spec cols 71-72 external file conditioning (U1-U8). Returns an i1
     * that is true when `fname` is either unconditioned or its conditioning
     * indicator is on. Used to gate every read/write against that file. */
    llvm::Value *file_cond_ok(const std::string &fname) {
        using namespace llvm;
        if (files_)
            for (const auto &f : *files_)
                if (f.name == fname)
                    return f.has_cond ? inds_.load_resolved(f.cond_ind)
                                      : ConstantInt::getTrue(mod_->getContext());
        return ConstantInt::getTrue(mod_->getContext());
    }

    /* Gate a boolean-producing call (`do_call`, invoked with the builder
     * positioned to emit it) behind `fname`'s U1-U8 conditioning indicator
     * (E1): when the indicator is off, `do_call` is skipped entirely -- not
     * just its result discarded -- and the gated value is false, matching the
     * manual's "treated as though the end of the file is reached" (78727-
     * 78739). When `fname` carries no conditioning, this reduces to just
     * running `do_call` with no extra branching. */
    llvm::Value *emit_conditioned_bool(llvm::Function *fn,
                                       const std::string &fname,
                                       const std::function<llvm::Value*()> &do_call) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        const FSpec *fs = nullptr;
        if (files_) for (const auto &f : *files_) if (f.name == fname) { fs = &f; break; }
        if (!fs || !fs->has_cond) return do_call();

        Value *condOk = inds_.load_resolved(fs->cond_ind);
        BasicBlock *doB    = BasicBlock::Create(ctx, "fcnd_do", fn);
        BasicBlock *skipB  = BasicBlock::Create(ctx, "fcnd_skip", fn);
        BasicBlock *afterB = BasicBlock::Create(ctx, "fcnd_after", fn);
        builder_.CreateCondBr(condOk, doB, skipB);
        builder_.SetInsertPoint(doB);
        Value *r = do_call();
        BasicBlock *doEnd = builder_.GetInsertBlock();
        builder_.CreateBr(afterB);
        builder_.SetInsertPoint(skipB);
        builder_.CreateBr(afterB);
        builder_.SetInsertPoint(afterB);
        PHINode *phi = builder_.CreatePHI(Type::getInt1Ty(ctx), 2, "fcnd_res");
        phi->addIncoming(r, doEnd);
        phi->addIncoming(ConstantInt::getFalse(ctx), skipB);
        return phi;
    }

    /* Open the input/update files referenced by CHAIN/SETLL/READ/READE that are
     * not the cycle's own primary/secondary files (Section G, G24). Files with a
     * key declared on the F-spec (cols 29-30 length, 35-38 start) get a set_key
     * call so the runtime builds a key index. Called from each cycle path. */
    void open_input_files(const Program &prog) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        // Cycle-fed files (primary + secondaries) are already opened by the
        // cycle; skip them so CHAIN targets an independently-opened stream.
        const FSpec *pf = find_primary_input(prog.files);
        auto secs = secondary_inputs(prog.files);
        auto is_cycle_file = [&](const std::string &name) {
            if (pf && pf->name == name) return true;
            for (const auto *s : secs) if (s->name == name) return true;
            return false;
        };
        for (const auto &f : prog.files) {
            if (f.name.empty()) continue;
            if (in_ids_.count(f.name)) continue;
            if (is_cycle_file(f.name)) continue;
            // Only DISK input/update files make sense for keyed/random access.
            if (f.device != Device::Disk) continue;
            if (f.type != FileType::Input && f.type != FileType::Update) continue;
            Value *nm = builder_.CreateGlobalStringPtr(f.name, "iname");
            // Update files open r+ so a later UPDATE can rewrite the record
            // CHAIN read (the same file id serves both reads and writes, G25).
            Value *fid = (f.type == FileType::Update)
                ? builder_.CreateCall(open_update_, {nm}, f.name + "_uid")
                : builder_.CreateCall(open_input_, {nm}, f.name + "_cid");
            int rl = f.reclen > 0 ? f.reclen : 80;
            builder_.CreateCall(set_reclen_,
                {fid, ConstantInt::get(i32, rl, true)});
            // E6: col 31 'I' means factor1 is a relative record number, not
            // a byte-compared key (manual 77334-77365), even when cols29-30
            // declare a width (the RRN digit count). Skipping set_key leaves
            // the runtime's key_len at 0, which rpg_rt_chain/setll/reade
            // already treat as "RRN access" (see rpg_runtime.c); calling
            // set_key here would instead binary-search the RRN digits as an
            // ordinary byte key, silently misreading every random access.
            if (f.key_len > 0 && f.key_start > 0 && f.addr_type != 'I') {
                builder_.CreateCall(set_key_, {
                    fid,
                    ConstantInt::get(i32, f.key_start, true),
                    ConstantInt::get(i32, f.key_len, true)});
            }
            in_ids_[f.name] = fid;
        }
    }

    bool generate_cycle(const Program &prog) {
        using namespace llvm;
        auto &c  = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i64 = Type::getInt64Ty(c);

        const FSpec *pf = find_primary_input(prog.files);
        if (!pf) return generate_linear(prog); // defensive

        int reclen = pf->reclen > 0 ? pf->reclen : 80;

        // Function + entry block.
        EntryInfo ei = create_entry_function();
        Function *main = ei.fn;
        BasicBlock *entry = BasicBlock::Create(c, "entry", main);
        builder_.SetInsertPoint(entry);
        emit_entry_first_call_reset(ei);
        emit_entry_plist_prologue(ei);
        entry = builder_.GetInsertBlock();  // prologue may have added blocks

        // Load any prerun-time arrays/tables before the cycle starts.
        emit_prerun_loads();

        // Record buffer global: [reclen+1 x i8], zeroed.
        auto *bufTy = ArrayType::get(Type::getInt8Ty(c), (unsigned)reclen + 1);
        rec_buf_ = new GlobalVariable(
            *mod_, bufTy, /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantAggregateZero::get(bufTy), "rpg_rec");

        // Open the primary file. The file name comes from the F-spec; the path
        // is taken to be the literal filename (a Linux file in the cwd). An
        // update file (type U) opens r+ so records can be rewritten (G25).
        Value *name = builder_.CreateGlobalStringPtr(pf->name, "fname");
        Value *fid  = (pf->type == FileType::Update)
            ? builder_.CreateCall(open_update_, {name}, "file_id")
            : builder_.CreateCall(open_input_, {name}, "file_id");
        // set_reclen(fid, reclen)
        builder_.CreateCall(set_reclen_,
            {fid, ConstantInt::get(i32, reclen, true)});
        // For an update primary, also register it in in_ids_ so CHAIN/READ can
        // target it, and configure its key if declared.
        if (pf->type == FileType::Update) {
            in_ids_[pf->name] = fid;
            // E6: skip set_key for an 'I' (RRN) file -- see open_input_files.
            if (pf->key_len > 0 && pf->key_start > 0 && pf->addr_type != 'I') {
                builder_.CreateCall(set_key_, {
                    fid,
                    ConstantInt::get(i32, pf->key_start, true),
                    ConstantInt::get(i32, pf->key_len, true)});
            }
        }

        // Open output files (those referenced by O-specs) and stash their file
        // ids. PRINTER files with an overflow indicator get configured here too.
        std::unordered_map<std::string, llvm::Value *> out_ids;
        // An update primary is also the write target for its UPDATE records, so
        // pre-register it so open_output_files doesn't open a second stream (G25).
        if (pf->type == FileType::Update) out_ids[pf->name] = fid;
        open_output_files(prog, out_ids);
        out_ids_ = &out_ids;
        // Open keyed/random-access input files (CHAIN/SETLL/READ/READE targets).
        open_input_files(prog);

        // First-page pass: emit headings (and 1P-conditioned detail) once at
        // program start, before the cycle begins (D12).
        BasicBlock *afterHead = emit_heading_pass(main, entry, prog.outputs);

        // Pre-create globals for every input field so the extract block and
        // the C-spec operand resolution share the same storage. Character
        // fields (blank decimal position) get [N x i8] storage and are loaded
        // from the raw record bytes; numeric fields stay i32.
        for (const auto &fld : prog.in_fields) {
            if (fld.name.empty()) continue;
            if (fld.decimals < 0) {
                // Alphanumeric: length = to - from + 1.
                int len = fld.to - fld.from + 1;
                if (len < 1) len = 1;
                syms_.get_or_create_char_field(fld.name, len);
            } else {
                syms_.get_or_create_field(fld.name);
                // Record the field's decimal-position count so arithmetic
                // (Section C) treats the stored i32 as a scaled integer.
                syms_.set_numeric_attrs(fld.name, fld.decimals);
            }
        }
        // Pre-create globals for look-ahead fields too (E19).
        for (const auto &fld : prog.lookahead_fields) {
            if (fld.name.empty()) continue;
            if (fld.decimals < 0) {
                int len = fld.to - fld.from + 1;
                if (len < 1) len = 1;
                syms_.get_or_create_char_field(fld.name, len);
            } else {
                syms_.get_or_create_field(fld.name);
                syms_.set_numeric_attrs(fld.name, fld.decimals);
            }
        }

        // Collect control fields (I-fields tagged with a control level L1-L9),
        // and create a "previous value" global for each so the cycle can detect
        // a break by comparing current vs previous.
        struct CtlField { const ISpecField *spec; llvm::GlobalVariable *prev; int level; };
        std::vector<CtlField> ctl_fields;
        for (const auto &fld : prog.in_fields) {
            if (fld.control_level.size() == 2 && fld.control_level[0]=='L'
                && fld.control_level[1]>='1' && fld.control_level[1]<='9') {
                auto *gv = new GlobalVariable(
                    *mod_, Type::getInt32Ty(c), /*isConstant=*/false,
                    GlobalValue::InternalLinkage,
                    ConstantInt::get(Type::getInt32Ty(c), -1),
                    "prev_" + fld.name);
                ctl_fields.push_back({&fld, gv, fld.control_level[1]-'0'});
            }
        }

        // A "first record seen" flag so we skip total time on cycle 1.
        auto *i32ty = Type::getInt32Ty(c);
        auto *firstflag = new GlobalVariable(
            *mod_, Type::getInt1Ty(c), /*isConstant=*/false,
            GlobalValue::InternalLinkage, ConstantInt::getTrue(c), "rpg_first");

        // cycle.head
        BasicBlock *head = BasicBlock::Create(c, "cycle.head", main);
        builder_.CreateBr(head);
        builder_.SetInsertPoint(head);
        // Step 6: turn off all control-level indicators (L0 stays on).
        inds_.reset_control_levels();
        // read_next(fid, rec_buf, reclen+1). E1: gated behind the primary
        // file's U1-U8 conditioning indicator, if any -- off means treated
        // as EOF with no actual read performed.
        Value *bufPtr = builder_.CreateConstInBoundsGEP2_32(
            ArrayType::get(Type::getInt8Ty(c), reclen + 1), rec_buf_, 0, 0);
        Value *haveRec = emit_conditioned_bool(main, pf->name, [&]() -> Value* {
            Value *got = builder_.CreateCall(read_next_,
                {fid, bufPtr, ConstantInt::get(i64, reclen + 1, false)}, "got_rec");
            return builder_.CreateICmpNE(got, ConstantInt::get(i32, 0), "have_rec");
        });

        BasicBlock *extract = BasicBlock::Create(c, "extract", main);
        BasicBlock *lrtotal = BasicBlock::Create(c, "lr.total", main);
        builder_.CreateCondBr(haveRec, extract, lrtotal);

        // extract: load each field from the buffer.
        builder_.SetInsertPoint(extract);
        // Record-identification selection (E17): if any record type for the
        // primary file carries identification codes, match the record and set
        // the matching record-identifying indicator; skip non-matching records.
        BasicBlock *fieldExtract = emit_record_selection(main, head, extract,
                                                         bufPtr, prog);
        if (fieldExtract != extract)
            builder_.SetInsertPoint(fieldExtract);
        for (const auto &fld : prog.in_fields) {
            if (fld.name.empty()) continue;
            // Field-record-relation (cols 63-64, E17): a field tied to a record
            // type is extracted only when that record-identifying indicator is on.
            if (fld.record_id > 0) {
                Value *on = inds_.load_resolved(fld.record_id);
                BasicBlock *fdo = BasicBlock::Create(c, "fld_rid", main);
                BasicBlock *fafter = BasicBlock::Create(c, "fld_ria", main);
                builder_.CreateCondBr(on, fdo, fafter);
                builder_.SetInsertPoint(fdo);
                emit_one_input_field(c, bufPtr, fld, reclen);
                builder_.CreateBr(fafter);
                builder_.SetInsertPoint(fafter);
            } else {
                emit_one_input_field(c, bufPtr, fld, reclen);
            }
        }

        // Look-ahead fields (E19): peek at the next record and decode the
        // look-ahead fields from it; at EOF the fields are filled with 9s.
        if (!prog.lookahead_fields.empty()) {
            // Look-ahead buffer global (created once).
            if (!la_buf_) {
                auto *bufTy = ArrayType::get(Type::getInt8Ty(c), reclen + 1);
                la_buf_ = new GlobalVariable(
                    *mod_, bufTy, /*isConstant=*/false,
                    GlobalValue::InternalLinkage,
                    ConstantAggregateZero::get(bufTy), "rpg_la_rec");
            }
            Value *laPtr = builder_.CreateConstInBoundsGEP2_32(
                ArrayType::get(Type::getInt8Ty(c), reclen + 1), la_buf_, 0, 0);
            Value *peeked = builder_.CreateCall(peek_next_,
                {fid, laPtr, ConstantInt::get(i64, reclen + 1, false)}, "la_got");
            Value *haveLA = builder_.CreateICmpNE(peeked,
                ConstantInt::get(i32, 0), "have_la");
            BasicBlock *laDo = BasicBlock::Create(c, "la.do", main);
            BasicBlock *laEOF = BasicBlock::Create(c, "la.eof", main);
            BasicBlock *laAfter = BasicBlock::Create(c, "la.after", main);
            builder_.CreateCondBr(haveLA, laDo, laEOF);
            // Decode from the peek buffer.
            builder_.SetInsertPoint(laDo);
            for (const auto &fld : prog.lookahead_fields) {
                if (fld.name.empty()) continue;
                emit_one_input_field(c, laPtr, fld, reclen);
            }
            builder_.CreateBr(laAfter);
            // EOF: fill every look-ahead field with all-9s (manual 83344-83346
            // requires this for numeric AND alphameric look-ahead fields; B4).
            builder_.SetInsertPoint(laEOF);
            auto *i8 = Type::getInt8Ty(c);
            for (const auto &fld : prog.lookahead_fields) {
                if (fld.name.empty()) continue;
                int len = fld.to - fld.from + 1;
                if (len < 1) len = 1;
                if (fld.decimals < 0) {
                    Value *dst = syms_.get_or_create_char_field(fld.name, len);
                    auto *arrTy = ArrayType::get(i8, (unsigned)len);
                    Value *dstp = builder_.CreateConstInBoundsGEP2_32(arrTy,
                        cast<GlobalVariable>(dst), 0, 0, fld.name+"_la9p");
                    for (int b = 0; b < len; ++b) {
                        Value *bp = builder_.CreateInBoundsGEP(i8, dstp,
                            ConstantInt::get(i32, b, true));
                        builder_.CreateStore(
                            ConstantInt::get(i8, (uint8_t)'9'), bp);
                    }
                    continue;
                }
                long nines = 1;
                for (int d = 0; d < len; ++d) nines *= 10;   // 999...9
                builder_.CreateStore(
                    ConstantInt::get(i32, (uint32_t)(nines - 1), true),
                    syms_.get_or_create_field(fld.name));
            }
            builder_.CreateBr(laAfter);
            builder_.SetInsertPoint(laAfter);
        }

        // Control-break detection: for each control field, compare current value
        // to the stored previous; the highest level that changed determines the
        // cascade (set L1..Ln). On the first cycle we still compute maxbrk and
        // store current values as "previous" (establishing the baseline), but we
        // do NOT cascade any levels: the manual bypasses totals until after the
        // first record with control fields is processed (step 18), so a first
        // record must not spuriously fire L1-L9 just because prev was -1.
        if (!ctl_fields.empty()) {
            auto *maxbrk = builder_.CreateAlloca(i32ty, nullptr, "maxbrk");
            builder_.CreateStore(ConstantInt::get(i32ty, 0), maxbrk);
            for (auto &cf : ctl_fields) {
                Value *cur  = syms_.load_field(cf.spec->name);
                Value *prv  = builder_.CreateLoad(i32ty, cf.prev, "pv");
                // B3: control-level fields compare sign-blind (manual
                // 53885-53887: -5 is considered equal to +5).
                Value *same = builder_.CreateICmpEQ(
                    emit_abs_i32(cur), emit_abs_i32(prv), "eq");
                Value *curmax = builder_.CreateLoad(i32ty, maxbrk, "mx");
                Value *lvlc   = ConstantInt::get(i32ty, cf.level);
                // newmax = (!same && lvl>curmax) ? lvl : curmax
                Value *changed = builder_.CreateNot(same, "ch");
                Value *higher  = builder_.CreateICmpSGT(lvlc, curmax, "hi");
                Value *take     = builder_.CreateAnd(changed, higher, "tk");
                Value *newmax  = builder_.CreateSelect(take, lvlc, curmax, "nm");
                builder_.CreateStore(newmax, maxbrk);
            }
            // Update previous values to current (step 25 "make data available").
            for (auto &cf : ctl_fields) {
                builder_.CreateStore(syms_.load_field(cf.spec->name), cf.prev);
            }
            // Cascade-set L1..maxbrk only when NOT on the first cycle (F23).
            Value *notFirst = builder_.CreateNot(
                builder_.CreateLoad(Type::getInt1Ty(c), firstflag, "first2"), "nf");
            BasicBlock *cascadeDo = BasicBlock::Create(c, "ctl.cascade", main);
            BasicBlock *cascadeAfter = BasicBlock::Create(c, "ctl.after", main);
            builder_.CreateCondBr(notFirst, cascadeDo, cascadeAfter);
            builder_.SetInsertPoint(cascadeDo);
            // for n=9..1: if maxbrk>=n set Ln on.
            Value *mb = builder_.CreateLoad(i32ty, maxbrk, "maxbrk_v");
            for (int n = 9; n >= 1; --n) {
                Value *ge = builder_.CreateICmpSGE(mb, ConstantInt::get(i32ty, n), "ge");
                inds_.store_resolved(-1 - n, ge);
            }
            builder_.CreateBr(cascadeAfter);
            builder_.SetInsertPoint(cascadeAfter);
        }

        // Total time (skip on first cycle). Run control-level total chains in
        // ascending order L0, L1, ..., L9. (LR totals run at EOF, below.)
        BasicBlock *totaltime = BasicBlock::Create(c, "total.time", main);
        BasicBlock *detail    = BasicBlock::Create(c, "detail.calcs", main);
        Value *isFirst = builder_.CreateLoad(Type::getInt1Ty(c), firstflag, "first");
        builder_.CreateCondBr(isFirst, detail, totaltime);

        builder_.SetInsertPoint(totaltime);
        BasicBlock *afterTotal = totaltime;
        // L0 chain (runs every total time), then L1..L9.
        afterTotal = emit_spec_chain(main, afterTotal, prog.calcs, "L0");
        for (int n = 1; n <= 9; ++n) {
            std::string lv = "L" + std::to_string(n);
            afterTotal = emit_spec_chain(main, afterTotal, prog.calcs, lv);
        }
        // Total output for control breaks (T-lines not conditioned by LR).
        afterTotal = emit_output(main, afterTotal, prog.outputs, OType::Total);
        builder_.SetInsertPoint(afterTotal);
        // Overflow check (F22): after total output, poll each PRINTER file's
        // overflow latch and run overflow-conditioned output if set.
        afterTotal = emit_overflow_check(main, afterTotal, prog);
        builder_.SetInsertPoint(afterTotal);
        builder_.CreateBr(detail);
        // Clear the first-cycle flag (only meaningful once).
        {
            BasicBlock *det2 = detail;
            builder_.SetInsertPoint(det2);
            builder_.CreateStore(ConstantInt::getFalse(c), firstflag);
            // B1: field indicators (manual step 25) are set here, after total
            // time has run, so total-time conditioning on a field indicator
            // saw the previous record's state rather than the one just read.
            emit_field_indicators_for(prog.in_fields);
            emit_field_indicators_for(prog.lookahead_fields);
        }

        // detail calculations: control level blank.
        BasicBlock *afterDetail = emit_spec_chain(main, detail, prog.calcs, "");
        builder_.SetInsertPoint(afterDetail);
        // Detail output: emit D record lines whose conditions hold.
        afterDetail = emit_output(main, afterDetail, prog.outputs, OType::Detail);
        builder_.SetInsertPoint(afterDetail);
        builder_.CreateBr(head);   // loop back for the next record

        // lr.total: set LR + L1-L9 on. At LR, all control levels are on, so the
        // L0-L9 total calcs run too (in level order), THEN the LR calcs. This
        // ensures the final control group's totals (e.g. a last subtotal) are
        // computed before the grand total.
        builder_.SetInsertPoint(lrtotal);
        inds_.set_all_for_lr();
        BasicBlock *afterLR = lrtotal;
        afterLR = emit_spec_chain(main, afterLR, prog.calcs, "L0");
        for (int n = 1; n <= 9; ++n) {
            std::string lv = "L" + std::to_string(n);
            afterLR = emit_spec_chain(main, afterLR, prog.calcs, lv);
        }
        afterLR = emit_spec_chain(main, afterLR, prog.calcs, "LR");
        builder_.SetInsertPoint(afterLR);
        // Total output (T-lines conditioned by control levels and LR all run).
        afterLR = emit_output(main, afterLR, prog.outputs, OType::Total);
        builder_.SetInsertPoint(afterLR);
        // Overflow check (F22) at LR too: a full page may have overflowed.
        afterLR = emit_overflow_check(main, afterLR, prog);
        builder_.SetInsertPoint(afterLR);

        // exit
        BasicBlock *exitbb = BasicBlock::Create(c, "exit", main);
        builder_.CreateBr(exitbb);
        builder_.SetInsertPoint(exitbb);
        builder_.CreateCall(close_all_);
        llvm::Value *ret = emit_entry_return_value();
        emit_entry_plist_copyout();
        builder_.CreateRet(ret);
        return true;
    }

    /* Multifile cycle (Section F, F20/F21): one primary input file plus one or
     * more secondary input files. Each file's current record is held in its own
     * buffer; each cycle selects the record to process (by M1 match field when
     * present, else primary-first then secondaries in order), copies it into the
     * shared rpg_rec, decodes that file's fields, and runs the shared total/detail
     * tail. MR (matching-record indicator, -12) is set on when the selected
     * record's M1 equals another held record's M1. */
    bool generate_multifile_cycle(const Program &prog) {
        using namespace llvm;
        auto &c  = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i64 = Type::getInt64Ty(c);

        const FSpec *pf = find_primary_input(prog.files);
        if (!pf) return generate_linear(prog);
        auto secs = secondary_inputs(prog.files);

        // The input files in priority order (primary first).
        std::vector<const FSpec *> inputs;
        inputs.push_back(pf);
        for (const auto *s : secs) inputs.push_back(s);
        unsigned nfiles = inputs.size();

        int reclen = pf->reclen > 0 ? pf->reclen : 80;

        EntryInfo ei = create_entry_function();
        Function *main = ei.fn;
        BasicBlock *entry = BasicBlock::Create(c, "entry", main);
        builder_.SetInsertPoint(entry);
        emit_entry_first_call_reset(ei);
        emit_entry_plist_prologue(ei);
        entry = builder_.GetInsertBlock();  // prologue may have added blocks

        emit_prerun_loads();

        // Shared record buffer (the extract tail and C-spec operand resolution
        // all read from rpg_rec, just like the single-file cycle).
        auto *bufTy = ArrayType::get(Type::getInt8Ty(c), (unsigned)reclen + 1);
        rec_buf_ = new GlobalVariable(*mod_, bufTy, /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantAggregateZero::get(bufTy), "rpg_rec");

        // Per-file state: hold buffer, got (i1), key (i32, only when M1 used).
        struct MFFile {
            const FSpec *spec;
            llvm::Value *fid = nullptr;
            llvm::GlobalVariable *buf = nullptr;   // hold buffer
            llvm::GlobalVariable *got = nullptr;   // i1: held record valid
            llvm::GlobalVariable *key = nullptr;   // i32: decoded M1 (numeric only)
            const ISpecField *m1field = nullptr;   // the M1 field for this file
            int has_m1 = false;
            bool m1_alpha = false;                 // M1 field is alphameric (A7)
            // A8: a file may mix matched and unmatched record types (e.g. a
            // header type carrying M1 plus an unmatched trailer type). When
            // true, has_m1 is resolved per held record at prime/advance time
            // instead of being a single file-wide fact.
            bool m1_dynamic = false;
            std::vector<std::pair<const ISpecRec *, const ISpecField *>> rec_m1;
            llvm::GlobalVariable *held_has_m1 = nullptr;   // i1, dynamic only
            int index;                             // priority (0 = primary)
        };
        std::vector<MFFile> mfs(nfiles);

        // Open each input file and create its hold buffer + got flag.
        for (unsigned i = 0; i < nfiles; ++i) {
            MFFile &mf = mfs[i];
            mf.spec = inputs[i];
            mf.index = (int)i;
            Value *nm = builder_.CreateGlobalStringPtr(mf.spec->name, "fname");
            mf.fid = builder_.CreateCall(open_input_, {nm},
                                         mf.spec->name + "_id");
            int rl = mf.spec->reclen > 0 ? mf.spec->reclen : reclen;
            builder_.CreateCall(set_reclen_,
                {mf.fid, ConstantInt::get(i32, rl, true)});
            auto *hbTy = ArrayType::get(Type::getInt8Ty(c), (unsigned)reclen + 1);
            mf.buf = new GlobalVariable(*mod_, hbTy, /*isConstant=*/false,
                GlobalValue::InternalLinkage,
                ConstantAggregateZero::get(hbTy), "rpg_rec_" + mf.spec->name);
            mf.got = new GlobalVariable(*mod_, Type::getInt1Ty(c),
                /*isConstant=*/false, GlobalValue::InternalLinkage,
                ConstantInt::getFalse(c), "got_" + mf.spec->name);
            // Locate this file's M1 match field(s) (Section F). A8: when the
            // file declares more than one identified record type, M1
            // presence/position can differ per type (a matched header plus
            // an unmatched trailer, say), so resolve it dynamically per held
            // record instead of picking the first M1 field found anywhere in
            // the file and applying it to every record type.
            std::vector<const ISpecRec *> ftypes;
            for (const auto &r : prog.in_records)
                if (r.name == mf.spec->name && !r.codes.empty())
                    ftypes.push_back(&r);
            if (ftypes.size() >= 2) {
                mf.m1_dynamic = true;
                mf.has_m1 = true;   // scaffold (key global) always needed
                for (const auto *r : ftypes) {
                    const ISpecField *f = nullptr;
                    for (const auto &fld : prog.in_fields) {
                        if (fld.file == mf.spec->name &&
                            fld.record_id == r->rec_indicator &&
                            fld.match_field.size() == 2 &&
                            fld.match_field[0] == 'M' && fld.match_field[1] == '1') {
                            f = &fld;
                            break;
                        }
                    }
                    mf.rec_m1.push_back({r, f});
                }
                // Only numeric M1 is supported in the per-record-type path;
                // mixing an alphameric M1 across record types in one file
                // isn't exercised by the manual and is left unimplemented.
                mf.m1_alpha = false;
            } else {
                for (const auto &fld : prog.in_fields) {
                    if (fld.file == mf.spec->name &&
                        fld.match_field.size() == 2 &&
                        fld.match_field[0] == 'M' && fld.match_field[1] == '1') {
                        mf.m1field = &fld;
                        mf.has_m1 = true;
                        break;
                    }
                }
                // Alphameric M1 fields (A7): the manual explicitly permits
                // matching on a character key (e.g. a customer code); those
                // are compared as bytes directly out of the hold buffer
                // rather than decoded through the numeric path, so no `key`
                // global is needed.
                if (mf.has_m1) mf.m1_alpha = (mf.m1field->decimals < 0);
            }
            if (mf.has_m1 && !mf.m1_alpha) {
                mf.key = new GlobalVariable(*mod_, Type::getInt32Ty(c),
                    /*isConstant=*/false, GlobalValue::InternalLinkage,
                    ConstantInt::get(i32, 0), "key_" + mf.spec->name);
            }
            if (mf.m1_dynamic) {
                mf.held_has_m1 = new GlobalVariable(*mod_, Type::getInt1Ty(c),
                    /*isConstant=*/false, GlobalValue::InternalLinkage,
                    ConstantInt::getFalse(c), "held_m1_" + mf.spec->name);
            }
        }

        // Open output files (+ overflow config).
        std::unordered_map<std::string, llvm::Value *> out_ids;
        open_output_files(prog, out_ids);
        out_ids_ = &out_ids;
        // Open keyed/random-access input files (CHAIN/SETLL/READ/READE targets).
        open_input_files(prog);

        // Pre-create globals for every input field (shared with the single-file
        // path; fields from all files live in the same name space).
        for (const auto &fld : prog.in_fields) {
            if (fld.name.empty()) continue;
            if (fld.decimals < 0) {
                int len = fld.to - fld.from + 1;
                if (len < 1) len = 1;
                syms_.get_or_create_char_field(fld.name, len);
            } else {
                syms_.get_or_create_field(fld.name);
                syms_.set_numeric_attrs(fld.name, fld.decimals);
            }
        }

        // Control fields (per primary file; control levels are a primary-file
        // concept). Reuse the same structure as the single-file cycle.
        struct CtlField { const ISpecField *spec; llvm::GlobalVariable *prev; int level; };
        std::vector<CtlField> ctl_fields;
        for (const auto &fld : prog.in_fields) {
            if (fld.control_level.size() == 2 && fld.control_level[0]=='L'
                && fld.control_level[1]>='1' && fld.control_level[1]<='9') {
                auto *gv = new GlobalVariable(*mod_, Type::getInt32Ty(c),
                    /*isConstant=*/false, GlobalValue::InternalLinkage,
                    ConstantInt::get(Type::getInt32Ty(c), -1), "prev_" + fld.name);
                ctl_fields.push_back({&fld, gv, fld.control_level[1]-'0'});
            }
        }

        // "first cycle" flag (skip totals on cycle 1; F23 control-break gate).
        auto *i32ty = Type::getInt32Ty(c);
        auto *firstflag = new GlobalVariable(*mod_, Type::getInt1Ty(c),
            /*isConstant=*/false, GlobalValue::InternalLinkage,
            ConstantInt::getTrue(c), "rpg_first");

        /* Helper lambdas to read/write per-file hold buffers. */
        auto buf_ptr = [&](GlobalVariable *gv) -> Value * {
            return builder_.CreateConstInBoundsGEP2_32(
                ArrayType::get(Type::getInt8Ty(c), reclen + 1), gv, 0, 0);
        };
        auto decode_m1 = [&](MFFile &mf, Value *bp) -> Value * {
            // Decode the M1 field from buffer bp into an i32 key.
            const ISpecField *f = mf.m1field;
            Function *dec = get_decimal_;
            if (f->data_format == 'P')      dec = get_packed_;
            else if (f->data_format == 'B') dec = get_binary_;
            Value *v = builder_.CreateCall(dec,
                {bp, ConstantInt::get(i32, reclen, true),
                 ConstantInt::get(i32, f->from, true),
                 ConstantInt::get(i32, f->to, true)}, mf.spec->name + "_mk");
            Value *v32 = builder_.CreateTrunc(v, i32, mf.spec->name + "_mk32");
            return emit_abs_i32(v32);   // B3: match fields compare sign-blind
        };
        // A8: for a file with mixed matched/unmatched record types, identify
        // which type the held buffer actually is and decode M1 only if that
        // specific type declares one; store both the key and whether a key
        // was actually found into per-file globals for the selection loop.
        auto decode_m1_dynamic = [&](MFFile &mf, Value *bp) {
            Value *present = ConstantInt::getFalse(c);
            Value *val = ConstantInt::get(i32, 0);
            for (const auto &rm : mf.rec_m1) {
                Value *match = eval_record_code_match(*rm.first, bp);
                if (rm.second) {
                    const ISpecField *f = rm.second;
                    Function *dec = get_decimal_;
                    if (f->data_format == 'P')      dec = get_packed_;
                    else if (f->data_format == 'B') dec = get_binary_;
                    Value *dv = builder_.CreateCall(dec,
                        {bp, ConstantInt::get(i32, reclen, true),
                         ConstantInt::get(i32, f->from, true),
                         ConstantInt::get(i32, f->to, true)}, mf.spec->name + "_dmk");
                    Value *dv32 = builder_.CreateTrunc(dv, i32, mf.spec->name + "_dmk32");
                    dv32 = emit_abs_i32(dv32);   // B3: match fields compare sign-blind
                    val = builder_.CreateSelect(match, dv32, val, "dmk_sel");
                    present = builder_.CreateOr(present, match, "dmk_present");
                }
            }
            builder_.CreateStore(val, mf.key);
            builder_.CreateStore(present, mf.held_has_m1);
        };

        // Heading pass (D12) before the cycle.
        BasicBlock *afterHead = emit_heading_pass(main, entry, prog.outputs);

        // Prime: read one record from each file into its hold buffer.
        BasicBlock *head = BasicBlock::Create(c, "cycle.head", main);
        for (unsigned i = 0; i < nfiles; ++i) {
            MFFile &mf = mfs[i];
            Value *bp = buf_ptr(mf.buf);
            // E1: gated behind this file's U1-U8 conditioning indicator.
            Value *have = emit_conditioned_bool(main, mf.spec->name, [&]() -> Value* {
                Value *got = builder_.CreateCall(read_next_,
                    {mf.fid, bp, ConstantInt::get(i64, reclen + 1, false)},
                    mf.spec->name + "_prime");
                return builder_.CreateICmpNE(got, ConstantInt::get(i32, 0),
                                             mf.spec->name + "_have");
            });
            builder_.CreateStore(have, mf.got);
            if (mf.has_m1 && !mf.m1_alpha) {
                // Only decode the key when a record was read. Alphameric M1
                // fields (A7) need no decode step: their bytes are compared
                // directly out of the hold buffer at selection time.
                BasicBlock *dk = BasicBlock::Create(c, "prime_k_" + mf.spec->name, main);
                BasicBlock *da = BasicBlock::Create(c, "prime_a_" + mf.spec->name, main);
                builder_.CreateCondBr(have, dk, da);
                builder_.SetInsertPoint(dk);
                if (mf.m1_dynamic) decode_m1_dynamic(mf, bp);
                else                builder_.CreateStore(decode_m1(mf, bp), mf.key);
                builder_.CreateBr(da);
                builder_.SetInsertPoint(da);
            }
        }
        builder_.CreateBr(head);

        // ---- cycle.head: select the record to process this cycle ----
        builder_.SetInsertPoint(head);
        inds_.reset_control_levels();
        // Step 6: turn off record-identifying indicators for every record type.
        for (const auto &r : prog.in_records)
            if (r.rec_indicator > 0)
                inds_.store_resolved(r.rec_indicator, ConstantInt::getFalse(c));
        // MR off until a match is found (set during selection).
        inds_.store_resolved(-12, ConstantInt::getFalse(c));

        // Selection state.
        auto *selAlloca = builder_.CreateAlloca(i32, nullptr, "selected");
        builder_.CreateStore(ConstantInt::get(i32, -1, true), selAlloca);
        auto *minkeyAlloca = builder_.CreateAlloca(i32, nullptr, "minkey");
        builder_.CreateStore(ConstantInt::get(i32, 0), minkeyAlloca);
        auto *mronAlloca = builder_.CreateAlloca(Type::getInt1Ty(c), nullptr, "mron");
        builder_.CreateStore(ConstantInt::getFalse(c), mronAlloca);

        // Alphameric M1 (A7): the running minimum key's bytes live in a
        // shared buffer (sized to the widest alpha M1 field in this program)
        // plus its length, compared via rpg_rt_cmp_str (blank-padded, same
        // semantics as character COMP).
        int alpha_keylen = 0;
        for (const auto &mf : mfs)
            if (mf.has_m1 && mf.m1_alpha)
                alpha_keylen = std::max(alpha_keylen,
                    mf.m1field->to - mf.m1field->from + 1);
        GlobalVariable *minkeyBuf = nullptr;
        llvm::AllocaInst *minkeyLenAlloca = nullptr;
        if (alpha_keylen > 0) {
            auto *bufTy8 = ArrayType::get(Type::getInt8Ty(c), (unsigned)alpha_keylen);
            minkeyBuf = new GlobalVariable(*mod_, bufTy8, /*isConstant=*/false,
                GlobalValue::InternalLinkage,
                ConstantAggregateZero::get(bufTy8), "mf_minkey");
            minkeyLenAlloca = builder_.CreateAlloca(i32, nullptr, "minkeylen");
            builder_.CreateStore(ConstantInt::get(i32, 0), minkeyLenAlloca);
            if (!cmp_str_) {
                cmp_str_ = Function::Create(
                    FunctionType::get(i32, {PointerType::get(c,0), i32,
                                            PointerType::get(c,0), i32}, false),
                    Function::ExternalLinkage, "rpg_rt_cmp_str", mod_.get());
            }
        }

        // E2: F-spec col 18 (sequence). The manual requires the same A/D
        // entry on every file that specifies match fields (77030-77032); take
        // the first non-blank one found among the matching files as the
        // group's direction (ascending unless a file says D), and flag a
        // mismatch instead of silently picking one side.
        bool mf_descending = false;
        {
            char group_seq = 0;
            for (const auto &mf : mfs) {
                if (!mf.has_m1 || mf.spec->sequence == 0) continue;
                if (group_seq == 0) group_seq = mf.spec->sequence;
                else if (group_seq != mf.spec->sequence) {
                    report("input", mf.spec->lineno, 18, DiagKind::Error,
                           "F-spec col 18 (sequence) for file '" + mf.spec->name +
                           "' is '" + std::string(1, mf.spec->sequence) +
                           "' but another matching file specifies '" +
                           std::string(1, group_seq) +
                           "' -- all files with match fields must agree "
                           "(manual 77030-77032)");
                }
            }
            mf_descending = (group_seq == 'D');
        }

        BasicBlock *lrtotal = BasicBlock::Create(c, "lr.total", main);
        BasicBlock *extract = BasicBlock::Create(c, "extract", main);

        // E3: F-spec col 17 (end-of-file requirement). If any file is marked
        // E ("must reach EOF before the program can end"), the program ends
        // as soon as every E-marked file is at EOF, regardless of whether
        // other, unmarked files still hold data (manual 76968-76989). Checked
        // up front, before the selection walk below, because a file with no
        // match field jumps straight to `extract` as soon as it finds any
        // held record -- that shortcut must not bypass this check.
        bool mf_any_required = false;
        for (const auto &mf : mfs)
            if (mf.spec->end_required) { mf_any_required = true; break; }
        if (mf_any_required) {
            Value *allReqDone = ConstantInt::getTrue(c);
            for (const auto &mf : mfs) {
                if (!mf.spec->end_required) continue;
                Value *have = builder_.CreateLoad(Type::getInt1Ty(c), mf.got,
                                                  mf.spec->name + "_got_req");
                Value *done = builder_.CreateNot(have, mf.spec->name + "_done");
                allReqDone = builder_.CreateAnd(allReqDone, done, "req_done_and");
            }
            BasicBlock *scanBlk = BasicBlock::Create(c, "mf_scan", main);
            builder_.CreateCondBr(allReqDone, lrtotal, scanBlk);
            builder_.SetInsertPoint(scanBlk);
        }

        // Walk files in priority order, refining the selection.
        for (unsigned i = 0; i < nfiles; ++i) {
            MFFile &mf = mfs[i];
            BasicBlock *nextFile = BasicBlock::Create(c,
                "sel_" + std::to_string(i) + "_" + mf.spec->name, main);
            BasicBlock *consider = BasicBlock::Create(c,
                "consider_" + std::to_string(i), main);
            // Skip files at EOF or with no held record.
            Value *have = builder_.CreateLoad(Type::getInt1Ty(c), mf.got,
                                              mf.spec->name + "_got");
            builder_.CreateCondBr(have, consider, nextFile);

            builder_.SetInsertPoint(consider);
            Value *curSel = builder_.CreateLoad(i32, selAlloca, "sel");
            if (!mf.has_m1) {
                // No match field: select this file unconditionally and jump to
                // extract (manual step 29: no-match records go first).
                builder_.CreateStore(ConstantInt::get(i32, (int)i, true), selAlloca);
                builder_.CreateBr(extract);
                // Anything after this in the chain is dead; but keep wiring sane.
                builder_.SetInsertPoint(nextFile);
                continue;
            }
            if (mf.m1_dynamic) {
                // A8: this file mixes matched/unmatched record types. Whether
                // the CURRENTLY held record has a key at all depends on which
                // type it turned out to be (decided at prime/advance time);
                // if not, treat it exactly like a no-match-field file this
                // cycle instead of comparing an unrelated type's bytes.
                Value *heldHasM1 = builder_.CreateLoad(Type::getInt1Ty(c),
                    mf.held_has_m1, mf.spec->name + "_heldm1");
                BasicBlock *dynKey = BasicBlock::Create(c,
                    "dynkey_" + std::to_string(i), main);
                BasicBlock *dynNoKey = BasicBlock::Create(c,
                    "dynnokey_" + std::to_string(i), main);
                builder_.CreateCondBr(heldHasM1, dynKey, dynNoKey);

                builder_.SetInsertPoint(dynNoKey);
                builder_.CreateStore(ConstantInt::get(i32, (int)i, true), selAlloca);
                builder_.CreateBr(extract);

                builder_.SetInsertPoint(dynKey);
            }
            Value *noSel = builder_.CreateICmpEQ(curSel,
                ConstantInt::get(i32, -1, true), "nosel");
            BasicBlock *firstPick = BasicBlock::Create(c,
                "first_" + std::to_string(i), main);
            BasicBlock *cmpPick = BasicBlock::Create(c,
                "cmp_" + std::to_string(i), main);
            // Numeric key must be loaded here, in `consider`, before the
            // block is terminated below -- it's only valid for !m1_alpha
            // files (mf.key is null for alpha files).
            Value *mykeyPre = mf.m1_alpha ? nullptr
                : builder_.CreateLoad(i32, mf.key, mf.spec->name + "_key");
            builder_.CreateCondBr(noSel, firstPick, cmpPick);

            if (mf.m1_alpha) {
                // Alphameric M1: compare this file's key bytes (straight out
                // of its hold buffer) against the running minimum's buffer.
                int flen = mf.m1field->to - mf.m1field->from + 1;
                Value *held = buf_ptr(mf.buf);
                Value *fptr = builder_.CreateInBoundsGEP(Type::getInt8Ty(c), held,
                    ConstantInt::get(i32, mf.m1field->from - 1, true),
                    mf.spec->name + "_mkp");
                Value *flenV = ConstantInt::get(i32, flen, true);
                Value *minBufPtr = builder_.CreateConstInBoundsGEP2_32(
                    ArrayType::get(Type::getInt8Ty(c), (unsigned)alpha_keylen),
                    minkeyBuf, 0, 0, "minkey_p");

                builder_.SetInsertPoint(firstPick);
                builder_.CreateStore(ConstantInt::get(i32, (int)i, true), selAlloca);
                builder_.CreateMemCpy(minBufPtr, MaybeAlign(), fptr, MaybeAlign(),
                                      (unsigned)flen);
                builder_.CreateStore(flenV, minkeyLenAlloca);
                builder_.CreateStore(ConstantInt::getFalse(c), mronAlloca);
                builder_.CreateBr(nextFile);

                builder_.SetInsertPoint(cmpPick);
                Value *minLen = builder_.CreateLoad(i32, minkeyLenAlloca, "mkl");
                Value *cmp = builder_.CreateCall(cmp_str_,
                    {fptr, flenV, minBufPtr, minLen}, mf.spec->name + "_kcmp");
                // E2: descending sequence (F-spec col 18 'D') takes the
                // running MAXIMUM key instead of the minimum.
                Value *isLT = mf_descending
                    ? builder_.CreateICmpSGT(cmp, ConstantInt::get(i32, 0), "lt")
                    : builder_.CreateICmpSLT(cmp, ConstantInt::get(i32, 0), "lt");
                Value *isEQ = builder_.CreateICmpEQ(cmp, ConstantInt::get(i32, 0), "eq");
                BasicBlock *ltBlk = BasicBlock::Create(c,
                    "lt_" + std::to_string(i), main);
                BasicBlock *eqNeBlk = BasicBlock::Create(c,
                    "eqne_" + std::to_string(i), main);
                builder_.CreateCondBr(isLT, ltBlk, eqNeBlk);

                builder_.SetInsertPoint(ltBlk);
                builder_.CreateStore(ConstantInt::get(i32, (int)i, true), selAlloca);
                builder_.CreateMemCpy(minBufPtr, MaybeAlign(), fptr, MaybeAlign(),
                                      (unsigned)flen);
                builder_.CreateStore(flenV, minkeyLenAlloca);
                builder_.CreateStore(ConstantInt::getFalse(c), mronAlloca);
                builder_.CreateBr(nextFile);

                builder_.SetInsertPoint(eqNeBlk);
                BasicBlock *eqBlk = BasicBlock::Create(c,
                    "eq_" + std::to_string(i), main);
                BasicBlock *gtBlk = nextFile;
                builder_.CreateCondBr(isEQ, eqBlk, gtBlk);

                builder_.SetInsertPoint(eqBlk);
                builder_.CreateStore(ConstantInt::getTrue(c), mronAlloca);
                builder_.CreateBr(nextFile);

                builder_.SetInsertPoint(nextFile);
                continue;
            }

            // Numeric M1: compare this file's decoded key to the running
            // minimum (both scaled integers, decoded at prime/advance time).
            Value *mykey = mykeyPre;

            builder_.SetInsertPoint(firstPick);
            builder_.CreateStore(ConstantInt::get(i32, (int)i, true), selAlloca);
            builder_.CreateStore(mykey, minkeyAlloca);
            builder_.CreateStore(ConstantInt::getFalse(c), mronAlloca);
            builder_.CreateBr(nextFile);

            builder_.SetInsertPoint(cmpPick);
            Value *minkey = builder_.CreateLoad(i32, minkeyAlloca, "mk");
            // E2: descending sequence (F-spec col 18 'D') takes the running
            // MAXIMUM key instead of the minimum.
            Value *isLT = mf_descending
                ? builder_.CreateICmpSGT(mykey, minkey, "lt")
                : builder_.CreateICmpSLT(mykey, minkey, "lt");
            Value *isEQ = builder_.CreateICmpEQ(mykey, minkey, "eq");
            BasicBlock *ltBlk = BasicBlock::Create(c,
                "lt_" + std::to_string(i), main);
            BasicBlock *eqNeBlk = BasicBlock::Create(c,
                "eqne_" + std::to_string(i), main);
            builder_.CreateCondBr(isLT, ltBlk, eqNeBlk);

            builder_.SetInsertPoint(ltBlk);
            // Lower key: reselect to this file, MR off.
            builder_.CreateStore(ConstantInt::get(i32, (int)i, true), selAlloca);
            builder_.CreateStore(mykey, minkeyAlloca);
            builder_.CreateStore(ConstantInt::getFalse(c), mronAlloca);
            builder_.CreateBr(nextFile);

            builder_.SetInsertPoint(eqNeBlk);
            BasicBlock *eqBlk = BasicBlock::Create(c,
                "eq_" + std::to_string(i), main);
            BasicBlock *gtBlk = nextFile;   // greater key: keep prior selection
            builder_.CreateCondBr(isEQ, eqBlk, gtBlk);

            builder_.SetInsertPoint(eqBlk);
            // Equal key: a match. Keep the higher-priority selection (the earlier
            // file already wins); just set MR on.
            builder_.CreateStore(ConstantInt::getTrue(c), mronAlloca);
            builder_.CreateBr(nextFile);

            builder_.SetInsertPoint(nextFile);
        }

        // After scanning all files: with no E3-required files, the job ends
        // once every held record is gone (selected == -1). When at least one
        // file is E-required, the up-front check above already forced a jump
        // to lrtotal if all required files were done -- reaching here means
        // that wasn't the case, so a required file still has data and the
        // scan above is guaranteed to have selected something.
        if (!mf_any_required) {
            Value *curSel = builder_.CreateLoad(i32, selAlloca, "sel_final");
            Value *none = builder_.CreateICmpEQ(curSel,
                ConstantInt::get(i32, -1, true), "all_eof");
            builder_.CreateCondBr(none, lrtotal, extract);
        } else {
            builder_.CreateBr(extract);
        }

        // ---- extract: copy the selected file's record into rpg_rec, decode its
        // fields, run record-id selection + control-break, then the shared tail.
        // B1: MR itself isn't stored here -- manual step 24 sets it only after
        // total time has run, so total-time conditioning on MR sees the
        // previous cycle's state. mronAlloca (the selection scan's verdict)
        // is read later, at the entry to `detail`.
        builder_.SetInsertPoint(extract);

        // Build a switch on the selected index, one block per file.
        Value *selVal = builder_.CreateLoad(i32, selAlloca, "sel_use");
        BasicBlock *tail = BasicBlock::Create(c, "extract.tail", main);
        auto *sw = builder_.CreateSwitch(selVal, tail, nfiles);
        for (unsigned i = 0; i < nfiles; ++i) {
            MFFile &mf = mfs[i];
            BasicBlock *fb = BasicBlock::Create(c,
                "ext_" + std::to_string(i) + "_" + mf.spec->name, main);
            sw->addCase(ConstantInt::get(i32, (int)i), fb);
            builder_.SetInsertPoint(fb);
            // Copy the hold buffer into rpg_rec.
            Value *src = buf_ptr(mf.buf);
            Value *dst = builder_.CreateConstInBoundsGEP2_32(
                ArrayType::get(Type::getInt8Ty(c), reclen + 1), rec_buf_, 0, 0);
            builder_.CreateMemCpy(dst, MaybeAlign(), src, MaybeAlign(),
                                  (unsigned)reclen + 1);
            // Record-identification selection scoped to this file.
            Program fileprog;   // minimal view: just this file's records
            fileprog.in_records.clear();
            for (const auto &r : prog.in_records)
                if (r.name == mf.spec->name) fileprog.in_records.push_back(r);
            BasicBlock *fext = emit_record_selection(main, head, fb, dst, fileprog);
            builder_.SetInsertPoint(fext);
            // A record type with an indicator but no identification codes is
            // always "selected" when its file is read: turn its indicator on.
            for (const auto &r : fileprog.in_records)
                if (r.codes.empty() && r.rec_indicator > 0)
                    inds_.store_resolved(r.rec_indicator,
                                         ConstantInt::getTrue(c));
            // Decode only this file's fields.
            for (const auto &fld : prog.in_fields) {
                if (fld.name.empty() || fld.file != mf.spec->name) continue;
                if (fld.record_id > 0) {
                    Value *on = inds_.load_resolved(fld.record_id);
                    BasicBlock *fdo = BasicBlock::Create(c, "fld_rid", main);
                    BasicBlock *fafter = BasicBlock::Create(c, "fld_ria", main);
                    builder_.CreateCondBr(on, fdo, fafter);
                    builder_.SetInsertPoint(fdo);
                    emit_one_input_field(c, dst, fld, reclen);
                    builder_.CreateBr(fafter);
                    builder_.SetInsertPoint(fafter);
                } else {
                    emit_one_input_field(c, dst, fld, reclen);
                }
            }
            builder_.CreateBr(tail);
        }

        // ---- extract.tail: control-break detection (shared logic) ----
        builder_.SetInsertPoint(tail);
        if (!ctl_fields.empty()) {
            auto *maxbrk = builder_.CreateAlloca(i32ty, nullptr, "maxbrk");
            builder_.CreateStore(ConstantInt::get(i32ty, 0), maxbrk);
            for (auto &cf : ctl_fields) {
                Value *cur  = syms_.load_field(cf.spec->name);
                Value *prv  = builder_.CreateLoad(i32ty, cf.prev, "pv");
                // B3: control-level fields compare sign-blind (manual
                // 53885-53887: -5 is considered equal to +5).
                Value *same = builder_.CreateICmpEQ(
                    emit_abs_i32(cur), emit_abs_i32(prv), "eq");
                Value *curmax = builder_.CreateLoad(i32ty, maxbrk, "mx");
                Value *lvlc   = ConstantInt::get(i32ty, cf.level);
                Value *changed = builder_.CreateNot(same, "ch");
                Value *higher  = builder_.CreateICmpSGT(lvlc, curmax, "hi");
                Value *take     = builder_.CreateAnd(changed, higher, "tk");
                Value *newmax  = builder_.CreateSelect(take, lvlc, curmax, "nm");
                builder_.CreateStore(newmax, maxbrk);
            }
            for (auto &cf : ctl_fields)
                builder_.CreateStore(syms_.load_field(cf.spec->name), cf.prev);
            // F23: cascade only when not on the first cycle.
            Value *notFirst = builder_.CreateNot(
                builder_.CreateLoad(Type::getInt1Ty(c), firstflag, "first2"), "nf");
            BasicBlock *cascadeDo = BasicBlock::Create(c, "ctl.cascade", main);
            BasicBlock *cascadeAfter = BasicBlock::Create(c, "ctl.after", main);
            builder_.CreateCondBr(notFirst, cascadeDo, cascadeAfter);
            builder_.SetInsertPoint(cascadeDo);
            Value *mb = builder_.CreateLoad(i32ty, maxbrk, "maxbrk_v");
            for (int n = 9; n >= 1; --n) {
                Value *ge = builder_.CreateICmpSGE(mb, ConstantInt::get(i32ty, n), "ge");
                inds_.store_resolved(-1 - n, ge);
            }
            builder_.CreateBr(cascadeAfter);
            builder_.SetInsertPoint(cascadeAfter);
        }

        // ---- Shared total/detail tail (same structure as single-file cycle).
        BasicBlock *totaltime = BasicBlock::Create(c, "total.time", main);
        BasicBlock *detail    = BasicBlock::Create(c, "detail.calcs", main);
        Value *isFirst = builder_.CreateLoad(Type::getInt1Ty(c), firstflag, "first");
        builder_.CreateCondBr(isFirst, detail, totaltime);

        builder_.SetInsertPoint(totaltime);
        BasicBlock *afterTotal = totaltime;
        afterTotal = emit_spec_chain(main, afterTotal, prog.calcs, "L0");
        for (int n = 1; n <= 9; ++n) {
            std::string lv = "L" + std::to_string(n);
            afterTotal = emit_spec_chain(main, afterTotal, prog.calcs, lv);
        }
        afterTotal = emit_output(main, afterTotal, prog.outputs, OType::Total);
        builder_.SetInsertPoint(afterTotal);
        afterTotal = emit_overflow_check(main, afterTotal, prog);
        builder_.SetInsertPoint(afterTotal);
        builder_.CreateBr(detail);

        // detail
        builder_.SetInsertPoint(detail);
        builder_.CreateStore(ConstantInt::getFalse(c), firstflag);
        // B1 (manual steps 24-25): MR and field indicators are set here, once
        // total time has completed, not back at `extract` -- so total-time
        // conditioning on MR or a field indicator sees the previous cycle's
        // state rather than the record just selected/read.
        Value *mron = builder_.CreateLoad(Type::getInt1Ty(c), mronAlloca, "mron_v");
        inds_.store_resolved(-12, mron);
        emit_field_indicators_for(prog.in_fields);
        BasicBlock *afterDetail = emit_spec_chain(main, detail, prog.calcs, "");
        builder_.SetInsertPoint(afterDetail);
        afterDetail = emit_output(main, afterDetail, prog.outputs, OType::Detail);
        builder_.SetInsertPoint(afterDetail);

        // ---- loop tail: advance the file that was selected this cycle ----
        BasicBlock *advSwitch = BasicBlock::Create(c, "advance", main);
        builder_.CreateBr(advSwitch);
        builder_.SetInsertPoint(advSwitch);
        auto *asw = builder_.CreateSwitch(selVal, head, nfiles);
        for (unsigned i = 0; i < nfiles; ++i) {
            MFFile &mf = mfs[i];
            BasicBlock *adv = BasicBlock::Create(c,
                "adv_" + std::to_string(i) + "_" + mf.spec->name, main);
            asw->addCase(ConstantInt::get(i32, (int)i), adv);
            builder_.SetInsertPoint(adv);
            Value *bp = buf_ptr(mf.buf);
            // E1: gated behind this file's U1-U8 conditioning indicator.
            Value *have = emit_conditioned_bool(main, mf.spec->name, [&]() -> Value* {
                Value *got = builder_.CreateCall(read_next_,
                    {mf.fid, bp, ConstantInt::get(i64, reclen + 1, false)},
                    mf.spec->name + "_read");
                return builder_.CreateICmpNE(got, ConstantInt::get(i32, 0),
                                             mf.spec->name + "_have2");
            });
            builder_.CreateStore(have, mf.got);
            if (mf.has_m1 && !mf.m1_alpha) {
                BasicBlock *dk = BasicBlock::Create(c, "adv_k_" + mf.spec->name, main);
                BasicBlock *da = BasicBlock::Create(c, "adv_a_" + mf.spec->name, main);
                builder_.CreateCondBr(have, dk, da);
                builder_.SetInsertPoint(dk);
                if (mf.m1_dynamic) decode_m1_dynamic(mf, bp);
                else                builder_.CreateStore(decode_m1(mf, bp), mf.key);
                builder_.CreateBr(da);
                builder_.SetInsertPoint(da);
            }
            builder_.CreateBr(head);
        }

        // ---- lr.total ----
        builder_.SetInsertPoint(lrtotal);
        inds_.set_all_for_lr();
        BasicBlock *afterLR = lrtotal;
        afterLR = emit_spec_chain(main, afterLR, prog.calcs, "L0");
        for (int n = 1; n <= 9; ++n) {
            std::string lv = "L" + std::to_string(n);
            afterLR = emit_spec_chain(main, afterLR, prog.calcs, lv);
        }
        afterLR = emit_spec_chain(main, afterLR, prog.calcs, "LR");
        builder_.SetInsertPoint(afterLR);
        afterLR = emit_output(main, afterLR, prog.outputs, OType::Total);
        builder_.SetInsertPoint(afterLR);
        afterLR = emit_overflow_check(main, afterLR, prog);
        builder_.SetInsertPoint(afterLR);

        BasicBlock *exitbb = BasicBlock::Create(c, "exit", main);
        builder_.CreateBr(exitbb);
        builder_.SetInsertPoint(exitbb);
        builder_.CreateCall(close_all_);
        llvm::Value *ret = emit_entry_return_value();
        emit_entry_plist_copyout();
        builder_.CreateRet(ret);
        return true;
    }

    /* Build the return value for main. RPGRET-low-byte if present, else LR. */
    llvm::Value *emit_exit_value() {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        if (syms_.has_field("RPGRET")) {
            Value *v = syms_.load_field("RPGRET");
            return builder_.CreateAnd(v, ConstantInt::get(i32, 0xFF), "exit_byte");
        }
        Value *lr = inds_.load_resolved(-1);
        return builder_.CreateZExt(lr, i32, "exit_code");
    }

    /* Emit the output record lines of the requested timing into the block
     * `prev`. Returns the block to continue from (the last merge block, or
     * `prev` if nothing was emitted). */
    /* Emit one output record line (conditioning gate + fields + emit_line)
     * into the block `prev`. Returns the continuation block. Shared by the
     * cycle-timed output (emit_output) and the calc-time EXCPT path
     * (emit_exception). A record whose file has no open output id is skipped. */
    llvm::BasicBlock *emit_one_record(llvm::Function *main,
                                      llvm::BasicBlock *prev,
                                      const ORecord &rec,
                                      bool allow_fetch = true) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);

        // Resolve the output file id for this record.
        auto it = out_ids_->find(rec.file);
        if (it == out_ids_->end()) return prev;   // no output file: skip
        Value *fid = it->second;
        cur_out_fid_ = fid;   // visible to PAGE fields (D14)

        builder_.SetInsertPoint(prev);
        // Conditioning: branch around the line if conditions don't hold.
        // F1: rec.conditions is OR-of-AND groups (base line + any AND/OR
        // continuation lines). E1: also ANDed with the target file's U1-U8
        // conditioning indicator (F-spec cols 71-72) -- off means no records
        // are written to it.
        Value *cond = builder_.CreateAnd(
            eval_conditions_grouped(inds_, builder_, rec.conditions),
            file_cond_ok(rec.file), "out_cond");
        BasicBlock *dob = BasicBlock::Create(c, "out_do", main);
        BasicBlock *nxt = BasicBlock::Create(c, "out_after", main);
        builder_.CreateCondBr(cond, dob, nxt);

        builder_.SetInsertPoint(dob);
        // Disk update/add/delete records (Section G, G25): build the record
        // from its fields into a byte buffer and call the runtime record op,
        // bypassing the printer line path.
        if (rec.rec_op != ORecOp::Write) {
            emit_disk_record(main, fid, rec);
            builder_.CreateBr(nxt);
            builder_.SetInsertPoint(nxt);
            return nxt;
        }
        // Skip-before (D13): advance to the requested line (form-feed on a
        // lower line). Space-before (col 17) adds blank lines first.
        if (rec.skip_before > 0) {
            builder_.CreateCall(skip_fn_, {fid,
                ConstantInt::get(i32, rec.skip_before, true)});
        }
        // Build the line: line_begin(reclen). Use 132 for PRINTER default.
        builder_.CreateCall(line_begin_, {ConstantInt::get(i32, 132)});
        // Place each field/constant. Per-field conditioning indicators
        // (cols 23-31) gate individual fields (D15): a field whose conditions
        // don't hold is skipped.
        for (const auto &f : rec.fields) {
            if (f.conditions.empty()) {
                emit_one_field(main, f);
            } else {
                Value *fc = eval_conditions(inds_, builder_, f.conditions);
                BasicBlock *fdo = BasicBlock::Create(c, "fld_do", main);
                BasicBlock *fskip = BasicBlock::Create(c, "fld_skip", main);
                BasicBlock *fafter = BasicBlock::Create(c, "fld_after", main);
                builder_.CreateCondBr(fc, fdo, fskip);
                builder_.SetInsertPoint(fdo);
                emit_one_field(main, f);
                builder_.CreateBr(fafter);
                builder_.SetInsertPoint(fskip);
                builder_.CreateBr(fafter);
                builder_.SetInsertPoint(fafter);
            }
        }
        builder_.CreateCall(emit_line_,
            {fid, ConstantInt::get(i32, rec.space_after, true)});
        // Skip-after (D13): advance to the requested line after printing.
        if (rec.skip_after > 0) {
            builder_.CreateCall(skip_fn_, {fid,
                ConstantInt::get(i32, rec.skip_after, true)});
        }
        // F2: fetch overflow (O-spec col 16 = F). Poll this file's overflow
        // latch right here, immediately after this line prints, instead of
        // waiting for the normal cycle-time check (emit_overflow_check,
        // which only runs once per cycle, after total time and before
        // detail time -- a detail line's own overflow would otherwise not
        // be serviced until the *next* cycle, or never in a single-cycle
        // program). Manual 88310-88356: fetch overflow is suppressed when
        // this same line is itself conditioned on the file's overflow
        // indicator ("the overflow routine is not fetched"). `allow_fetch`
        // is false for records emitted from inside an overflow-output pass
        // itself, so a fetch-flagged record reached that way services at
        // most one nested level instead of chaining indefinitely.
        if (rec.fetch_overflow && allow_fetch && outputs_) {
            const FSpec *ofs = nullptr;
            if (files_)
                for (const auto &f : *files_)
                    if (f.name == rec.file) { ofs = &f; break; }
            if (ofs && ofs->has_overflow &&
                !has_overflow_condition(rec.conditions, ofs->overflow_ind)) {
                Value *latched = builder_.CreateCall(take_overflow_, {fid},
                                                      "ov_take_fetch");
                latched = builder_.CreateICmpNE(
                    latched, ConstantInt::get(i32, 0), "ov_on_fetch");
                BasicBlock *fdo = BasicBlock::Create(c, "fetchov_do", main);
                BasicBlock *fafter = BasicBlock::Create(c, "fetchov_after", main);
                builder_.CreateCondBr(latched, fdo, fafter);
                builder_.SetInsertPoint(fdo);
                inds_.store_resolved(ofs->overflow_ind, ConstantInt::getTrue(c));
                BasicBlock *ovend = emit_overflow_output(main, fdo, *outputs_,
                                                         ofs->overflow_ind,
                                                         /*allow_fetch=*/false);
                builder_.SetInsertPoint(ovend);
                inds_.store_resolved(ofs->overflow_ind, ConstantInt::getFalse(c));
                builder_.CreateBr(fafter);
                builder_.SetInsertPoint(fafter);
            }
        }
        builder_.CreateBr(nxt);
        builder_.SetInsertPoint(nxt);
        return nxt;
    }

    /* Emit a disk update/add/delete record (Section G, G25). For ADD/UPDATE the
     * record's fields are placed into the line buffer the same way printer lines
     * are, then flushed as a disk record; DELETE just marks the current record. */
    void emit_disk_record(llvm::Function *main, llvm::Value *fid, const ORecord &rec) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        if (rec.rec_op == ORecOp::Delete) {
            builder_.CreateCall(delete_rec_, {fid});
            return;
        }
        // Build the record from its fields (reuse the printer placement logic).
        int width = 132;
        if (files_) for (const auto &f : *files_) if (f.name == rec.file) width = f.reclen > 0 ? f.reclen : 132;
        builder_.CreateCall(line_begin_, {ConstantInt::get(i32, width, true)});
        for (const auto &f : rec.fields) {
            if (f.conditions.empty()) {
                emit_one_field(main, f);
            } else {
                Value *fc = eval_conditions(inds_, builder_, f.conditions);
                BasicBlock *fdo = BasicBlock::Create(mod_->getContext(), "dfld_do", main);
                BasicBlock *fskip = BasicBlock::Create(mod_->getContext(), "dfld_skip", main);
                BasicBlock *fafter = BasicBlock::Create(mod_->getContext(), "dfld_after", main);
                builder_.CreateCondBr(fc, fdo, fskip);
                builder_.SetInsertPoint(fdo);
                emit_one_field(main, f);
                builder_.CreateBr(fafter);
                builder_.SetInsertPoint(fskip);
                builder_.CreateBr(fafter);
                builder_.SetInsertPoint(fafter);
            }
        }
        int op = (rec.rec_op == ORecOp::Update) ? 1 : 0;
        builder_.CreateCall(flush_rec_, {fid, ConstantInt::get(i32, op, true)});
    }

    /* Place a single field/constant onto the current output line. Assumes the
     * caller has already set the insert point and run line_begin_. */
    void emit_one_field(llvm::Function *main, const OField &f) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        if (f.is_const) {
            Value *str = builder_.CreateGlobalStringPtr(f.text, "oconst");
            builder_.CreateCall(line_put_str_, {
                str,
                ConstantInt::get(i32, (int)f.text.size(), true),
                ConstantInt::get(i32, f.end_pos, true)});
        } else if (!f.edit_word.empty()) {
            // Edit word (D16): format the numeric field via the runtime.
            emit_edit_word_field(f);
        } else if (is_page_field(f.name)) {
            // PAGE / PAGE1-PAGE7 (D14): place the page counter.
            emit_page_field(main, f);
        } else if (syms_.has_field(f.name)) {
            if (syms_.is_char_field(f.name)) {
                // Character field: put_str with its bytes.
                int len = syms_.info(f.name)->length;
                Value *ptr; int plen;
                syms_.resolve_char_operand(f.name, ptr, plen);
                builder_.CreateCall(line_put_str_, {
                    ptr, ConstantInt::get(i32, len, true),
                    ConstantInt::get(i32, f.end_pos, true)});
            } else {
                // Numeric field. Section C: honour the field's decimal
                // positions so scaled-integer values print correctly.
                Value *v = syms_.load_field(f.name);
                Value *v64 = builder_.CreateSExt(v, Type::getInt64Ty(c), f.name+"_o");
                int dec = syms_.field_decimals(f.name);
                if (f.edit_code != 0) {
                    // Edit code: format via runtime into a temp buffer.
                    Value *buf = builder_.CreateAlloca(
                        Type::getInt8Ty(c),
                        ConstantInt::get(i32, 64, false), f.name+"_eb");
                    Value *n = builder_.CreateCall(edit_dec_fn_, {
                        v64,
                        ConstantInt::get(i32, (int)f.edit_code, true),
                        ConstantInt::get(i32, 0, true),
                        ConstantInt::get(i32, dec, true),
                        ConstantInt::get(i32, (int)f.fill_char, true),
                        buf,
                        ConstantInt::get(i32, 64, true)}, f.name+"_en");
                    builder_.CreateCall(line_put_str_, {buf, n,
                        ConstantInt::get(i32, f.end_pos, true)});
                } else {
                    builder_.CreateCall(line_put_num_dec_, {
                        v64, ConstantInt::get(i32, f.end_pos, true),
                        ConstantInt::get(i32, dec, true)});
                }
            }
        }
    }

    /* True if `name` is the PAGE / PAGE1-PAGE7 reserved output field. */
    bool is_page_field(const std::string &name) {
        std::string u;
        std::transform(name.begin(), name.end(), std::back_inserter(u),
                       [](unsigned char ch){ return std::toupper(ch); });
        if (u == "PAGE") return true;
        if (u.size() == 5 && u.substr(0,4) == "PAGE" &&
            u[4] >= '1' && u[4] <= '7') return true;
        return false;
    }

    /* Place a PAGE / PAGE1-PAGE7 page-counter value (D14). The page counters
     * live in the runtime, advanced on form-feed by rpg_rt_skip. */
    void emit_page_field(llvm::Function * /*main*/, const OField &f) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i64 = Type::getInt64Ty(c);
        if (!page_fn_) {
            page_fn_ = Function::Create(
                FunctionType::get(i32, {i32, i32}, false),
                Function::ExternalLinkage, "rpg_rt_page", mod_.get());
        }
        // Determine which counter: PAGE => 0 (the record's own file); PAGE1-7
        // => 1..7 (the nth-opened file). The file id is the current record's;
        // for PAGE1-7 we pass the digit and the runtime maps it to the file.
        std::string u;
        std::transform(f.name.begin(), f.name.end(), std::back_inserter(u),
                       [](unsigned char ch){ return std::toupper(ch); });
        int which = 0;
        if (u.size() == 5 && u.substr(0,4) == "PAGE") which = u[4] - '0';
        // The current record's file id is on top of the call chain; fetch it
        // from the record being emitted via the cur_out_fid_ member.
        Value *fid = cur_out_fid_;
        Value *pg = builder_.CreateCall(page_fn_,
            {fid, ConstantInt::get(i32, which, true)}, "pg");
        Value *pg64 = builder_.CreateSExt(pg, i64, "pg64");
        builder_.CreateCall(line_put_num_, {pg64,
            ConstantInt::get(i32, f.end_pos, true)});
    }

    /* Format a numeric field via an edit word (D16). */
    void emit_edit_word_field(const OField &f) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i64 = Type::getInt64Ty(c);
        auto *ptr = PointerType::get(c, 0);
        if (!edit_word_fn_) {
            edit_word_fn_ = Function::Create(
                FunctionType::get(i32, {i64, ptr, i32, i32, i32, ptr, i32}, false),
                Function::ExternalLinkage, "rpg_rt_edit_word", mod_.get());
        }
        Value *v = syms_.load_field(f.name);
        Value *v64 = builder_.CreateSExt(v, i64, f.name+"_ew");
        int dec = syms_.field_decimals(f.name);
        Value *word = builder_.CreateGlobalStringPtr(f.edit_word, "eword");
        Value *buf = builder_.CreateAlloca(Type::getInt8Ty(c),
            ConstantInt::get(i32, 64, false), f.name+"_ewb");
        // D1: the floating-currency character (H-spec col 18; default '$').
        Value *n = builder_.CreateCall(edit_word_fn_, {
            v64, word,
            ConstantInt::get(i32, (int)f.edit_word.size(), true),
            ConstantInt::get(i32, dec, true),
            ConstantInt::get(i32, (int)currency_symbol_, true),
            buf,
            ConstantInt::get(i32, 64, true)}, f.name+"_ewn");
        builder_.CreateCall(line_put_str_, {buf, n,
            ConstantInt::get(i32, f.end_pos, true)});
    }

    llvm::BasicBlock *emit_output(llvm::Function *main, llvm::BasicBlock *prev,
                     const std::vector<ORecord> &records, OType want) {
        for (const auto &rec : records) {
            // Heading lines are emitted once at program start by
            // emit_heading_pass, not during the detail/total cycle.
            if (rec.type == OType::Heading) continue;
            if (rec.type != want) continue;
            prev = emit_one_record(main, prev, rec);
        }
        return prev;
    }

    /* True if any condition (in any OR group) references the overflow
     * indicator `ov_idx`. */
    bool has_overflow_condition(const std::vector<std::vector<CondInd>> &groups,
                                int ov_idx) {
        for (const auto &g : groups)
            for (const auto &c : g) if (c.indicator == ov_idx) return true;
        return false;
    }

    /* Overflow output (Section F, F22). When the overflow indicator is on, the
     * program writes (manual steps for "when the overflow indicator turns on"):
     * total lines conditioned by the overflow indicator, then heading + detail
     * lines conditioned by it. We run this with the indicator forced on, then
     * turn it off. Each record still carries its own conditioning gate, so only
     * lines referencing the overflow indicator actually print. */
    llvm::BasicBlock *emit_overflow_output(llvm::Function *main,
                                           llvm::BasicBlock *prev,
                                           const std::vector<ORecord> &records,
                                           int ov_idx,
                                           bool allow_fetch = true) {
        for (const auto &rec : records) {
            if (rec.type == OType::Exception) continue;
            if (rec.type == OType::Heading) {
                if (has_overflow_condition(rec.conditions, ov_idx))
                    prev = emit_one_record(main, prev, rec, allow_fetch);
            } else {
                // Detail or Total lines conditioned by the overflow indicator.
                if (has_overflow_condition(rec.conditions, ov_idx))
                    prev = emit_one_record(main, prev, rec, allow_fetch);
            }
        }
        return prev;
    }

    /* Poll the runtime for overflow on each PRINTER file that carries an
     * overflow indicator, and if overflow occurred, run that indicator's
     * overflow output then clear the indicator. This implements manual cycle
     * step 22-23 (after total output, before detail). `prev` is the current
     * insertion-point block; returns the continuation block. */
    llvm::BasicBlock *emit_overflow_check(llvm::Function *main,
                                          llvm::BasicBlock *prev,
                                          const Program &prog) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        // Gather {file_id, ov_idx} for each output file with an overflow ind.
        struct OvFile { llvm::Value *fid; int ov_idx; };
        std::vector<OvFile> ovs;
        for (const auto &f : prog.files) {
            if (!f.has_overflow) continue;
            auto it = out_ids_->find(f.name);
            if (it == out_ids_->end()) continue;
            ovs.push_back({it->second, f.overflow_ind});
        }
        if (ovs.empty()) return prev;
        for (const auto &of : ovs) {
            builder_.SetInsertPoint(prev);
            Value *latched = builder_.CreateCall(take_overflow_, {of.fid},
                                                 "ov_take");
            latched = builder_.CreateICmpNE(latched, ConstantInt::get(i32, 0),
                                            "ov_on");
            // Force the overflow indicator to the polled state, then if on,
            // emit overflow output, then turn it off (step 5).
            inds_.store_resolved(of.ov_idx, latched);
            BasicBlock *doOv = BasicBlock::Create(mod_->getContext(),
                                                  "ov_do", main);
            BasicBlock *after = BasicBlock::Create(mod_->getContext(),
                                                   "ov_after", main);
            builder_.CreateCondBr(latched, doOv, after);
            builder_.SetInsertPoint(doOv);
            prev = emit_overflow_output(main, doOv, prog.outputs, of.ov_idx);
            inds_.store_resolved(of.ov_idx,
                                 ConstantInt::getFalse(mod_->getContext()));
            builder_.CreateBr(after);
            builder_.SetInsertPoint(after);
            prev = after;
        }
        return prev;
    }

    /* First-page pass (D12): run once at program start, while the 1P indicator
     * is on. Emits all Heading records (each gated by its own conditions, which
     * may include 1P) and any non-heading records conditioned by 1P. Then turns
     * 1P off so these records never fire again. */
    llvm::BasicBlock *emit_heading_pass(llvm::Function *main,
                                        llvm::BasicBlock *prev,
                                        const std::vector<ORecord> &records) {
        bool any = false;
        for (const auto &rec : records) {
            if (rec.type == OType::Heading) {
                prev = emit_one_record(main, prev, rec);
                any = true;
            } else if (rec.type == OType::Detail && has_1p_condition(rec.conditions)) {
                // A detail line conditioned by 1P also prints at first-page time.
                prev = emit_one_record(main, prev, rec);
                any = true;
            }
        }
        if (any) inds_.clear_first_page();
        return prev;
    }

    /* True if any condition (in any OR group) references the 1P indicator
     * (idx -11). */
    bool has_1p_condition(const std::vector<std::vector<CondInd>> &groups) {
        for (const auto &g : groups)
            for (const auto &c : g) if (c.indicator == -11) return true;
        return false;
    }

    /* EXCPT: write type-E (exception) O-records during calculation time. With a
     * name in factor2, only E-records whose except_name matches are written;
     * with blank factor2, only the unnamed E-records. `outputs` is the whole
     * O-spec record list; `name` is the EXCPT name from the C-spec. */
    llvm::BasicBlock *emit_exception(llvm::Function *main,
                                     llvm::BasicBlock *prev,
                                     const std::vector<ORecord> &outputs,
                                     const std::string &name) {
        for (const auto &rec : outputs) {
            if (rec.type != OType::Exception) continue;
            if (rec.except_name != name) continue;
            prev = emit_one_record(main, prev, rec);
        }
        return prev;
    }

    /* Emit a chain of C-specs that share a control-level timing. `level` is:
     *   ""    => detail (control_level blank)
     *   "L0"  => total, always runs at total time
     *   "L1".."L9" => total, gated on that level or higher being on
     *   "LR"  => total, gated on LR
     *
     * Two passes: (1) pre-create a basic block for every TAG in this chain so
     * that GOTOs -- including forward references -- can target them; (2) emit
     * the bodies. */
    llvm::BasicBlock *emit_spec_chain(llvm::Function *main,
                                      llvm::BasicBlock *prev,
                                      const std::vector<CSpec> &specs,
                                      const std::string &level) {
        tag_blocks_.clear();

        // Filter to the specs belonging to this control level, EXCLUDING specs
        // inside subroutines (BEGSR..ENDSR ranges) which are compiled separately.
        std::vector<const CSpec *> chain;
        bool in_sub = false;
        for (const auto &c : specs) {
            if (c.op == Op::BEGSR) { in_sub = true; continue; }
            if (c.op == Op::ENDSR) { in_sub = false; continue; }
            if (in_sub) continue;   // skip subroutine body specs
            std::string cl = c.control_level;
            if (cl.empty()) cl = ""; else if (cl == "L0") cl = "L0";
            else if (cl == "SR") continue;   // subroutine-tagged line
            if (cl != level) continue;
            chain.push_back(&c);
        }

        // Pass 1: pre-create TAG blocks.
        auto &ctx = main->getContext();
        for (const auto *c : chain) {
            if (c->op == Op::TAG && !c->factor1.empty()) {
                tag_blocks_[c->factor1] =
                    llvm::BasicBlock::Create(ctx, "tag_" + c->factor1, main);
            }
        }

        // Pass 2: emit bodies. Structured ops (IF/ELSE/END/DOW/DOU) are handled
        // here with a block stack so they nest; everything else goes through
        // emit_spec (which handles TAG/GOTO and conditioned regular ops).
        struct Frame {
            Op op;                       // IF, DOW, DOU, DO, or CAS
            llvm::BasicBlock *header;    // DOW loop-back target (test-at-top)
            llvm::BasicBlock *exit;      // block after the construct (merge/exit)
            const CSpec *spec;           // the opening spec (for DOU bottom-test)
            bool has_else;               // IF: an ELSE was seen
            // DO (counted loop) bookkeeping.
            llvm::Value *do_index_ptr = nullptr;  // &index field
            llvm::Value *do_limit = nullptr;      // limit value (evaluated once)
            std::string  do_index_name;           // result field name (or generated)
        };
        std::vector<Frame> frames;

        // Control-level gate: for an L1..L9 / LR chain, run the whole chain only
        // if that level's indicator is on (L0/detail/blank run unconditionally).
        // Because of the cascade, an Ln chain runs when Ln (and all lower) are on.
        if (chain.empty()) return prev;
        {
            using namespace llvm;
            int lvind = 0;   // resolved indicator index, 0 = none
            if (level == "LR")            lvind = -1;
            else if (level.size() == 2 && level[0]=='L' && level[1]>='1' && level[1]<='9')
                lvind = -1 - (level[1] - '0');   // L1 -> -2 ...
            if (lvind != 0) {
                auto &ctx = main->getContext();
                BasicBlock *dob = BasicBlock::Create(ctx, "lvl_" + level, main);
                BasicBlock *after = BasicBlock::Create(ctx, "lvl_after", main);
                builder_.SetInsertPoint(prev);
                Value *on = inds_.load_resolved(lvind);
                builder_.CreateCondBr(on, dob, after);
                prev = dob;
                // remember `after` to return at the end
                level_skip_ = after;
            } else {
                level_skip_ = nullptr;
            }
        }

        for (const auto *c : chain) {
            using namespace llvm;
            auto &ctx = main->getContext();

            if (c->op == Op::IF || c->op == Op::DOW || c->op == Op::DOU
                                 || c->op == Op::DO) {
                Frame fr{};
                fr.op   = c->op;
                fr.exit = BasicBlock::Create(ctx, "merge", main);
                fr.spec = c;

                if (c->op == Op::IF) {
                    // then-block falls through. The false path goes to a
                    // placeholder block; if an ELSE appears later it becomes
                    // the else-block's entry, otherwise it just flows to merge.
                    BasicBlock *thenb = BasicBlock::Create(ctx, "then", main);
                    BasicBlock *falseb = BasicBlock::Create(ctx, "if_false", main);
                    builder_.SetInsertPoint(prev);
                    Value *cond = eval_cmp_op(*c);
                    Value *cind = eval_conditions(inds_, builder_, c->conditions);
                    if (cond) cond = builder_.CreateAnd(cond, cind, "if_cond");
                    else      cond = cind;
                    builder_.CreateCondBr(cond, thenb, falseb);
                    fr.header = falseb;   // repurpose header slot as false-target
                    prev = thenb;
                } else if (c->op == Op::DOW) {
                    // test-at-top: header tests; false -> exit.
                    BasicBlock *header = BasicBlock::Create(ctx, "dow_head", main);
                    BasicBlock *body   = BasicBlock::Create(ctx, "dow_body", main);
                    builder_.SetInsertPoint(prev);
                    builder_.CreateBr(header);
                    builder_.SetInsertPoint(header);
                    Value *cond = eval_cmp_op(*c);
                    builder_.CreateCondBr(cond ? cond : ConstantInt::getTrue(ctx),
                                          body, fr.exit);
                    fr.header = header;
                    prev = body;
                } else if (c->op == Op::DO) { // DO -- counted loop.
                    // The DO operation itself is gated by its conditioning
                    // indicators. If they fail, skip the whole group (step 1).
                    builder_.SetInsertPoint(prev);
                    Value *cind = eval_conditions(inds_, builder_, c->conditions);
                    BasicBlock *doEntry = BasicBlock::Create(ctx, "do_entry", main);
                    builder_.CreateCondBr(cind, doEntry, fr.exit);

                    builder_.SetInsertPoint(doEntry);
                    // Step 2: move the starting value (factor1, default 1) into
                    // the index field (result, compiler-generated if absent).
                    auto *i32 = Type::getInt32Ty(ctx);
                    Value *start = c->factor1.empty()
                        ? (Value*)ConstantInt::get(i32, 1)
                        : syms_.resolve_operand(c->factor1);
                    if (!start) {
                        report("input", c->lineno, 18, DiagKind::Error,
                               "DO requires a numeric starting value in factor 1");
                        start = ConstantInt::get(i32, 1);
                    }
                    // Resolve the index field. If a result field is named, use
                    // it; otherwise generate a hidden one per group.
                    std::string idxName = c->result;
                    if (idxName.empty())
                        idxName = "__do_idx" + std::to_string(c->lineno);
                    Value *idxPtr = syms_.get_or_create_field(idxName);
                    builder_.CreateStore(start, idxPtr);
                    fr.do_index_ptr  = idxPtr;
                    fr.do_index_name = idxName;

                    // The limit is read once at entry (the manual allows the
                    // limit to be modified inside the body, but re-reading each
                    // iteration matches the step-3 description as "compare
                    // index to factor2"; we evaluate the limit value up front
                    // so a literal/field limit is stable).
                    Value *limit = c->factor2.empty()
                        ? (Value*)ConstantInt::get(i32, 1)
                        : syms_.resolve_operand(c->factor2);
                    if (!limit) {
                        report("input", c->lineno, 33, DiagKind::Error,
                               "DO requires a numeric limit value in factor 2");
                        limit = ConstantInt::get(i32, 1);
                    }
                    fr.do_limit = limit;

                    // Header (step 3): if index > limit, exit; else run body.
                    BasicBlock *header = BasicBlock::Create(ctx, "do_head", main);
                    BasicBlock *body   = BasicBlock::Create(ctx, "do_body", main);
                    builder_.CreateBr(header);
                    builder_.SetInsertPoint(header);
                    Value *idx = builder_.CreateLoad(i32, idxPtr, "do_idx");
                    Value *gt  = builder_.CreateICmpSGT(idx, limit, "do_gt");
                    builder_.CreateCondBr(gt, fr.exit, body);
                    fr.header = header;
                    prev = body;
                } else { // DOU -- test-at-bottom; enter body unconditionally.
                    BasicBlock *body = BasicBlock::Create(ctx, "dou_body", main);
                    builder_.SetInsertPoint(prev);
                    builder_.CreateBr(body);
                    fr.header = body;   // back-edge target (the body top)
                    prev = body;
                }
                frames.push_back(std::move(fr));
                continue;
            }

            if (c->op == Op::ELSE) {
                if (frames.empty() || frames.back().op != Op::IF) {
                    report("input", c->lineno, 28, DiagKind::Error,
                           "ELSE without matching IF");
                    continue;
                }
                Frame &fr = frames.back();
                if (fr.has_else) {
                    report("input", c->lineno, 28, DiagKind::Error,
                           "duplicate ELSE in one IF");
                    continue;
                }
                // Close the then-branch (branch to merge). The IF's false-target
                // block becomes the entry to the else code.
                builder_.SetInsertPoint(prev);
                builder_.CreateBr(fr.exit);
                builder_.SetInsertPoint(fr.header);   // false-target -> else body
                fr.has_else = true;
                prev = fr.header;
                continue;
            }

            if (c->op == Op::END) {
                if (frames.empty()) {
                    report("input", c->lineno, 28, DiagKind::Error,
                           "END without matching IF/DOW/DOU/DO/CAS");
                    continue;
                }
                Frame fr = std::move(frames.back());
                frames.pop_back();

                if (fr.op == Op::IF) {
                    // Close whichever then/else block we ended in -> merge.
                    builder_.SetInsertPoint(prev);
                    builder_.CreateBr(fr.exit);
                    // If there was no ELSE, the IF's false-target block still
                    // needs a terminator: branch it to merge too.
                    if (!fr.has_else) {
                        builder_.SetInsertPoint(fr.header);
                        builder_.CreateBr(fr.exit);
                    }
                    prev = fr.exit;
                } else if (fr.op == Op::DOW) {
                    // Loop back to header (which re-tests at top).
                    builder_.SetInsertPoint(prev);
                    builder_.CreateBr(fr.header);
                    prev = fr.exit;
                } else if (fr.op == Op::DO) {
                    // Step 6: add the increment (END's factor2, default 1) to
                    // the index and branch back to the header (step 3). The
                    // increment is re-read each iteration so a field-valued
                    // increment can change inside the loop.
                    builder_.SetInsertPoint(prev);
                    auto *i32 = Type::getInt32Ty(ctx);
                    const CSpec *endspec = c;   // the END spec
                    Value *incv;
                    if (!endspec->factor2.empty()) {
                        incv = syms_.resolve_operand(endspec->factor2);
                        if (!incv) incv = ConstantInt::get(i32, 1);
                    } else {
                        incv = ConstantInt::get(i32, 1);
                    }
                    Value *cur = builder_.CreateLoad(i32, fr.do_index_ptr, "do_i");
                    Value *nxt = builder_.CreateAdd(cur, incv, "do_inc");
                    builder_.CreateStore(nxt, fr.do_index_ptr);
                    builder_.CreateBr(fr.header);
                    prev = fr.exit;
                } else if (fr.op == Op::CAS) {
                    // END of a CAS group: the fall-through (no CASxx matched,
                    // or a matched sub returned) continues at the merge block.
                    builder_.SetInsertPoint(prev);
                    builder_.CreateBr(fr.exit);
                    prev = fr.exit;
                } else { // DOU -- test-at-bottom, here at END.
                    // body falls into a test block; exit when cond TRUE, else
                    // loop back to the body top (stored in fr.header).
                    BasicBlock *test = BasicBlock::Create(ctx, "dou_test", main);
                    builder_.SetInsertPoint(prev);
                    builder_.CreateBr(test);
                    builder_.SetInsertPoint(test);
                    Value *cond = eval_cmp_op(*fr.spec);
                    builder_.CreateCondBr(cond ? cond : ConstantInt::getTrue(ctx),
                                          fr.exit, fr.header);
                    prev = fr.exit;
                }
                continue;
            }

            // CASxx: conditionally call the subroutine named in the result
            // field. A CAS group is one or more CASxx ops followed by END. We
            // lazily open a CAS frame on the first CASxx (sharing one merge
            // block for the whole group) and close it at END. An unconditional
            // CAS (blank xx) calls its sub unconditionally and ends the group.
            if (c->op == Op::CAS) {
                // Open a CAS frame if we're not already in one.
                if (frames.empty() || frames.back().op != Op::CAS) {
                    Frame fr{};
                    fr.op   = Op::CAS;
                    fr.exit = BasicBlock::Create(ctx, "cas_end", main);
                    frames.push_back(std::move(fr));
                }
                BasicBlock *casExit = frames.back().exit;

                // Emit into the current fall-through block. (eval_conditions /
                // eval_cmp_op below emit instructions, so the insert point must
                // be set first.)
                builder_.SetInsertPoint(prev);

                // Conditioning indicators on the CASxx line gate just this line.
                Value *cind = eval_conditions(inds_, builder_, c->conditions);

                // The comparison (xx) between factor1 and factor2. For an
                // unconditional CAS (blank xx) there is no comparison; the sub
                // runs whenever the line's conditioning indicators hold.
                Value *cmp = nullptr;
                if (c->cmp != CmpOp::NONE || (!c->factor1.empty() && !c->factor2.empty())) {
                    if (!c->factor1.empty() && !c->factor2.empty())
                        cmp = eval_cmp_op(*c);
                }
                // Resulting indicators (HI/LO/EQ): set per the comparison even
                // when no subroutine is named.
                emit_cas_result_indicators(*c);

                // Decide whether this CASxx fires.
                Value *fire = cmp;
                if (cmp && cind)  fire = builder_.CreateAnd(cmp, cind, "cas_c");
                else if (cind)    fire = cind;

                BasicBlock *callBB =
                    BasicBlock::Create(ctx, "cas_call", main);
                BasicBlock *nextBB =
                    BasicBlock::Create(ctx, "cas_next", main);

                builder_.SetInsertPoint(prev);
                // A12: the unconditional CAS form (blank comparison operator)
                // only makes the *comparison* unconditional (manual
                // 105580-105586) -- the line's own conditioning indicators
                // (cols 9-17) still gate it like any other C-spec op. Since
                // eval_conditions never returns null, `fire` already equals
                // `cind` alone for the unconditional case (no cmp to AND in);
                // it must never be bypassed with a bare CreateBr.
                if (fire) {
                    builder_.CreateCondBr(fire, callBB, nextBB);
                } else {
                    // Defensive: eval_conditions always returns a value, so
                    // fire is never null in practice.
                    builder_.CreateBr(callBB);
                }

                // Call block: invoke the subroutine, then branch to the group
                // merge. After a sub runs, the program continues after END.
                builder_.SetInsertPoint(callBB);
                if (!c->result.empty()) {
                    auto it = subroutines_.find(c->result);
                    if (it == subroutines_.end()) {
                        report("input", c->lineno, 43, DiagKind::Error,
                               "CASxx subroutine '" + c->result +
                               "' has no matching BEGSR");
                    } else {
                        builder_.CreateCall(it->second);
                    }
                }
                builder_.CreateBr(casExit);

                prev = nextBB;
                continue;
            }

            // Regular op (incl. TAG/GOTO/arith).
            prev = emit_spec(main, prev, *c);
        }

        // Unmatched openers: report but keep going. Point at the opener's op
        // column for precision (H29).
        for (const auto &fr : frames) {
            if (fr.spec) {
                report("input", fr.spec->lineno, 28, DiagKind::Error,
                       std::string("unbalanced ") +
                       (fr.op == Op::IF ? "IFxx" :
                        fr.op == Op::DOW ? "DOWxx" :
                        fr.op == Op::DOU ? "DOUxx" :
                        fr.op == Op::DO ? "DO" :
                        fr.op == Op::CAS ? "CASxx" : "structured op") +
                       " (missing END)");
            }
        }
        // If this chain had a control-level gate, branch its fall-through into
        // the merge block and return the merge as the continuation.
        if (level_skip_) {
            builder_.SetInsertPoint(prev);
            builder_.CreateBr(level_skip_);
            llvm::BasicBlock *ret = level_skip_;
            level_skip_ = nullptr;
            return ret;
        }
        return prev;
    }

    /* Emit one C-spec. `prev` is the block to fall through from. Returns the
     * new "fall-through" block (the block after this spec).
     *
     * Special cases:
     *   TAG  -- the label's block is registered in tag_blocks_ so GOTOs (even
     *           forward ones) can branch to it; the block then continues as
     *           the "do" block for fall-through.
     *   GOTO -- instead of falling through to next_bb, branches unconditionally
     *           to the registered TAG block. */
    llvm::BasicBlock *emit_spec(llvm::Function *main,
                                llvm::BasicBlock *prev,
                                const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        std::string tag = std::to_string(c.lineno);

        // TAG: a position marker. Its block was pre-created in
        // emit_spec_chain (so forward GOTOs resolve). Wire prev into it and
        // continue emitting from there. No condition/do/next blocks needed.
        if (c.op == Op::TAG) {
            auto it = tag_blocks_.find(c.factor1);
            BasicBlock *tagbb = (it != tag_blocks_.end())
                                ? it->second
                                : BasicBlock::Create(ctx, "tag_" + c.factor1, main);
            tag_blocks_[c.factor1] = tagbb;
            builder_.SetInsertPoint(prev);
            builder_.CreateBr(tagbb);
            return tagbb;
        }

        BasicBlock *cond_bb = BasicBlock::Create(ctx, "spec" + tag, main);
        BasicBlock *do_bb   = BasicBlock::Create(ctx, "do" + tag, main);
        BasicBlock *next_bb = BasicBlock::Create(ctx, "after" + tag, main);

        // Link previous block into the condition test.
        builder_.SetInsertPoint(prev);
        builder_.CreateBr(cond_bb);

        // Condition test.
        builder_.SetInsertPoint(cond_bb);
        Value *ok = eval_conditions(inds_, builder_, c.conditions);
        builder_.CreateCondBr(ok, do_bb, next_bb);

        // Op body.
        builder_.SetInsertPoint(do_bb);
        bool branched_away = emit_op_body(c);   // true if op ended in a branch (GOTO)
        if (!branched_away) builder_.CreateBr(next_bb);

        return next_bb;
    }

    /* Emit the body of an operation into the current block. Returns true if
     * the op terminated the block with an explicit branch (so the caller must
     * not append a fall-through branch). */
    bool emit_op_body(const CSpec &c) {
        // MVR validity: the remainder only carries to the immediately following
        // op. Any op other than MVR after a DIV clears it.
        bool is_div = (c.op == Op::DIV);
        bool is_mvr = (c.op == Op::MVR);
        bool is_struct = (c.op == Op::IF || c.op == Op::DOW || c.op == Op::DOU ||
                          c.op == Op::DO || c.op == Op::ELSE ||
                          c.op == Op::END || c.op == Op::CAS);
        if (!is_div && !is_mvr && !is_struct) last_remainder_ = false;

        switch (c.op) {
            case Op::ADD:
            case Op::SUB:
            case Op::MULT:
            case Op::DIV:  emit_binop(c);  return false;
            case Op::MVR:  emit_mvr(c);    return false;
            case Op::ZADD: emit_zadd(c);   return false;
            case Op::ZSUB: emit_zsub(c);   return false;
            case Op::SETON:case Op::SETOF: emit_seton(c); return false;
            case Op::COMP:  emit_comp(c);  return false;
            case Op::MOVE:
            case Op::MOVEL: emit_move(c);  return false;
            case Op::TAG:   return false;   // marker; nothing to emit
            case Op::GOTO:  emit_goto(c);   return true;  // branches away
            case Op::EXSR:  emit_exsr(c);   return false;  // call subroutine
            case Op::EXCPT: emit_except(c); return false;  // write E-lines
            case Op::XFOOT: emit_xfoot(c);  return false;
            case Op::SQRT:  emit_sqrt(c);   return false;
            case Op::LOKUP: emit_lokup(c);  return false;
            case Op::MOVEA: emit_movea(c);  return false;
            case Op::TESTZ: emit_testz(c);  return false;
            case Op::TESTB: emit_testb(c);  return false;
            // Section G (G24) file access:
            case Op::CHAIN: emit_chain(c);  return false;
            case Op::SETLL: emit_setll(c);  return false;
            case Op::READ:  emit_read(c);   return false;
            case Op::READE: emit_reade(c);  return false;
            case Op::READP: emit_readp(c);  return false;
            // Group C: additional operation codes.
            case Op::BITON: emit_bit_set(c, true);  return false;
            case Op::BITOF: emit_bit_set(c, false); return false;
            case Op::DEFN:  emit_defn(c);  return false;
            case Op::SORTA: emit_sorta(c); return false;
            case Op::TIME:  emit_time(c);  return false;
            case Op::MHHZO: emit_movezone(c, true,  true,  "MHHZO"); return false;
            case Op::MHLZO: emit_movezone(c, true,  false, "MHLZO"); return false;
            case Op::MLHZO: emit_movezone(c, false, true,  "MLHZO"); return false;
            case Op::MLLZO: emit_movezone(c, false, false, "MLLZO"); return false;
            // Program linkage. PLIST/PARM/RLABL carry
            // no codegen of their own -- they're consumed by the PLIST/EXIT
            // grouping passes (cspec.cpp) and read back by emit_call/
            // emit_exit/emit_entry_plist_*; they fall through to `default`.
            case Op::CALL:  emit_call(c);   return false;
            case Op::EXIT:  emit_exit_op(c); return false;
            case Op::FREE:  emit_free(c);   return false;
            case Op::RETRN: {
                if (in_subroutine_) {
                    report("input", c.lineno, 28, DiagKind::Error,
                           "RETRN is not allowed inside a subroutine");
                    return false;
                }
                emit_entry_plist_copyout();
                builder_.CreateRet(emit_entry_return_value());
                return true;  // branched away, like GOTO
            }
            default:
                // Structured ops (IF/DOU/DOW/ELSE/END/CAS) are handled at the
                // chain level, not as individual op bodies. Unknown ops were
                // reported at parse time.
                return false;
        }
    }

    /* A compile-time power of ten as an i64 constant (1, 10, 100, ...). */
    llvm::Constant *pow10_i64(int n) {
        long v = 1;
        for (int i = 0; i < n; ++i) v *= 10;
        return llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(mod_->getContext()), (uint64_t)v, true);
    }

    /* Decimal positions of a factor token: 0 for literals/undeclared, the
     * field's recorded decimals otherwise. */
    int operand_decimals(const std::string &tok) {
        if (tok.empty()) return 0;
        // "ARR,INDEX" element refs and integer literals are scale-0.
        std::string an, it;
        if (syms_.parse_array_ref(tok, an, it)) return 0;
        return syms_.field_decimals(tok);
    }

    /* ADD/SUB/MULT/DIV share the "factor1 optional, result substitutes" rule.
     *
     * Section C: every numeric field is a scaled integer (stored value =
     * true value x 10^decimals). Arithmetic works in i64 with explicit decimal
     * alignment:
     *   ADD/SUB - align both operands to max(d1,d2), then adjust to result_dec.
     *   MULT    - product scale = d1+d2; adjust to result_dec.
     *   DIV     - scale the numerator up to gain precision before sdiv; adjust
     *             to result_dec.
     * Half-adjust (col 53 = H) rounds when digits are dropped. The remainder of
     * a DIV (unscaled integer remainder) is kept in last_remainder_ for MVR. */
    void emit_binop(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);

        const char *opname = "arith";
        switch (c.op) {
            case Op::ADD:  opname = "ADD";  break;
            case Op::SUB:  opname = "SUB";  break;
            case Op::MULT: opname = "MULT"; break;
            case Op::DIV:  opname = "DIV";  break;
            default: return;
        }
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   std::string(opname) + " requires a result field");
            return;
        }
        Value *f2 = syms_.resolve_operand(c.factor2);
        if (!f2) {
            report("input", c.lineno, 33, DiagKind::Error,
                   std::string(opname) + " requires factor 2");
            return;
        }
        Value *rptr = resolve_result_ptr(c);

        // Result decimal scale: explicit col 52 wins, else the field's own.
        int result_dec = c.result_dec >= 0 ? c.result_dec
                                            : syms_.field_decimals(c.result);
        // An inline-declared result field (col 52 present) records its scale.
        if (c.result_dec >= 0) syms_.set_numeric_attrs(c.result, c.result_dec);

        // Resolve operands as i64, tracking their scales.
        int d2 = operand_decimals(c.factor2);
        Value *f2_64 = builder_.CreateSExt(f2, i64, "f2_64");
        Value *lhs_64;
        int d1;
        if (!c.factor1.empty()) {
            Value *lhs = syms_.resolve_operand(c.factor1);
            lhs_64 = builder_.CreateSExt(lhs, i64, "f1_64");
            d1 = operand_decimals(c.factor1);
        } else {
            lhs_64 = builder_.CreateSExt(
                builder_.CreateLoad(i32, rptr, c.result + "_cur"), i64, "cur_64");
            d1 = syms_.field_decimals(c.result);
        }

        // Helper: scale a value from `from_dec` to `to_dec` by a power of ten
        // (multiply up, or truncating-divide down with optional half-adjust).
        auto scale_to = [&](Value *v, int from_dec, int to_dec,
                            bool ha, const char *tag) -> Value * {
            if (from_dec == to_dec) return v;
            if (to_dec > from_dec) {
                return builder_.CreateMul(v, pow10_i64(to_dec - from_dec), tag);
            }
            int drop = from_dec - to_dec;
            if (ha) {
                // half-adjust: add 5*10^(drop-1) before truncating.
                Value *bias = builder_.CreateMul(
                    pow10_i64(drop - 1),
                    llvm::ConstantInt::get(i64, 5, true), "ha_bias");
                // Sign-aware: add bias for positive, subtract for negative.
                Value *neg = builder_.CreateICmpSLT(v,
                    llvm::ConstantInt::get(i64, 0, true), "ha_neg");
                Value *adj = builder_.CreateSelect(neg,
                    builder_.CreateNeg(bias, "ha_negb"), bias, "ha_adj");
                v = builder_.CreateAdd(v, adj, "ha_v");
            }
            return builder_.CreateSDiv(v, pow10_i64(drop), tag);
        };

        Value *res_64;
        switch (c.op) {
            case Op::ADD:
            case Op::SUB: {
                int hi = std::max(d1, d2);
                Value *a = scale_to(lhs_64, d1, hi, false, "al");
                Value *b = scale_to(f2_64,  d2, hi, false, "bl");
                Value *sum = (c.op == Op::ADD)
                    ? builder_.CreateAdd(a, b, "add")
                    : builder_.CreateSub(a, b, "sub");
                res_64 = scale_to(sum, hi, result_dec, c.half_adjust, "rs");
                break;
            }
            case Op::MULT: {
                Value *prod = builder_.CreateMul(lhs_64, f2_64, "mul");
                res_64 = scale_to(prod, d1 + d2, result_dec, c.half_adjust, "rs");
                break;
            }
            case Op::DIV: {
                // Scale the numerator so the quotient carries result_dec
                // decimals (plus a couple of guard digits for half-adjust).
                int guard = c.half_adjust ? 1 : 0;
                int num_dec = result_dec + guard;
                Value *num = scale_to(lhs_64, d1, num_dec, false, "dn");
                // f2 has d2 decimals; dividing num (num_dec) by f2 (d2) yields
                // a quotient of scale (num_dec - d2).
                int q_dec = num_dec - d2;
                Value *quot = builder_.CreateSDiv(num, f2_64, "div");
                // The integer remainder for MVR is the true remainder of the
                // operand-as-written (scaled to result_dec).
                Value *lhs_res = scale_to(lhs_64, d1, result_dec, false, "lrd");
                Value *f2_res  = scale_to(f2_64,  d2, result_dec, false, "frd");
                Value *rem = builder_.CreateSRem(lhs_res, f2_res, "rem");
                builder_.CreateStore(
                    builder_.CreateTrunc(rem, i32, "rem32"), get_divrem_slot());
                last_remainder_ = true;
                last_remainder_dec_ = result_dec;
                res_64 = scale_to(quot, q_dec, result_dec, c.half_adjust, "rs");
                break;
            }
            default: return;
        }
        Value *res = builder_.CreateTrunc(res_64, i32, "res32");
        builder_.CreateStore(res, rptr);
        emit_arith_result_indicators(inds_, builder_, c, res);
    }

    /* MVR: move the remainder of the immediately preceding DIV into the result
     * field. No factor1/factor2. The remainder was stashed in a hidden global
     * by the DIV (see emit_binop), so we just load it back. */
    void emit_mvr(const CSpec &c) {
        using namespace llvm;
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "MVR requires a result field");
            return;
        }
        if (!last_remainder_) {
            report("input", c.lineno, 28, DiagKind::Error,
                   "MVR must immediately follow a DIV operation");
            return;
        }
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        Value *rem = builder_.CreateLoad(i32, get_divrem_slot(), "mvr_rem");
        last_remainder_ = false;   // consume: a later stray MVR is an error

        // A10: the remainder was stashed scaled to the preceding DIV's own
        // result_dec; rescale it to MVR's own result field's decimals (which
        // may differ, manual 123342-123367, Figure 306) before storing, the
        // same way Z-ADD/Z-SUB already rescale factor 2 via rescale_to_result.
        int to_dec = c.result_dec >= 0 ? c.result_dec
                                        : syms_.field_decimals(c.result);
        if (c.result_dec >= 0) syms_.set_numeric_attrs(c.result, c.result_dec);
        if (to_dec != last_remainder_dec_) {
            auto *i64 = Type::getInt64Ty(mod_->getContext());
            Value *rem64 = builder_.CreateSExt(rem, i64, "mvr_rem64");
            if (to_dec > last_remainder_dec_) {
                rem64 = builder_.CreateMul(rem64,
                    pow10_i64(to_dec - last_remainder_dec_), "mvr_rs_up");
            } else {
                rem64 = builder_.CreateSDiv(rem64,
                    pow10_i64(last_remainder_dec_ - to_dec), "mvr_rs_dn");
            }
            rem = builder_.CreateTrunc(rem64, i32, "mvr_rs");
        }

        Value *rptr = resolve_result_ptr(c);
        builder_.CreateStore(rem, rptr);
        emit_arith_result_indicators(inds_, builder_, c, rem);
    }

    /* Hidden global holding the remainder of the most recent DIV, for MVR. */
    llvm::GlobalVariable *get_divrem_slot() {
        if (!divrem_slot_) {
            divrem_slot_ = new llvm::GlobalVariable(
                *mod_, llvm::Type::getInt32Ty(mod_->getContext()),
                /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(mod_->getContext()),
                                       0),
                "rpg_divrem");
        }
        return divrem_slot_;
    }

    /* Rescale an i32 value (from operand `tok`'s decimal count) to the result
     * field's decimal count, returning an i32. Used by Z-ADD/Z-SUB. When the
     * result declares its own decimals (col 52) and the result field is being
     * freshly defined, record that scale on the field. Section C. */
    llvm::Value *rescale_to_result(llvm::Value *v, const std::string &tok,
                                   const CSpec &c) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        auto *i64 = Type::getInt64Ty(mod_->getContext());
        int from_dec = operand_decimals(tok);
        // A result declaring col 52 decimals defines the field's scale.
        int to_dec = c.result_dec >= 0 ? c.result_dec
                                        : syms_.field_decimals(c.result);
        if (c.result_dec >= 0) syms_.set_numeric_attrs(c.result, c.result_dec);
        if (from_dec == to_dec) return v;
        Value *v64 = builder_.CreateSExt(v, i64, "sc_in");
        if (to_dec > from_dec) {
            v64 = builder_.CreateMul(v64, pow10_i64(to_dec - from_dec), "sc_up");
        } else {
            v64 = builder_.CreateSDiv(v64, pow10_i64(from_dec - to_dec), "sc_dn");
        }
        return builder_.CreateTrunc(v64, i32, "sc_out");
    }

    /* Z-ADD: r = f2 (clears result first, i.e. result = 0 + f2). Factor 2 is
     * rescaled to the result field's decimal positions (Section C). */
    void emit_zadd(const CSpec &c) {
        using namespace llvm;
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "Z-ADD requires a result field");
            return;
        }
        Value *f2 = syms_.resolve_operand(c.factor2);
        if (!f2) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "Z-ADD requires factor 2");
            return;
        }
        Value *rptr = resolve_result_ptr(c);
        Value *v = rescale_to_result(f2, c.factor2, c);
        builder_.CreateStore(v, rptr);
        emit_arith_result_indicators(inds_, builder_, c, v);
    }

    /* Z-SUB: r = -f2 (zero and subtract). Same operands/result-indicators as
     * Z-ADD, but the value stored is the negation of factor 2. */
    void emit_zsub(const CSpec &c) {
        using namespace llvm;
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "Z-SUB requires a result field");
            return;
        }
        Value *f2 = syms_.resolve_operand(c.factor2);
        if (!f2) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "Z-SUB requires factor 2");
            return;
        }
        Value *neg = builder_.CreateNeg(f2, "zsub");
        Value *rptr = resolve_result_ptr(c);
        Value *v = rescale_to_result(neg, c.factor2, c);
        builder_.CreateStore(v, rptr);
        emit_arith_result_indicators(inds_, builder_, c, v);
    }

    /* SETON / SETOF: turn the named indicators on/off. Up to three, one per
     * resulting-indicator slot (HI/LO/EQ columns 54-59).
     *
     * B5 (manual 104980-104982): SETON must not set 1P, MR, L0, or KA-KY;
     * SETOF must not clear 1P, MR, L0, or LR. L0 (idx 0) is already excluded
     * by the `slot.indicator != 0` guard below (ind_token maps L0 to 0), so
     * only 1P(-11)/MR(-12)/KA-KY(-38..-62)/LR(-1) need an explicit check. */
    void emit_seton(const CSpec &c) {
        using namespace llvm;
        auto *ctx = &mod_->getContext();
        bool is_on = (c.op == Op::SETON);
        auto *v   = is_on ? ConstantInt::getTrue(*ctx)
                           : ConstantInt::getFalse(*ctx);
        for (ResultInd slot : {c.hi, c.lo, c.eq}) {
            int idx = slot.indicator;
            if (idx == 0) continue;
            if (idx == -11 || idx == -12) continue;              // 1P, MR
            if (is_on && idx <= -38 && idx >= -62) continue;      // KA-KY
            if (!is_on && idx == -1) continue;                    // LR
            inds_.store_resolved(idx, v);
        }
    }

    /* EXSR: call the subroutine named in factor2. The subroutine is compiled
     * as a separate void() LLVM function; globals (fields, indicators) are
     * shared, so no arguments are needed. */
    void emit_exsr(const CSpec &c) {
        auto it = subroutines_.find(c.factor2);
        if (it == subroutines_.end()) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "EXSR '" + c.factor2 + "' has no matching BEGSR");
            return;
        }
        builder_.CreateCall(it->second);
    }

    /* EXCPT: write type-E O-records at calculation time. `factor2` names an
     * EXCPT group; blank factor2 selects the unnamed E-records. Emits into the
     * current block (already gated by the C-spec's conditioning indicators via
     * emit_spec). Each E-record then re-applies its own line conditioning. */
    void emit_except(const CSpec &c) {
        if (!outputs_ || outputs_->empty()) return;
        // The current function is the one the spec chain is emitting into.
        llvm::Function *main = builder_.GetInsertBlock()->getParent();
        llvm::BasicBlock *prev = builder_.GetInsertBlock();
        prev = emit_exception(main, prev, *outputs_, c.factor2);
        builder_.SetInsertPoint(prev);
    }

    /* Generate one subroutine as a void() function. The body runs the specs
     * between BEGSR and ENDSR (exclusive). Returns at ENDSR. */
    void generate_subroutine(const std::string &name,
                             std::vector<const CSpec *>::const_iterator begin,
                             std::vector<const CSpec *>::const_iterator end) {
        using namespace llvm;
        auto &c = mod_->getContext();
        auto *voidTy = Type::getVoidTy(c);
        FunctionType *ft = FunctionType::get(voidTy, false);
        Function *fn = Function::Create(ft, Function::InternalLinkage,
                                        "sub_" + name, mod_.get());
        subroutines_[name] = fn;
        BasicBlock *entry = BasicBlock::Create(c, "entry", fn);
        builder_.SetInsertPoint(entry);

        std::vector<CSpec> body;
        for (auto it = begin; it != end; ++it) body.push_back(**it);
        bool saved_in_sub = in_subroutine_;
        in_subroutine_ = true;
        BasicBlock *after = emit_spec_chain(fn, entry, body, "");
        in_subroutine_ = saved_in_sub;
        builder_.SetInsertPoint(after);
        builder_.CreateRetVoid();
    }

    /* Scan all C-specs for BEGSR..ENDSR groups and compile each as a function.
     * Subroutine specs (control_level == "SR") are also recorded so the normal
     * chains skip them. Must run before the main body so EXSR can resolve. */
    void generate_subroutines(const std::vector<CSpec> &calcs) {
        // First, detect recursion (H28): build a call graph from each
        // subroutine's EXSR (factor2) and CASxx (result) targets, then DFS. RPG
        // II forbids direct or mutual recursion.
        struct SubRange { std::string name; size_t begin, end; };
        auto upperize = [](std::string s){ for (auto &ch : s) ch = (char)std::toupper((unsigned char)ch); return s; };
        std::vector<SubRange> subs;
        for (size_t i = 0; i < calcs.size(); ++i) {
            if (calcs[i].op != Op::BEGSR) continue;
            std::string name = upperize(calcs[i].factor1);
            size_t j;
            for (j = i + 1; j < calcs.size(); ++j)
                if (calcs[j].op == Op::ENDSR) break;
            subs.push_back({name, i + 1, j});
        }
        std::unordered_map<std::string, std::vector<std::string>> callees;
        for (const auto &sr : subs) {
            auto &out = callees[sr.name];
            for (size_t k = sr.begin; k < sr.end; ++k) {
                if (calcs[k].op == Op::EXSR && !calcs[k].factor2.empty())
                    out.push_back(upperize(calcs[k].factor2));
                else if (calcs[k].op == Op::CAS && !calcs[k].result.empty())
                    out.push_back(upperize(calcs[k].result));
            }
        }
        // DFS with an on-stack set; report any back-edge as recursion.
        std::unordered_map<std::string, int> state; // 0=unseen,1=on stack,2=done
        std::function<bool(const std::string &)> dfs = [&](const std::string &n) -> bool {
            state[n] = 1;
            auto it = callees.find(n);
            if (it != callees.end()) {
                for (const auto &callee : it->second) {
                    if (callee == n) {
                        report("input", 0, 0, DiagKind::Error,
                               "subroutine '" + n + "' is recursive (RPG II forbids recursion)");
                        return true;
                    }
                    if (callees.count(callee) && state[callee] == 1) {
                        report("input", 0, 0, DiagKind::Error,
                               "subroutine '" + n + "' / '" + callee +
                               "' are mutually recursive (RPG II forbids recursion)");
                        return true;
                    }
                    if (callees.count(callee) && state[callee] == 0 && dfs(callee))
                        return true;
                }
            }
            state[n] = 2;
            return false;
        };
        for (const auto &sr : subs)
            if (state[sr.name] == 0) dfs(sr.name);

        for (size_t i = 0; i < calcs.size(); ++i) {
            if (calcs[i].op == Op::BEGSR) {
                std::string name = calcs[i].factor1;
                // find matching ENDSR
                size_t j;
                for (j = i + 1; j < calcs.size(); ++j)
                    if (calcs[j].op == Op::ENDSR) break;
                if (j >= calcs.size()) {
                    report("input", calcs[i].lineno, 0, DiagKind::Error,
                           "subroutine '" + name + "' has no ENDSR");
                    return;
                }
                // Collect body specs as pointers for generate_subroutine.
                std::vector<const CSpec *> body;
                for (size_t k = i + 1; k < j; ++k) body.push_back(&calcs[k]);
                generate_subroutine(name, body.begin(), body.end());
                i = j;   // skip past the subroutine
            }
        }
    }

    /* GOTO: unconditional branch to the block registered for the TAG named in
     * factor2. The branch sits inside the current "do" block (already gated by
     * any conditioning indicators by emit_spec). */
    void emit_goto(const CSpec &c) {
        auto it = tag_blocks_.find(c.factor2);
        if (it == tag_blocks_.end()) {
            // Parse-time validation should prevent this; be defensive.
            report("input", c.lineno, 33, DiagKind::Error,
                   "GOTO target '" + c.factor2 + "' not found during codegen");
            return;
        }
        builder_.CreateBr(it->second);
    }

    /* CASxx resulting indicators: same three-way HI/LO/EQ semantics as COMP.
     * Used when a CASxx line names indicators in cols 54-59 (valid with or
     * without a subroutine in the result field). No-op if no factors. */
    void emit_cas_result_indicators(const CSpec &c) {
        using namespace llvm;
        if (c.hi.indicator == 0 && c.lo.indicator == 0 && c.eq.indicator == 0)
            return;
        if (c.factor1.empty() || c.factor2.empty()) return;
        auto set = [&](ResultInd slot, CmpOp cop) {
            if (slot.indicator == 0) return;
            CSpec tmp = c; tmp.cmp = cop;
            Value *cmp = eval_cmp_op(tmp);
            if (cmp) inds_.store_resolved(slot.indicator, cmp);
        };
        set(c.hi, CmpOp::GT);
        set(c.lo, CmpOp::LT);
        set(c.eq, CmpOp::EQ);
    }

    /* COMP: three-way compare factor1 vs factor2. Sets HI/LO/EQ (54-59).
     * Handles both numeric and character operands (Phase 10 added char). */
    void emit_comp(const CSpec &c) {
        using namespace llvm;
        // Determine whether this is a character or numeric compare so we can
        // issue the three independent comparisons.
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        Value *a, *b; int alen, blen;
        bool a_is_char = syms_.resolve_char_operand(c.factor1, a, alen);
        bool b_is_char = syms_.resolve_char_operand(c.factor2, b, blen);
        bool char_cmp = a_is_char && b_is_char;

        auto set = [&](ResultInd slot, CmpOp cop, const char *nm) {
            if (slot.indicator == 0) return;
            CSpec tmp = c; tmp.cmp = cop;
            Value *cmp = eval_cmp_op(tmp);
            if (cmp) inds_.store_resolved(slot.indicator, cmp);
        };
        set(c.hi, CmpOp::GT, "cmp_hi");
        set(c.lo, CmpOp::LT, "cmp_lo");
        set(c.eq, CmpOp::EQ, "cmp_eq");
        (void)char_cmp;
    }
    /* Build the i1 comparison of two operands per a CmpOp. Handles both numeric
     * (i32, signed) and character (left-aligned, blank-padded) operands. Returns
     * nullptr if an operand is missing. */
    llvm::Value *eval_cmp_op(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);

        // Try character operands first (if either factor is a char field/literal).
        Value *a, *b; int alen, blen;
        bool a_is_char = syms_.resolve_char_operand(c.factor1, a, alen);
        bool b_is_char = syms_.resolve_char_operand(c.factor2, b, blen);
        // If both are character-typed, do a string compare via the runtime.
        if (a_is_char && b_is_char) {
            if (!cmp_str_) {
                cmp_str_ = Function::Create(
                    FunctionType::get(i32, {PointerType::get(ctx,0), i32,
                                            PointerType::get(ctx,0), i32}, false),
                    Function::ExternalLinkage, "rpg_rt_cmp_str", mod_.get());
            }
            Value *r = builder_.CreateCall(cmp_str_,
                {a, ConstantInt::get(i32, alen, true),
                 b, ConstantInt::get(i32, blen, true)}, "ccmp");
            ICmpInst::Predicate p;
            switch (c.cmp) {
                case CmpOp::EQ: p = ICmpInst::ICMP_EQ;  break;
                case CmpOp::NE: p = ICmpInst::ICMP_NE;  break;
                case CmpOp::GT: p = ICmpInst::ICMP_SGT; break;
                case CmpOp::LT: p = ICmpInst::ICMP_SLT; break;
                case CmpOp::GE: p = ICmpInst::ICMP_SGE; break;
                case CmpOp::LE: p = ICmpInst::ICMP_SLE; break;
                default: return nullptr;
            }
            return builder_.CreateICmp(p, r, ConstantInt::get(i32, 0), "ccmp_r");
        }

        // Numeric compare.
        Value *f1 = syms_.resolve_operand(c.factor1);
        Value *f2 = syms_.resolve_operand(c.factor2);
        if (!f1 || !f2) {
            report("input", c.lineno, 0, DiagKind::Error,
                   std::string(c.op_text) + " requires both factor 1 and factor 2");
            return nullptr;
        }
        // Align both operands at their implied decimal point before comparing
        // (manual 104817-104822): a raw icmp on differently-scaled storage
        // would compare e.g. 1.50 (stored 150, 2 decimals) against 2 (stored
        // 2, 0 decimals) as 150 > 2 instead of the correct 1.50 < 2.
        int d1 = operand_decimals(c.factor1);
        int d2 = operand_decimals(c.factor2);
        Value *lhs = f1, *rhs = f2;
        if (d1 != d2) {
            auto *i64 = Type::getInt64Ty(ctx);
            int hi = std::max(d1, d2);
            Value *lhs64 = builder_.CreateSExt(f1, i64, "cmp_a64");
            Value *rhs64 = builder_.CreateSExt(f2, i64, "cmp_b64");
            if (hi > d1) lhs64 = builder_.CreateMul(lhs64, pow10_i64(hi - d1), "cmp_al");
            if (hi > d2) rhs64 = builder_.CreateMul(rhs64, pow10_i64(hi - d2), "cmp_bl");
            lhs = lhs64; rhs = rhs64;
        }
        ICmpInst::Predicate p;
        switch (c.cmp) {
            case CmpOp::EQ: p = ICmpInst::ICMP_EQ;  break;
            case CmpOp::NE: p = ICmpInst::ICMP_NE;  break;
            case CmpOp::GT: p = ICmpInst::ICMP_SGT; break;
            case CmpOp::LT: p = ICmpInst::ICMP_SLT; break;
            case CmpOp::GE: p = ICmpInst::ICMP_SGE; break;
            case CmpOp::LE: p = ICmpInst::ICMP_SLE; break;
            default: return nullptr;
        }
        return builder_.CreateICmp(p, lhs, rhs, "cmp");
    }

    /* MOVE / MOVEL: copy factor2 into the result field.
     *
     * Phase 4 implements the numeric case (the common one for our i32 model):
     * factor2's value is stored into the result field. MOVE is right-justified
     * and MOVEL left-justified, but for integer values both amount to an
     * assignment at this precision; the difference matters for character
     * fields (alphanumeric MOVE), which arrive with packed/character support.
     * Result length/decimals (cols 49-52) are noted but do not change the i32
     * store in this phase. MOVE sets no resulting indicators (manual p.716). */
    void emit_move(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i8  = Type::getInt8Ty(ctx);
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "MOVE requires a result field");
            return;
        }

        // Resolve factor2 as char bytes if it is a char field / literal.
        Value *srcp; int srclen;
        bool f2_char = syms_.resolve_char_operand(c.factor2, srcp, srclen);

        // Decide whether the result is numeric. It is numeric when it is an
        // already-declared numeric field, or when the C-spec declares it so
        // (col 52 decimals present, or col 49-51 length with no decimal blank).
        bool result_numeric = syms_.has_field(c.result)
                              && !syms_.is_char_field(c.result);
        if (!syms_.has_field(c.result) && c.result_dec >= 0) result_numeric = true;

        // --- char factor2 -> numeric result: sign-overpunch decode (C10) ---
        if (f2_char && result_numeric) {
            Value *v64 = builder_.CreateCall(overpunch_in_,
                {srcp, ConstantInt::get(i32, srclen, true)}, c.result+"_op");
            Value *v32 = builder_.CreateTrunc(v64, i32, c.result+"_op32");
            // Record the result's scale and store.
            int rdec = c.result_dec >= 0 ? c.result_dec
                                          : syms_.field_decimals(c.result);
            if (c.result_dec >= 0) syms_.set_numeric_attrs(c.result, c.result_dec);
            Value *rptr = syms_.get_or_create_field(c.result);
            // The decoded digits are already in the result field's scale (an
            // N-digit zoned string maps to N integer digits).
            (void)rdec;
            builder_.CreateStore(v32, rptr);
            return;
        }

        // --- character -> character MOVE/MOVEL: right/left justified copy ---
        if (f2_char) {
            // D2: go through resolve_char_operand for the destination pointer
            // (rather than building a GEP off a freshly-fetched GlobalVariable
            // directly) so a data-structure subfield result -- whose storage
            // is an aliased byte range into its parent DS buffer, not its own
            // global -- resolves correctly instead of hitting a null gv.
            if (!syms_.is_char_field(c.result)) {
                int newlen = c.result_len > 0 ? c.result_len : srclen;
                syms_.get_or_create_char_field(c.result, newlen);
            }
            Value *dstp; int dstlen;
            if (!syms_.resolve_char_operand(c.result, dstp, dstlen)) {
                report("input", c.lineno, 43, DiagKind::Error,
                       std::string(c.op == Op::MOVEL ? "MOVEL" : "MOVE") +
                       " result field must be alphameric");
                return;
            }
            int copylen = std::min(srclen, dstlen);
            int srcoff = 0, dstoff = 0;
            if (c.op == Op::MOVE) {
                dstoff = dstlen - copylen;
                srcoff = srclen - copylen;
            }
            Value *d = builder_.CreateInBoundsGEP(i8, dstp,
                ConstantInt::get(i32, dstoff, true));
            Value *s = builder_.CreateInBoundsGEP(i8, srcp,
                ConstantInt::get(i32, srcoff, true));
            builder_.CreateMemCpy(d, MaybeAlign(), s, MaybeAlign(),
                                  (unsigned)copylen);
            return;
        }

        // --- numeric factor2 -> character result: sign-overpunch encode (C10) -
        bool result_char = syms_.is_char_field(c.result)
                           || (!syms_.has_field(c.result) && c.result_dec < 0
                               && c.result_len > 0);
        Value *f2 = syms_.resolve_operand(c.factor2);
        if (result_char) {
            if (!f2) {
                report("input", c.lineno, 33, DiagKind::Error,
                       std::string(c.op == Op::MOVEL ? "MOVEL" : "MOVE") +
                       " requires factor 2");
                return;
            }
            // D2: same resolve_char_operand rationale as the char->char branch
            // above -- required for a DS subfield result.
            if (!syms_.is_char_field(c.result)) {
                int newlen = c.result_len > 0 ? c.result_len : 1;
                syms_.get_or_create_char_field(c.result, newlen);
            }
            Value *dstp; int dstlen;
            syms_.resolve_char_operand(c.result, dstp, dstlen);
            Value *v64 = builder_.CreateSExt(f2, i64, c.result+"_oe");
            builder_.CreateCall(overpunch_out_,
                {v64, dstp, ConstantInt::get(i32, dstlen, true)}, c.result+"_o");
            return;
        }

        // --- numeric MOVE: simple value store (Phase 4 behaviour) ---
        if (!f2) {
            report("input", c.lineno, 33, DiagKind::Error,
                   std::string(c.op == Op::MOVEL ? "MOVEL" : "MOVE") +
                   " requires factor 2");
            return;
        }
        Value *rptr = syms_.get_or_create_field(c.result);
        builder_.CreateStore(f2, rptr);
    }

    /* Resolve a result-field token to an i32* store target. Handles plain
     * fields and "ARR,INDEX" element refs. */
    llvm::Value *resolve_result_ptr(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        std::string an, it_tok;
        if (syms_.parse_array_ref(c.result, an, it_tok)) {
            const FieldInfo *fi = syms_.info(an);
            if (fi && fi->is_char_array) {
                // A9: a numeric store target ("ARR,INDEX") doesn't apply to
                // an alphameric array; report clearly instead of building a
                // type-mismatched GEP against the real [count x [len x i8]]
                // storage.
                report("input", c.lineno, 43, DiagKind::Error,
                       "cannot store a numeric result into alphameric array/table '"
                       + an + "'");
                return builder_.CreateAlloca(i32, nullptr, "bad_char_arr_result");
            }
            auto *arrTy = ArrayType::get(i32, (unsigned)fi->array_count);
            Value *idx;
            bool isLit = !it_tok.empty() && std::all_of(it_tok.begin(), it_tok.end(),
                [](unsigned char ch){ return std::isdigit(ch)||ch=='-'||ch=='+'; });
            if (isLit) idx = ConstantInt::get(i32, (uint32_t)(std::stoi(it_tok)-1), true);
            else {
                Value *fv = syms_.resolve_operand(it_tok);
                idx = fv ? builder_.CreateSub(fv, ConstantInt::get(i32,1,true), "ridx0")
                         : ConstantInt::get(i32, 0);
            }
            return builder_.CreateInBoundsGEP(arrTy, fi->gv,
                {ConstantInt::get(i32,0), idx}, an+"_rp");
        }
        // A bare table name as a result target: store into the current element.
        if (auto *tep = syms_.table_elem_ptr(c.result)) return tep;
        return syms_.get_or_create_field(c.result);
    }

    /* XFOOT: sum all elements of the numeric array named in factor2 into the
     * result field. Numeric only (Phase 9). */
    void emit_xfoot(const CSpec &c) {
        using namespace llvm;
        if (!syms_.is_array(c.factor2) || c.result.empty()) {
            report("input", c.lineno, 0, DiagKind::Error,
                   "XFOOT requires an array in factor 2 and a result field");
            return;
        }
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        const FieldInfo *fi = syms_.info(c.factor2);
        if (fi && fi->is_char_array) {
            // A9: XFOOT sums a numeric array; alphameric arrays have no
            // numeric total.
            report("input", c.lineno, 33, DiagKind::Error,
                   "XFOOT requires a numeric array in factor 2");
            return;
        }
        auto *arrTy = ArrayType::get(i32, (unsigned)fi->array_count);
        Value *sum = ConstantInt::get(i32, 0);
        for (int e = 0; e < fi->array_count; ++e) {
            Value *p = builder_.CreateInBoundsGEP(arrTy, fi->gv,
                {ConstantInt::get(i32,0), ConstantInt::get(i32, e)}, "xf_p");
            Value *v = builder_.CreateLoad(i32, p, "xf_v");
            sum = builder_.CreateAdd(sum, v, "xf_s");
        }
        builder_.CreateStore(sum, resolve_result_ptr(c));
        emit_arith_result_indicators(inds_, builder_, c, sum);
    }

    /* SQRT: square root of factor2 into result (Phase 9). Uses integer sqrt
     * via the runtime; half-adjusted per the manual. */
    void emit_sqrt(const CSpec &c) {
        using namespace llvm;
        // Use LLVM's intrinsic for integer sqrt if factor2 fits; for i32 we
        // emit an iterative integer sqrt via a small loop-free expression.
        // Simpler: cast to double, call sqrt, trunc. Half-adjusted.
        Value *f2 = syms_.resolve_operand(c.factor2);
        if (!f2) { report("input", c.lineno, 33, DiagKind::Error,
                          "SQRT requires factor 2"); return; }
        auto &ctx = mod_->getContext();
        Value *d = builder_.CreateSIToFP(f2, Type::getDoubleTy(ctx), "sq_d");
        // declare double @sqrt(double) if not yet
        if (!sqrt_fn_) {
            sqrt_fn_ = Function::Create(
                FunctionType::get(Type::getDoubleTy(ctx),
                                  {Type::getDoubleTy(ctx)}, false),
                Function::ExternalLinkage, "sqrt", mod_.get());
        }
        Value *r = builder_.CreateCall(sqrt_fn_, {d}, "sq_r");
        // half-adjust: floor(r + 0.5)
        Value *r1 = builder_.CreateFAdd(r, ConstantFP::get(Type::getDoubleTy(ctx), 0.5), "sq_h");
        Value *i = builder_.CreateFPToSI(r1, Type::getInt32Ty(ctx), "sq_i");
        builder_.CreateStore(i, resolve_result_ptr(c));
    }

    /* ----- Section G (G24): keyed / random file access --------------------- */

    /* Resolve the file id named in `fname` from the input-files map (set up by
     * open_input_files). Reports an error and returns nullptr if unknown. */
    llvm::Value *resolve_input_file(const CSpec &c, const std::string &fname) {
        auto it = in_ids_.find(fname);
        if (it == in_ids_.end()) {
            report("input", c.lineno, 33, DiagKind::Error,
                   std::string(c.op == Op::CHAIN ? "CHAIN" :
                               c.op == Op::SETLL ? "SETLL" :
                               c.op == Op::READE ? "READE" :
                               c.op == Op::READP ? "READP" : "READ")
                   + " file '" + fname + "' is not a keyed/random input file");
            return nullptr;
        }
        return it->second;
    }

    /* Resolve factor1 as a key operand. Returns a (ptr, len) pair: for a
     * character field, its bytes; for a numeric field/literal, a freshly-built
     * decimal-string buffer (the runtime compares raw key bytes). `width_hint`
     * (the target file's key length, or 0) sizes a numeric key's format width. */
    llvm::Value *emit_key_ptr(const CSpec &c, const std::string &tok,
                              int &len_out, int width_hint = 0) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);
        auto *i8  = Type::getInt8Ty(ctx);
        // Character field: use its bytes directly.
        Value *ptr; int plen = 0;
        if (syms_.resolve_char_operand(tok, ptr, plen)) {
            len_out = plen;
            return ptr;
        }
        // Numeric literal or field: format as a zero-padded decimal string
        // right-justified to the key width (from the file's key length).
        Value *val = syms_.resolve_operand(tok);
        if (!val) { len_out = 0; return nullptr; }
        int width = width_hint > 0 ? width_hint : 8;
        Value *buf = builder_.CreateAlloca(i8, ConstantInt::get(i32, width + 1, false), "keybuf");
        Value *v64 = builder_.CreateSExt(val, Type::getInt64Ty(ctx), "key_i64");
        Function *fmt_key = mod_->getFunction("rpg_rt_fmt_key");
        if (!fmt_key) {
            fmt_key = Function::Create(
                FunctionType::get(i32, {i64, i32, PointerType::get(ctx, 0)}, false),
                Function::ExternalLinkage, "rpg_rt_fmt_key", mod_.get());
        }
        builder_.CreateCall(fmt_key,
            {v64, ConstantInt::get(i32, width, true), buf}, "key_fmt");
        len_out = width;
        return buf;
    }

    /* Look up the key length declared for input file `fname` (F-spec cols
     * 29-30), or 0 if none. */
    int file_key_len(const std::string &fname) const {
        if (!files_) return 0;
        for (const auto &f : *files_) if (f.name == fname) return f.key_len;
        return 0;
    }

    /* CHAIN: random read of one record. factor1 = key or RRN, factor2 = file.
     * On success the record is decoded into that file's I-spec fields; the
     * cols 54-55 indicator (c.hi) turns on if no record was found. */
    void emit_chain(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);
        Function *fn = builder_.GetInsertBlock()->getParent();
        Value *fid = resolve_input_file(c, c.factor2);
        if (!fid) return;
        int keylen = 0;
        Value *keyp = emit_key_ptr(c, c.factor1, keylen, file_key_len(c.factor2));
        if (!keyp) {
            report("input", c.lineno, 18, DiagKind::Error,
                   "CHAIN factor1 '" + c.factor1 + "' is not a valid key operand");
            return;
        }
        // A buffer to receive the chained record.
        int rlen = 80;
        if (files_) for (const auto &f : *files_) if (f.name == c.factor2) rlen = f.reclen > 0 ? f.reclen : 80;
        Value *buf = builder_.CreateAlloca(Type::getInt8Ty(ctx),
            ConstantInt::get(i32, rlen + 1, false), "chain_buf");
        // E1: gated behind factor2's U1-U8 conditioning indicator.
        Value *found = emit_conditioned_bool(fn, c.factor2, [&]() -> Value* {
            Value *r = builder_.CreateCall(chain_,
                {fid, keyp, ConstantInt::get(i32, keylen, true), buf,
                 ConstantInt::get(i64, rlen + 1, false)}, "chain_found");
            return builder_.CreateICmpNE(r, ConstantInt::get(i32, 0), "chain_ok");
        });
        // On success, decode the file's fields from buf (like the cycle extract).
        BasicBlock *doDec = BasicBlock::Create(ctx, "chain_dec", fn);
        BasicBlock *after = BasicBlock::Create(ctx, "chain_after", fn);
        builder_.CreateCondBr(found, doDec, after);
        builder_.SetInsertPoint(doDec);
        decode_file_fields(c.factor2, buf, rlen);
        builder_.CreateBr(after);
        builder_.SetInsertPoint(after);
        // No-record indicator (cols 54-55): on when NOT found.
        if (c.hi.indicator)
            inds_.store_resolved(c.hi.indicator, builder_.CreateNot(found, "chain_nf"));
    }

    /* SETLL: position file (factor2) at the first key >= factor1. */
    void emit_setll(const CSpec &c) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        Value *fid = resolve_input_file(c, c.factor2);
        if (!fid) return;
        int keylen = 0;
        Value *keyp = emit_key_ptr(c, c.factor1, keylen, file_key_len(c.factor2));
        if (!keyp) {
            report("input", c.lineno, 18, DiagKind::Error,
                   "SETLL factor1 '" + c.factor1 + "' is not a valid key operand");
            return;
        }
        // E1: skip the position call entirely when factor2's U1-U8
        // conditioning indicator is off.
        Value *condOk = file_cond_ok(c.factor2);
        if (auto *cst = dyn_cast<ConstantInt>(condOk); cst && cst->isOne()) {
            builder_.CreateCall(setll_,
                {fid, keyp, ConstantInt::get(i32, keylen, true)});
            return;
        }
        Function *fn = builder_.GetInsertBlock()->getParent();
        auto &ctx = mod_->getContext();
        BasicBlock *doB = BasicBlock::Create(ctx, "setll_do", fn);
        BasicBlock *after = BasicBlock::Create(ctx, "setll_after", fn);
        builder_.CreateCondBr(condOk, doB, after);
        builder_.SetInsertPoint(doB);
        builder_.CreateCall(setll_,
            {fid, keyp, ConstantInt::get(i32, keylen, true)});
        builder_.CreateBr(after);
        builder_.SetInsertPoint(after);
    }

    /* READ: read the next record from file (factor2). On success decode the
     * file's fields; cols 58-59 indicator (c.eq) turns on at EOF. */
    void emit_read(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);
        Function *fn = builder_.GetInsertBlock()->getParent();
        Value *fid = resolve_input_file(c, c.factor2);
        if (!fid) return;
        int rlen = 80;
        if (files_) for (const auto &f : *files_) if (f.name == c.factor2) rlen = f.reclen > 0 ? f.reclen : 80;
        Value *buf = builder_.CreateAlloca(Type::getInt8Ty(ctx),
            ConstantInt::get(i32, rlen + 1, false), "read_buf");
        // E1: gated behind factor2's U1-U8 conditioning indicator.
        Value *got = emit_conditioned_bool(fn, c.factor2, [&]() -> Value* {
            Value *r = builder_.CreateCall(read_op_,
                {fid, buf, ConstantInt::get(i64, rlen + 1, false)}, "read_got");
            return builder_.CreateICmpNE(r, ConstantInt::get(i32, 0), "read_ok");
        });
        BasicBlock *doDec = BasicBlock::Create(ctx, "read_dec", fn);
        BasicBlock *after = BasicBlock::Create(ctx, "read_after", fn);
        builder_.CreateCondBr(got, doDec, after);
        builder_.SetInsertPoint(doDec);
        decode_file_fields(c.factor2, buf, rlen);
        builder_.CreateBr(after);
        builder_.SetInsertPoint(after);
        // EOF indicator (cols 58-59): on when no record read.
        if (c.eq.indicator)
            inds_.store_resolved(c.eq.indicator, builder_.CreateNot(got, "read_eof"));
    }

    /* READE: read next from file (factor2) only if its key == factor1; else
     * cols 58-59 indicator (c.eq) turns on (unequal / EOF). */
    void emit_reade(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);
        Function *fn = builder_.GetInsertBlock()->getParent();
        Value *fid = resolve_input_file(c, c.factor2);
        if (!fid) return;
        int keylen = 0;
        Value *keyp = emit_key_ptr(c, c.factor1, keylen, file_key_len(c.factor2));
        if (!keyp) {
            report("input", c.lineno, 18, DiagKind::Error,
                   "READE factor1 '" + c.factor1 + "' is not a valid key operand");
            return;
        }
        int rlen = 80;
        if (files_) for (const auto &f : *files_) if (f.name == c.factor2) rlen = f.reclen > 0 ? f.reclen : 80;
        Value *buf = builder_.CreateAlloca(Type::getInt8Ty(ctx),
            ConstantInt::get(i32, rlen + 1, false), "reade_buf");
        // E1: gated behind factor2's U1-U8 conditioning indicator.
        Value *got = emit_conditioned_bool(fn, c.factor2, [&]() -> Value* {
            Value *r = builder_.CreateCall(reade_,
                {fid, keyp, ConstantInt::get(i32, keylen, true), buf,
                 ConstantInt::get(i64, rlen + 1, false)}, "reade_got");
            return builder_.CreateICmpNE(r, ConstantInt::get(i32, 0), "reade_ok");
        });
        BasicBlock *doDec = BasicBlock::Create(ctx, "reade_dec", fn);
        BasicBlock *after = BasicBlock::Create(ctx, "reade_after", fn);
        builder_.CreateCondBr(got, doDec, after);
        builder_.SetInsertPoint(doDec);
        decode_file_fields(c.factor2, buf, rlen);
        builder_.CreateBr(after);
        builder_.SetInsertPoint(after);
        if (c.eq.indicator)
            inds_.store_resolved(c.eq.indicator, builder_.CreateNot(got, "reade_ne"));
    }

    /* READP: read the prior record from file (factor2), moving the read
     * cursor backward -- the mirror image of READ (manual 123813-123840). On
     * success decode the file's fields; cols 58-59 indicator (c.eq) turns on
     * when no prior record exists (beginning-of-file). */
    void emit_readp(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);
        Function *fn = builder_.GetInsertBlock()->getParent();
        Value *fid = resolve_input_file(c, c.factor2);
        if (!fid) return;
        int rlen = 80;
        if (files_) for (const auto &f : *files_) if (f.name == c.factor2) rlen = f.reclen > 0 ? f.reclen : 80;
        Value *buf = builder_.CreateAlloca(Type::getInt8Ty(ctx),
            ConstantInt::get(i32, rlen + 1, false), "readp_buf");
        // E1: gated behind factor2's U1-U8 conditioning indicator.
        Value *got = emit_conditioned_bool(fn, c.factor2, [&]() -> Value* {
            Value *r = builder_.CreateCall(readp_,
                {fid, buf, ConstantInt::get(i64, rlen + 1, false)}, "readp_got");
            return builder_.CreateICmpNE(r, ConstantInt::get(i32, 0), "readp_ok");
        });
        BasicBlock *doDec = BasicBlock::Create(ctx, "readp_dec", fn);
        BasicBlock *after = BasicBlock::Create(ctx, "readp_after", fn);
        builder_.CreateCondBr(got, doDec, after);
        builder_.SetInsertPoint(doDec);
        decode_file_fields(c.factor2, buf, rlen);
        builder_.CreateBr(after);
        builder_.SetInsertPoint(after);
        // Beginning-of-file indicator (cols 58-59): on when no prior record.
        if (c.eq.indicator)
            inds_.store_resolved(c.eq.indicator, builder_.CreateNot(got, "readp_bof"));
    }

    /* Decode the I-spec fields belonging to `file` from record buffer `buf`
     * (shared by CHAIN/READ/READE). Mirrors the cycle's extract for one file.
     * Unlike the implicit cycle's automatic read, these are explicit
     * calc-time procedural ops (manual step 26) with no later "total time" to
     * defer to, so field indicators are set immediately after extraction. */
    void decode_file_fields(const std::string &file, llvm::Value *buf, int reclen) {
        auto &c = mod_->getContext();
        if (!in_fields_) return;
        for (const auto &fld : *in_fields_) {
            if (fld.name.empty() || fld.file != file) continue;
            emit_one_input_field(c, buf, fld, reclen);
        }
        emit_field_indicators_for(*in_fields_, &file);
    }

    /* LOKUP: search the array named in factor2 for factor1's value. Sets HI/LO/
     * EQ indicators per the result code from rpg_rt_lokup. If factor2 is
     * "ARR,INDEX", the index field is updated on a match (result code 0). */
    void emit_lokup(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        auto *i64 = Type::getInt64Ty(ctx);

        Value *key = syms_.resolve_operand(c.factor1);
        if (!key) { report("input", c.lineno, 18, DiagKind::Error,
                           "LOKUP requires factor 1"); return; }
        // factor2 = array name, optionally with ,INDEX
        std::string an, it_tok;
        bool has_idx = syms_.parse_array_ref(c.factor2, an, it_tok);
        if (!has_idx) an = c.factor2;
        if (!syms_.is_array(an)) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "LOKUP requires an array in factor 2");
            return;
        }
        const FieldInfo *fi = syms_.info(an);
        if (fi && fi->is_char_array) {
            // A9 creates real storage for alphameric arrays/tables so they're
            // no longer unfindable/silently misbound, but a byte-compare
            // LOKUP (as opposed to this numeric one) isn't implemented yet.
            report("input", c.lineno, 33, DiagKind::Error,
                   "LOKUP against an alphameric array/table is not supported");
            return;
        }
        // Build the runtime call: rpg_rt_lokup(key, arrptr, count, &idx, asc)
        if (!lokup_fn_) {
            lokup_fn_ = Function::Create(
                FunctionType::get(i32,
                    {i64, PointerType::get(i32,0), i32, PointerType::get(i32,0), i32},
                    false),
                Function::ExternalLinkage, "rpg_rt_lokup", mod_.get());
        }
        auto *arrTy = ArrayType::get(i32, (unsigned)fi->array_count);
        Value *arrp = builder_.CreateConstInBoundsGEP2_32(arrTy,
            cast<GlobalVariable>(fi->gv), 0, 0, an+"_lp");
        Value *key64 = builder_.CreateSExt(key, i64, "lk_key");
        // index: if "ARR,INDEX", pass &indexfield (1-based); else a temp of 1.
        Value *idxp;
        if (has_idx && !it_tok.empty()) {
            idxp = resolve_result_ptr_for_token(it_tok);
            if (!idxp) idxp = builder_.CreateAlloca(i32, nullptr, "lk_i");
        } else {
            idxp = builder_.CreateAlloca(i32, nullptr, "lk_i");
            builder_.CreateStore(ConstantInt::get(i32, 1), idxp);
        }
        // A11: HI/LO "nearest" semantics need the array's declared sequence
        // (E-spec column 45, B2). Default to ascending when unspecified --
        // the manual requires an explicit A/D for a HI/LO LOKUP to be valid
        // at all, so this only affects the (already out-of-spec) case where
        // one is missing.
        bool ascending = true;
        if (arrays_) {
            for (const auto &a : *arrays_) {
                if (a.name == an) { ascending = a.ascending; break; }
                if (a.alt_name == an) { ascending = a.alt_ascending; break; }
            }
        }
        Value *rc = builder_.CreateCall(lokup_fn_,
            {key64, arrp, ConstantInt::get(i32, fi->array_count, true), idxp,
             ConstantInt::get(i32, ascending ? 1 : 0, true)},
            "lk_rc");
        // rc: 0=equal, +1=higher, -1=lower, -2=nothing.
        auto setind = [&](ResultInd slot, int want, const char *nm) {
            if (slot.indicator == 0) return;
            Value *eq = builder_.CreateICmpEQ(rc, ConstantInt::get(i32, want), nm);
            inds_.store_resolved(slot.indicator, eq);
        };
        setind(c.eq, 0,  "lk_eq");
        setind(c.hi, 1,  "lk_hi");
        setind(c.lo, -1, "lk_lo");

        // Tables (bare table name in factor 2, no explicit index): on an equal
        // match the table's current-element shadow is updated to the matched
        // element. If the result field names a *different* table (a related
        // table), its shadow advances in lockstep so its corresponding element
        // is now "current". rc==0 means an equal element was found.
        if (!has_idx && syms_.is_table(an)) {
            Value *found = builder_.CreateICmpEQ(rc, ConstantInt::get(i32, 0), "lk_f");
            Value *midx  = builder_.CreateLoad(i32, idxp, "lk_mi");
            if (GlobalVariable *sh = syms_.table_shadow(an)) {
                Value *old = builder_.CreateLoad(i32, sh, an+"_so");
                Value *nw  = builder_.CreateSelect(found, midx, old, an+"_sn");
                builder_.CreateStore(nw, sh);
            }
            if (!c.result.empty() && c.result != an && syms_.is_table(c.result)) {
                if (GlobalVariable *rsh = syms_.table_shadow(c.result)) {
                    Value *old = builder_.CreateLoad(i32, rsh, c.result+"_so");
                    Value *nw  = builder_.CreateSelect(found, midx, old,
                                                       c.result+"_sn");
                    builder_.CreateStore(nw, rsh);
                }
            }
        }
    }

    /* MOVEA: left-justified move of factor2's bytes into the result. Treats
     * operands as raw bytes; copy length = min(src, dst). Works for
     * field->field, array->field, field->array. Phase 10. */
    void emit_movea(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        if (c.result.empty()) { report("input", c.lineno, 43, DiagKind::Error,
                                       "MOVEA requires a result field"); return; }
        // Resolve source bytes.
        Value *sp; int slen;
        bool s_char = syms_.resolve_char_operand(c.factor2, sp, slen);
        if (!s_char) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "MOVEA requires a character factor 2");
            return;
        }
        // Ensure result is a character field; if not declared, size it to the
        // source length (MOVEA's destination gets the source's width).
        if (!syms_.is_char_field(c.result))
            syms_.get_or_create_char_field(c.result, slen);
        Value *dp; int dlen;
        syms_.resolve_char_operand(c.result, dp, dlen);
        int n = std::min(slen, dlen);
        builder_.CreateMemCpy(dp, MaybeAlign(), sp, MaybeAlign(), (unsigned)n);
    }

    /* TESTZ: test the "zone" of the leftmost character of the result field
     * (which must be alphameric). Sets HI on a plus zone, LO on a minus zone,
     * EQ otherwise. The manual defines the zones by EBCDIC high-nibble; since
     * this is an ASCII compiler there is no EBCDIC zone, so we honor the
     * explicit character sets the manual names (plus lowercase as an ASCII
     * extension, treating a letter's case as sharing its zone):
     *   plus  zone:  '&'  A-I (and a-i)
     *   minus zone:  '-'  J-R (and j-r)
     *   zero  zone:  anything else
     * Factor 1 and factor 2 are not used. */
    void emit_testz(const CSpec &c) {
        using namespace llvm;
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "TESTZ requires a result field");
            return;
        }
        // Result must be a character field.
        if (!syms_.is_char_field(c.result)) {
            // If undeclared, treat as a 1-position character field.
            syms_.get_or_create_char_field(c.result, 1);
        }
        Value *rp; int rlen;
        if (!syms_.resolve_char_operand(c.result, rp, rlen)) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "TESTZ result field must be alphameric");
            return;
        }
        auto *i8  = Type::getInt8Ty(mod_->getContext());
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        // Load byte[0] and zero-extend to i32 for range tests.
        Value *b0 = builder_.CreateLoad(i8, rp, "tz_b");
        Value *ch = builder_.CreateZExt(b0, i32, "tz_ch");

        auto inRange = [&](int lo, int hi) -> Value * {
            Value *geLo = builder_.CreateICmpSGE(ch,
                ConstantInt::get(i32, lo), "tz_ge");
            Value *leHi = builder_.CreateICmpSLE(ch,
                ConstantInt::get(i32, hi), "tz_le");
            return builder_.CreateAnd(geLo, leHi, "tz_rng");
        };
        // Plus zone: '&' (0x26) | A-I (0x41-0x49) | a-i (0x61-0x69)
        Value *plusAmp = builder_.CreateICmpEQ(ch,
            ConstantInt::get(i32, (int)'&'), "tz_amp");
        Value *plus    = builder_.CreateOr(plusAmp,
            builder_.CreateOr(inRange('A','I'), inRange('a','i'), "tz_pAi"),
            "tz_plus");
        // Minus zone: '-' (0x2D) | J-R (0x4A-0x52) | j-r (0x6A-0x72)
        Value *minusDsh= builder_.CreateICmpEQ(ch,
            ConstantInt::get(i32, (int)'-'), "tz_dsh");
        Value *minus   = builder_.CreateOr(minusDsh,
            builder_.CreateOr(inRange('J','R'), inRange('j','r'), "tz_mJr"),
            "tz_minus");
        // Zero zone = neither (the fallthrough).

        auto set = [&](ResultInd slot, Value *v) {
            if (slot.indicator == 0) return;
            inds_.store_resolved(slot.indicator, v);
        };
        set(c.hi, plus);
        set(c.lo, minus);
        if (c.eq.indicator != 0) {
            Value *zero = builder_.CreateOr(plus, minus, "tz_pm");
            zero = builder_.CreateNot(zero, "tz_zero");
            inds_.store_resolved(c.eq.indicator, zero);
        }
    }

    /* Build the i32 bit mask named in factor2, shared by TESTB/BITON/BITOF:
     * either a bit-number literal '025' (digits 0-7, 0 = leftmost/high bit)
     * or the ON bits of a 1-position alphameric field. Returns nullptr (and
     * reports) if factor2 is neither. */
    llvm::Value *emit_bit_mask(const CSpec &c, const char *opname) {
        using namespace llvm;
        auto *i8  = Type::getInt8Ty(mod_->getContext());
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        if (!c.factor2.empty() && c.factor2.front() == '\'') {
            // Bit-number literal: each digit 0-7 names a bit (0=leftmost=0x80).
            std::string lit = c.factor2.substr(1);
            if (!lit.empty() && lit.back() == '\'') lit.pop_back();
            unsigned m = 0;
            for (char d : lit) {
                if (d >= '0' && d <= '7') m |= (0x80 >> (d - '0'));
            }
            return ConstantInt::get(i32, m);
        }
        Value *fp; int flen;
        if (syms_.resolve_char_operand(c.factor2, fp, flen)) {
            Value *fb = builder_.CreateLoad(i8, fp, "bm_f");
            return builder_.CreateZExt(fb, i32, "bm_mv");
        }
        report("input", c.lineno, 33, DiagKind::Error,
               std::string(opname) +
               " requires a bit-number literal or character field in factor 2");
        return nullptr;
    }

    /* TESTB: test the bits named in factor2 against the corresponding bits of
     * the 1-position result field. Sets:
     *   HI: every tested bit is OFF in the result
     *   LO: the tested bits are of MIXED status (some on, some off)
     *   EQ: every tested bit is ON in the result  (also set if mask==0) */
    void emit_testb(const CSpec &c) {
        using namespace llvm;
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "TESTB requires a result field");
            return;
        }
        // Resolve the result byte (1-position character field).
        if (!syms_.is_char_field(c.result))
            syms_.get_or_create_char_field(c.result, 1);
        Value *rp; int rlen;
        if (!syms_.resolve_char_operand(c.result, rp, rlen)) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "TESTB result field must be alphameric");
            return;
        }
        auto *i8  = Type::getInt8Ty(mod_->getContext());
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        Value *rb = builder_.CreateLoad(i8, rp, "tb_r");
        Value *resVal = builder_.CreateZExt(rb, i32, "tb_rv");

        Value *mask = emit_bit_mask(c, "TESTB");
        if (!mask) return;

        // tested = resVal & mask  (bits we care about, with their result status)
        // The manual compares only the bits named in the mask.
        Value *tested = builder_.CreateAnd(resVal, mask, "tb_tested");
        Value *allOn  = builder_.CreateICmpEQ(tested, mask, "tb_allon");
        Value *anyOn  = builder_.CreateICmpNE(tested, ConstantInt::get(i32, 0),
                                              "tb_anyon");
        Value *allOff = builder_.CreateICmpEQ(tested, ConstantInt::get(i32, 0),
                                              "tb_alloff");
        // HI: every tested bit off. EQ: every tested bit on. LO: mixed (some on,
        // some off). If mask == 0 (no bits selected), the manual says EQ turns on.
        Value *mixed = builder_.CreateAnd(anyOn, builder_.CreateNot(allOn),
                                          "tb_mixed");

        auto set = [&](ResultInd slot, Value *v) {
            if (slot.indicator == 0) return;
            inds_.store_resolved(slot.indicator, v);
        };
        set(c.hi, allOff);
        set(c.lo, mixed);
        set(c.eq, allOn);
    }

    /* BITON/BITOF: set on (BITON) or off (BITOF) each bit named in factor2's
     * mask within the 1-position result field; bits not named are unaffected
     * (manual 105336-105362, 105207-105233). Factor1/decimals/half-adjust/
     * resulting indicators are validated blank in cspec.cpp. */
    void emit_bit_set(const CSpec &c, bool on) {
        using namespace llvm;
        const char *nm = on ? "BITON" : "BITOF";
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   std::string(nm) + " requires a result field");
            return;
        }
        if (!syms_.is_char_field(c.result))
            syms_.get_or_create_char_field(c.result, 1);
        Value *rp; int rlen;
        if (!syms_.resolve_char_operand(c.result, rp, rlen)) {
            report("input", c.lineno, 43, DiagKind::Error,
                   std::string(nm) + " result field must be alphameric");
            return;
        }
        Value *mask = emit_bit_mask(c, nm);
        if (!mask) return;
        auto *i8 = Type::getInt8Ty(mod_->getContext());
        Value *rb = builder_.CreateLoad(i8, rp, "bs_r");
        Value *m8 = builder_.CreateTrunc(mask, i8, "bs_m");
        Value *nv = on
            ? builder_.CreateOr(rb, m8, "bs_or")
            : builder_.CreateAnd(rb, builder_.CreateNot(m8, "bs_notm"), "bs_and");
        builder_.CreateStore(nv, rp);
    }

    /* *LIKE DEFN: define a new field (result) with the attributes of an
     * existing field (factor2). Unlike every other op, columns 49-51 (parsed
     * into c.result_len) are a signed length DELTA, not an absolute length --
     * a blank field parses to 0, which already means "no change" here, so no
     * special-casing is needed. Decimals always copy from factor2 verbatim
     * (manual 106341-106375). Numeric fields in this compiler carry no
     * separate digit-length attribute (see symbols.h), so the length delta
     * only has an effect for a character source field. */
    void emit_defn(const CSpec &c) {
        if (c.factor2.empty() || c.result.empty()) return;  // reported in cspec.cpp
        if (syms_.has_field(c.result)) return;  // idempotent, like get_or_create_*
        const FieldInfo *fi = syms_.info(c.factor2);
        if (!fi) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "*LIKE DEFN factor 2 '" + c.factor2 +
                   "' is not a declared field");
            return;
        }
        if (fi->kind == FieldKind::Character) {
            int len = fi->length + c.result_len;
            if (len < 1) len = 1;
            syms_.get_or_create_char_field(c.result, len);
        } else if (fi->kind == FieldKind::Numeric) {
            syms_.set_numeric_attrs(c.result, fi->decimals < 0 ? 0 : fi->decimals);
        } else {
            report("input", c.lineno, 33, DiagKind::Error,
                   "*LIKE DEFN factor 2 '" + c.factor2 +
                   "' must be a simple numeric or character field");
        }
    }

    /* SORTA: sort the array named in factor2 in place, per its E-spec
     * ascending/descending sequence flag (B2). Numeric arrays only, matching
     * LOKUP's existing restriction (A9) -- there is no byte-compare sort.
     * Factor1/result/half-adjust/resulting-indicators are validated blank in
     * cspec.cpp. */
    void emit_sorta(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        if (c.factor2.empty()) return;  // reported in cspec.cpp
        const std::string &an = c.factor2;
        if (!syms_.is_array(an)) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "SORTA requires an array in factor 2");
            return;
        }
        const FieldInfo *fi = syms_.info(an);
        if (fi && fi->is_char_array) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "SORTA against an alphameric array/table is not supported");
            return;
        }
        if (!sorta_fn_) {
            sorta_fn_ = Function::Create(
                FunctionType::get(Type::getVoidTy(ctx),
                    {PointerType::get(ctx, 0), i32, i32}, false),
                Function::ExternalLinkage, "rpg_rt_sorta", mod_.get());
        }
        auto *arrTy = ArrayType::get(i32, (unsigned)fi->array_count);
        Value *arrp = builder_.CreateConstInBoundsGEP2_32(arrTy,
            cast<GlobalVariable>(fi->gv), 0, 0, an + "_sp");
        // B2: default ascending when the array has no declared sequence.
        bool ascending = true;
        if (arrays_) {
            for (const auto &a : *arrays_) {
                if (a.name == an) { ascending = a.ascending; break; }
                if (a.alt_name == an) { ascending = a.alt_ascending; break; }
            }
        }
        builder_.CreateCall(sorta_fn_,
            {arrp, ConstantInt::get(i32, fi->array_count, true),
             ConstantInt::get(i32, ascending ? 1 : 0, true)});
    }

    /* TIME: store the current time-of-day into the numeric result field
     * (manual 124880-124913). This compiler represents numeric fields as
     * native 32-bit scaled integers with no separate digit-length attribute
     * (see symbols.h), so only the always-fitting 6-digit hhmmss form is
     * produced; the manual's 12-digit time+date variant needs a wider field
     * representation than exists here. */
    void emit_time(const CSpec &c) {
        using namespace llvm;
        auto &ctx = mod_->getContext();
        auto *i32 = Type::getInt32Ty(ctx);
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "TIME requires a result field");
            return;
        }
        Value *t64 = builder_.CreateCall(time_fn_, {}, "tm_v");
        Value *t32 = builder_.CreateTrunc(t64, i32, "tm_v32");
        Value *rp = resolve_result_ptr(c);
        if (!rp) return;
        builder_.CreateStore(t32, rp);
    }

    /* MHHZO/MHLZO/MLHZO/MLLZO: move the "zone" (high nibble) of one byte of
     * factor2 into the corresponding byte of the result, leaving the digit
     * (low) nibble of that result byte untouched. `src_high`/`dst_high`
     * select the leftmost (true) or rightmost (false) byte of factor2/result
     * respectively (manual 113217-113347). This compiler stores numeric
     * fields as native i32s rather than zoned-decimal bytes (symbols.h), so
     * -- like TESTZ/TESTB's own result field -- only alphameric operands are
     * supported on either side, narrower than the manual's numeric-capable
     * cases. */
    void emit_movezone(const CSpec &c, bool src_high, bool dst_high,
                       const char *opname) {
        using namespace llvm;
        if (c.factor2.empty() || c.result.empty()) {
            report("input", c.lineno, 33, DiagKind::Error,
                   std::string(opname) + " requires factor 2 and a result field");
            return;
        }
        Value *sp; int slen;
        if (!syms_.resolve_char_operand(c.factor2, sp, slen)) {
            report("input", c.lineno, 33, DiagKind::Error,
                   std::string(opname) + " factor 2 must be alphameric");
            return;
        }
        if (!syms_.is_char_field(c.result))
            syms_.get_or_create_char_field(c.result, slen);
        Value *dp; int dlen;
        if (!syms_.resolve_char_operand(c.result, dp, dlen)) {
            report("input", c.lineno, 43, DiagKind::Error,
                   std::string(opname) + " result field must be alphameric");
            return;
        }
        auto &ctx = mod_->getContext();
        auto *i8  = Type::getInt8Ty(ctx);
        auto *i32 = Type::getInt32Ty(ctx);
        int soff = src_high ? 0 : slen - 1;
        int doff = dst_high ? 0 : dlen - 1;
        Value *sbp = builder_.CreateInBoundsGEP(i8, sp, ConstantInt::get(i32, soff), "mz_sp");
        Value *dbp = builder_.CreateInBoundsGEP(i8, dp, ConstantInt::get(i32, doff), "mz_dp");
        Value *sb = builder_.CreateLoad(i8, sbp, "mz_sb");
        Value *db = builder_.CreateLoad(i8, dbp, "mz_db");
        Value *szone = builder_.CreateAnd(sb, ConstantInt::get(i8, 0xF0), "mz_sz");
        Value *ddig  = builder_.CreateAnd(db, ConstantInt::get(i8, 0x0F), "mz_dd");
        Value *nv    = builder_.CreateOr(szone, ddig, "mz_nv");
        builder_.CreateStore(nv, dbp);
    }

    /* Like resolve_result_ptr but takes a raw token (for LOKUP index fields). */
    llvm::Value *resolve_result_ptr_for_token(const std::string &tok) {
        if (tok.empty()) return nullptr;
        CSpec dummy; dummy.result = tok;
        return resolve_result_ptr(dummy);
    }

    std::unique_ptr<llvm::Module> mod_;
    llvm::IRBuilder<>             builder_;
    SymbolTable                   syms_;
    IndicatorEmitter              inds_;
    // Runtime function declarations.
    llvm::Function               *close_all_    = nullptr;
    llvm::Function               *open_input_   = nullptr;
    llvm::Function               *set_reclen_   = nullptr;
    llvm::Function               *read_next_    = nullptr;
    llvm::Function               *peek_next_    = nullptr;  // Section E (E19)
    // Section G (G24) keyed / random access.
    llvm::Function               *set_key_      = nullptr;
    llvm::Function               *chain_        = nullptr;
    llvm::Function               *setll_        = nullptr;
    llvm::Function               *read_op_      = nullptr;
    llvm::Function               *reade_        = nullptr;
    llvm::Function               *readp_        = nullptr;  // Group C (C1)
    llvm::Function               *time_fn_      = nullptr;  // Group C (C5)
    // Section G (G25) update files.
    llvm::Function               *open_update_  = nullptr;
    llvm::Function               *write_rec_    = nullptr;
    llvm::Function               *update_rec_   = nullptr;
    llvm::Function               *delete_rec_   = nullptr;
    llvm::Function               *flush_rec_    = nullptr;
    llvm::Function               *get_decimal_  = nullptr;
    llvm::Function               *get_packed_   = nullptr;  // Section C
    llvm::Function               *get_binary_   = nullptr;  // Section C
    llvm::Function               *overpunch_in_  = nullptr; // Section C (C10)
    llvm::Function               *overpunch_out_ = nullptr; // Section C (C10)
    // Section D output helpers.
    llvm::Function               *skip_fn_       = nullptr; // D13 skip/page
    llvm::Function               *page_fn_       = nullptr; // D14 page counter
    llvm::Function               *edit_word_fn_  = nullptr; // D16 edit words
    // Section F (F22) overflow configuration / polling.
    llvm::Function               *set_overflow_   = nullptr;
    llvm::Function               *take_overflow_  = nullptr;
    // The file id of the record currently being emitted (for PAGE fields).
    llvm::Value                  *cur_out_fid_   = nullptr;
    // Record buffer global for the cycle.
    llvm::GlobalVariable         *rec_buf_      = nullptr;
    // Look-ahead peek buffer global (E19).
    llvm::GlobalVariable         *la_buf_       = nullptr;
    // Runtime output-function declarations.
    llvm::Function               *open_output_     = nullptr;
    llvm::Function               *line_begin_      = nullptr;
    llvm::Function               *line_put_str_    = nullptr;
    llvm::Function               *line_put_num_    = nullptr;
    llvm::Function               *line_put_num_dec_ = nullptr;  // Section C
    llvm::Function               *emit_line_       = nullptr;
    // Phase 10 edit-code formatter.
    llvm::Function               *edit_fn_         = nullptr;
    llvm::Function               *edit_dec_fn_     = nullptr;   // Section C
    // Section B prerun-time array/table loader.
    llvm::Function               *load_arrays_  = nullptr;
    llvm::Function               *load_char_arrays_ = nullptr;   // A9
    // Map of output filename -> its file-id Value (set during cycle entry).
    std::unordered_map<std::string, llvm::Value *> *out_ids_ = nullptr;
    // Map of input/chained/full-proc filename -> file-id Value (Section G, G24).
    // Populated by open_input_files(); used by CHAIN/SETLL/READ/READE.
    std::unordered_map<std::string, llvm::Value *> in_ids_;
    // The program's O-spec records, stashed for calc-time EXCPT emission.
    const std::vector<ORecord> *outputs_ = nullptr;
    // The program's F-specs and I-fields, stashed for calc-time file ops
    // (CHAIN/SETLL/READ/READE field decode, Section G).
    const std::vector<FSpec> *files_ = nullptr;
    const std::vector<ISpecField> *in_fields_ = nullptr;
    // The program's E-spec arrays/tables (for prerun-time load emission).
    const std::vector<ESpecArray> *arrays_ = nullptr;
    // TAG label -> its basic block, for resolving GOTO (incl. forward refs).
    // Cleared per spec chain so detail and total chains don't cross-resolve.
    std::unordered_map<std::string, llvm::BasicBlock *> tag_blocks_;
    // Merge block for the current chain's control-level gate (Phase 8).
    llvm::BasicBlock *level_skip_ = nullptr;
    // Subroutine name -> LLVM function (Phase 8b).
    std::unordered_map<std::string, llvm::Function *> subroutines_;
    // sqrt intrinsic (Phase 9).
    llvm::Function *sqrt_fn_ = nullptr;
    // rpg_rt_cmp_str (Phase 10 character compare).
    llvm::Function *cmp_str_ = nullptr;
    // rpg_rt_lokup (Phase 10 array search).
    llvm::Function *lokup_fn_ = nullptr;
    // rpg_rt_sorta (Group C, C4 array sort).
    llvm::Function *sorta_fn_ = nullptr;
    // Program linkage.
    llvm::Function *register_program_fn_ = nullptr;
    llvm::Function *call_fn_             = nullptr;
    llvm::Function *free_fn_             = nullptr;
    llvm::Function *field_to_cstr_fn_    = nullptr;
    // Scratch buffer for CALL/FREE's dynamic (field-valued) target-name
    // form -- rpg_rt_field_to_cstr's NUL-terminated output. One shared
    // buffer is safe: CALL/FREE run synchronously, never overlapping.
    llvm::GlobalVariable *name_scratch_ = nullptr;
    static constexpr unsigned kNameScratchLen = 32;
    const std::vector<ParamList> *param_lists_ = nullptr;
    const std::vector<ExitDecl>  *exit_decls_  = nullptr;
    // False for a "library" program compiled alongside others and only
    // reachable via CALL (see main.cpp's multi-file build): its entry
    // function is rpg_prog_<NAME>(void **parm_ptrs, i32 parm_count,
    // i32 first_call) -> i32 status instead of the process's own `main`.
    bool is_top_level_ = true;
    // Upper-cased H-spec program-id (or a sanitized fallback derived from the
    // module name), used as both the registry key and the entry symbol name.
    std::string program_name_;
    // Shared out-param slots for rpg_rt_call's error/LR results (reused
    // across every CALL site; RPG II has no concurrency to race on them).
    llvm::GlobalVariable *call_err_slot_ = nullptr;
    llvm::GlobalVariable *call_lr_slot_  = nullptr;
    // One shared void*[] buffer per named PLIST, reused across repeated
    // CALLs through the same PLIST.
    std::unordered_map<std::string, llvm::GlobalVariable *> parm_bufs_;
    // True while emitting a subroutine body (BEGSR..ENDSR): RETRN's early
    // `ret i32` would be ill-typed there (the subroutine function returns
    // void) -- the manual only allows RETRN in the main calculation body.
    bool in_subroutine_ = false;
    // True if the most recent op was a DIV whose remainder is available for a
    // following MVR. Cleared by any intervening op or by MVR itself.
    bool last_remainder_ = false;
    // Decimal positions the stashed remainder is scaled to (the DIV's own
    // result_dec) -- MVR must rescale from this to its own result field's
    // decimals before storing (A10). Compile-time only: DIV/MVR pairing is
    // resolved by codegen's linear scan, same as last_remainder_ itself.
    int last_remainder_dec_ = 0;
    // Hidden global holding the DIV remainder, lazily created.
    llvm::GlobalVariable *divrem_slot_ = nullptr;
    // D1: H-spec col 18 currency symbol (default '$'), consulted by
    // emit_edit_word_field for the floating-currency detection (A13).
    char currency_symbol_ = '$';
};

} // namespace

std::unique_ptr<llvm::Module> generate_module(
    const Program &prog,
    const std::string &module_name,
    llvm::LLVMContext &ctx,
    bool is_top_level) {

    CodeGen cg(ctx, module_name, is_top_level);
    cg.generate(prog);
    auto mod = cg.take_module();

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        error("generated module failed verification");
        mod->print(llvm::errs(), nullptr);
    }
    return mod;
}

std::unique_ptr<llvm::Module> generate_module_linear(
    const std::vector<CSpec> &specs,
    const std::string &module_name,
    llvm::LLVMContext &ctx) {

    CodeGen cg(ctx, module_name);
    Program prog;
    prog.calcs = specs;
    cg.generate_linear(prog);
    auto mod = cg.take_module();

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        error("generated module failed verification");
        mod->print(llvm::errs(), nullptr);
    }
    return mod;
}

} // namespace rpgc
