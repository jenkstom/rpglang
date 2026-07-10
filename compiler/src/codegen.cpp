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

#include <cstdio>
#include <algorithm>
#include <unordered_map>

namespace rpgc {

namespace {

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
     *   1..99   => general indicator slot
     */
    llvm::Value *load_resolved(int idx) {
        if (idx == -1)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), lr_, "lr");
        if (idx == -11)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), first_page_, "p1");
        if (idx <= -2 && idx >= -10)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_),
                                       ctl_[-idx - 1], "l" + std::to_string(-idx-1));
        return load(idx);
    }
    void store_resolved(int idx, llvm::Value *v) {
        if (idx == -1) { builder_.CreateStore(v, lr_); return; }
        if (idx == -11) { builder_.CreateStore(v, first_page_); return; }
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
    CodeGen(llvm::LLVMContext &ctx, const std::string &name)
        : mod_(std::make_unique<llvm::Module>(name, ctx)),
          builder_(ctx),
          syms_(*mod_, builder_),
          inds_(*mod_, builder_) {}

    llvm::Module *module() { return mod_.get(); }
    std::unique_ptr<llvm::Module> take_module() { return std::move(mod_); }

    /* Top-level: generate either the cycle (if a primary input file exists)
     * or the linear form (C-specs once). */
    bool generate(const Program &prog) {
        declare_runtime();
        // Create globals for every numeric E-spec array/table. Tables (TAB-
        // prefixed) additionally get a current-element shadow index. An
        // alternating partner (cols 46-51) becomes its own array/table global.
        for (const auto &a : prog.arrays) {
            if (a.decimals >= 0)
                syms_.get_or_create_array(a.name, a.entries, a.init_data,
                                          is_table_name(a.name));
            if (!a.alt_name.empty() && a.alt_decimals >= 0)
                syms_.get_or_create_array(a.alt_name, a.entries, a.alt_init_data,
                                          is_table_name(a.alt_name));
        }
        arrays_ = &prog.arrays;
        // Stash the O-spec records so EXCPT can reach them from calc time.
        outputs_ = &prog.outputs;
        // Compile subroutines first so EXSR calls in main can resolve.
        generate_subroutines(prog.calcs);
        if (find_primary_input(prog.files)) {
            return generate_cycle(prog);
        }
        return generate_linear(prog.calcs, prog.outputs);
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
            if (a.decimals < 0) continue;   // alphanumeric: deferred
            Value *path = builder_.CreateGlobalStringPtr(a.from_file, "afn");
            const FieldInfo *fi = syms_.info(a.name);
            if (!fi) continue;
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
            builder_.CreateCall(load_arrays_,
                {path, ConstantInt::get(i32, lenA, true),
                 ConstantInt::get(i32, lenB, true),
                 ConstantInt::get(i32, a.entries, true), outA, outB},
                a.name+"_ld");
        }
    }

    /* Linear form: C-specs run once, in order (Phase 2 behaviour). */
    bool generate_linear(const std::vector<CSpec> &specs,
                         const std::vector<ORecord> &outputs) {
        using namespace llvm;

        // Build main.
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        FunctionType *ft = FunctionType::get(i32, false);
        Function *main = Function::Create(ft, Function::ExternalLinkage,
                                          "main", mod_.get());
        BasicBlock *entry = BasicBlock::Create(mod_->getContext(), "entry", main);
        builder_.SetInsertPoint(entry);

        // Load any prerun-time arrays/tables before the program runs.
        emit_prerun_loads();

        // Linear programs generally have no output, but if O-specs are present
        // run the first-page (heading) pass too (D12).
        static std::unordered_map<std::string, llvm::Value *> empty_ids;
        out_ids_ = &empty_ids;
        BasicBlock *afterHead = emit_heading_pass(main, entry, outputs);

        // Route through emit_spec_chain so structured ops (IF/ELSE/END/DOW/DOU)
        // and GOTO/TAG work in linear programs too. Level "" picks up all specs
        // whose control_level is blank.
        BasicBlock *prev = emit_spec_chain(main, afterHead, specs, "");

        BasicBlock *exitbb = BasicBlock::Create(mod_->getContext(), "exit", main);
        builder_.SetInsertPoint(prev);
        builder_.CreateBr(exitbb);
        builder_.SetInsertPoint(exitbb);
        builder_.CreateCall(close_all_);

        Value *ret = specs.empty() ? (Value*)ConstantInt::get(i32, 0)
                                   : emit_exit_value();
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
        // Section C: decimal-aware edit-code formatter.
        edit_dec_fn_ = Function::Create(
            FunctionType::get(i32, {i64, i32, i32, i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_edit_dec", mod_.get());

        // Section B: prerun-time array/table loader.
        load_arrays_ = Function::Create(
            FunctionType::get(i32, {ptr, i32, i32, i32, ptr, ptr}, false),
            Function::ExternalLinkage, "rpg_rt_load_arrays", mod_.get());
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

    /* Decode one input field from the record buffer into its global, and emit
     * any field indicators (E18). Extracted from the extract loop so the
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
            // Field indicator (cols 69-70): on when an alphameric field is
            // all blanks.
            if (fld.zero_ind) {
                Value *allblank = ConstantInt::getTrue(c);
                for (int b = 0; b < len; ++b) {
                    Value *bp = builder_.CreateInBoundsGEP(i8, srcp,
                        ConstantInt::get(i32, b, true));
                    Value *ch = builder_.CreateLoad(i8, bp, fld.name+"_cb");
                    Value *isSp = builder_.CreateICmpEQ(ch,
                        ConstantInt::get(i8, (uint8_t)' '), fld.name+"_sp");
                    allblank = builder_.CreateAnd(allblank, isSp, fld.name+"_ab");
                }
                inds_.store_resolved(fld.zero_ind, allblank);
            }
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
            // Field indicators (cols 65-70): plus on >0, minus on <0, zero on 0.
            auto *zero = ConstantInt::get(i32, 0, true);
            if (fld.plus_ind)
                inds_.store_resolved(fld.plus_ind,
                    builder_.CreateICmpSGT(v32, zero, fld.name+"_p"));
            if (fld.minus_ind)
                inds_.store_resolved(fld.minus_ind,
                    builder_.CreateICmpSLT(v32, zero, fld.name+"_m"));
            if (fld.zero_ind)
                inds_.store_resolved(fld.zero_ind,
                    builder_.CreateICmpEQ(v32, zero, fld.name+"_z"));
        }
    }

    /* Record-identification selection (E17). If any record type for the primary
     * file carries identification codes, match the current record (in bufPtr)
     * against each type's code-sets, set the matching record-identifying
     * indicator, and return a new block to continue field extraction from.
     * Records matching no type branch back to `head` (skipped). If no record
     * type has codes, returns `extract` unchanged (no-op). */
    llvm::BasicBlock *emit_record_selection(llvm::Function *main,
                                            llvm::BasicBlock *head,
                                            llvm::BasicBlock *extract,
                                            llvm::Value *bufPtr,
                                            const Program &prog) {
        using namespace llvm;
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i8  = Type::getInt8Ty(c);
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
            Value *match = ConstantInt::getTrue(c);
            for (const auto &cs : r->codes) {
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

    bool generate_cycle(const Program &prog) {
        using namespace llvm;
        auto &c  = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i64 = Type::getInt64Ty(c);

        const FSpec *pf = find_primary_input(prog.files);
        if (!pf) return generate_linear(prog.calcs, prog.outputs); // defensive

        int reclen = pf->reclen > 0 ? pf->reclen : 80;

        // Function + entry block.
        FunctionType *ft = FunctionType::get(i32, false);
        Function *main = Function::Create(ft, Function::ExternalLinkage,
                                          "main", mod_.get());
        BasicBlock *entry = BasicBlock::Create(c, "entry", main);
        builder_.SetInsertPoint(entry);

        // Load any prerun-time arrays/tables before the cycle starts.
        emit_prerun_loads();

        // Record buffer global: [reclen+1 x i8], zeroed.
        auto *bufTy = ArrayType::get(Type::getInt8Ty(c), (unsigned)reclen + 1);
        rec_buf_ = new GlobalVariable(
            *mod_, bufTy, /*isConstant=*/false,
            GlobalValue::InternalLinkage,
            ConstantAggregateZero::get(bufTy), "rpg_rec");

        // Open the primary file. The file name comes from the F-spec; the path
        // is taken to be the literal filename (a Linux file in the cwd).
        Value *name = builder_.CreateGlobalStringPtr(pf->name, "fname");
        Value *fid  = builder_.CreateCall(open_input_, {name}, "file_id");
        // set_reclen(fid, reclen)
        builder_.CreateCall(set_reclen_,
            {fid, ConstantInt::get(i32, reclen, true)});

        // Open output files (those referenced by O-specs) and stash their file
        // ids. PRINTER files map to a real file named after the F-spec filename.
        std::unordered_map<std::string, llvm::Value *> out_ids;
        for (const auto &o : prog.outputs) {
            if (o.file.empty()) continue;
            if (out_ids.count(o.file)) continue;
            // Find the F-spec to confirm it's an output/printer file.
            const FSpec *ofs = nullptr;
            for (const auto &f : prog.files)
                if (f.name == o.file) { ofs = &f; break; }
            if (!ofs || ofs->type == FileType::Input) continue;
            Value *onm = builder_.CreateGlobalStringPtr(o.file, "oname");
            out_ids[o.file] = builder_.CreateCall(open_output_, {onm},
                                                  o.file + "_id");
        }
        out_ids_ = &out_ids;

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
        // read_next(fid, rec_buf, reclen+1)
        Value *bufPtr = builder_.CreateConstInBoundsGEP2_32(
            ArrayType::get(Type::getInt8Ty(c), reclen + 1), rec_buf_, 0, 0);
        Value *got = builder_.CreateCall(read_next_,
            {fid, bufPtr, ConstantInt::get(i64, reclen + 1, false)}, "got_rec");

        BasicBlock *extract = BasicBlock::Create(c, "extract", main);
        BasicBlock *lrtotal = BasicBlock::Create(c, "lr.total", main);
        Value *haveRec = builder_.CreateICmpNE(
            got, ConstantInt::get(i32, 0), "have_rec");
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
            // EOF: fill numeric look-ahead fields with all-9s (the manual's
            // end-of-file rule); leave alphameric fields unchanged.
            builder_.SetInsertPoint(laEOF);
            for (const auto &fld : prog.lookahead_fields) {
                if (fld.name.empty() || fld.decimals < 0) continue;
                int len = fld.to - fld.from + 1;
                if (len < 1) len = 1;
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
        // cascade (set L1..Ln). Then store the new values as previous.
        if (!ctl_fields.empty()) {
            auto *maxbrk = builder_.CreateAlloca(i32ty, nullptr, "maxbrk");
            builder_.CreateStore(ConstantInt::get(i32ty, 0), maxbrk);
            for (auto &cf : ctl_fields) {
                Value *cur  = syms_.load_field(cf.spec->name);
                Value *prv  = builder_.CreateLoad(i32ty, cf.prev, "pv");
                Value *same = builder_.CreateICmpEQ(cur, prv, "eq");
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
            // Cascade-set L1..maxbrk. We do it with a runtime branch per level:
            // for n=9..1: if maxbrk>=n set Ln on.
            Value *mb = builder_.CreateLoad(i32ty, maxbrk, "maxbrk_v");
            for (int n = 9; n >= 1; --n) {
                Value *ge = builder_.CreateICmpSGE(mb, ConstantInt::get(i32ty, n), "ge");
                inds_.store_resolved(-1 - n, ge);
            }
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
        builder_.CreateBr(detail);
        // Clear the first-cycle flag (only meaningful once).
        {
            BasicBlock *det2 = detail;
            builder_.SetInsertPoint(det2);
            builder_.CreateStore(ConstantInt::getFalse(c), firstflag);
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

        // exit
        BasicBlock *exitbb = BasicBlock::Create(c, "exit", main);
        builder_.CreateBr(exitbb);
        builder_.SetInsertPoint(exitbb);
        builder_.CreateCall(close_all_);
        builder_.CreateRet(emit_exit_value());
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
                                      const ORecord &rec) {
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
        Value *cond = eval_conditions(inds_, builder_, rec.conditions);
        BasicBlock *dob = BasicBlock::Create(c, "out_do", main);
        BasicBlock *nxt = BasicBlock::Create(c, "out_after", main);
        builder_.CreateCondBr(cond, dob, nxt);

        builder_.SetInsertPoint(dob);
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
        builder_.CreateBr(nxt);
        builder_.SetInsertPoint(nxt);
        return nxt;
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
                        ConstantInt::get(i32, f.end_pos, true),
                        ConstantInt::get(i32, dec, true),
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
                FunctionType::get(i32, {i64, ptr, i32, i32, ptr, i32}, false),
                Function::ExternalLinkage, "rpg_rt_edit_word", mod_.get());
        }
        Value *v = syms_.load_field(f.name);
        Value *v64 = builder_.CreateSExt(v, i64, f.name+"_ew");
        int dec = syms_.field_decimals(f.name);
        Value *word = builder_.CreateGlobalStringPtr(f.edit_word, "eword");
        Value *buf = builder_.CreateAlloca(Type::getInt8Ty(c),
            ConstantInt::get(i32, 64, false), f.name+"_ewb");
        Value *n = builder_.CreateCall(edit_word_fn_, {
            v64, word,
            ConstantInt::get(i32, (int)f.edit_word.size(), true),
            ConstantInt::get(i32, dec, true),
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

    /* True if any condition references the 1P indicator (idx -11). */
    bool has_1p_condition(const std::vector<CondInd> &conds) {
        for (const auto &c : conds) if (c.indicator == -11) return true;
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

                bool unconditional = (c->cmp == CmpOp::NONE &&
                                      c->factor1.empty() && c->factor2.empty());

                BasicBlock *callBB =
                    BasicBlock::Create(ctx, "cas_call", main);
                BasicBlock *nextBB =
                    BasicBlock::Create(ctx, "cas_next", main);

                builder_.SetInsertPoint(prev);
                if (unconditional) {
                    // Always run the sub (subject only to conditioning inds, but
                    // a true unconditional CAS ignores them like EXSR). Branch
                    // straight to the call.
                    builder_.CreateBr(callBB);
                } else if (fire) {
                    builder_.CreateCondBr(fire, callBB, nextBB);
                } else {
                    // No comparison and not unconditional (e.g. CAS with no
                    // factors but indicators only): treat as unconditional.
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

        // Unmatched openers: report but keep going.
        if (!frames.empty()) {
            error("unbalanced IFxx/DOWxx/DOUxx/DO (missing END)");
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
        Value *rem = builder_.CreateLoad(Type::getInt32Ty(mod_->getContext()),
                                         get_divrem_slot(), "mvr_rem");
        last_remainder_ = false;   // consume: a later stray MVR is an error
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
     * resulting-indicator slot (HI/LO/EQ columns 54-59). */
    void emit_seton(const CSpec &c) {
        using namespace llvm;
        auto *ctx = &mod_->getContext();
        auto *v   = (c.op == Op::SETON)
                        ? ConstantInt::getTrue(*ctx)
                        : ConstantInt::getFalse(*ctx);
        for (ResultInd slot : {c.hi, c.lo, c.eq}) {
            if (slot.indicator != 0) {
                inds_.store_resolved(slot.indicator, v);
            }
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
        BasicBlock *after = emit_spec_chain(fn, entry, body, "");
        builder_.SetInsertPoint(after);
        builder_.CreateRetVoid();
    }

    /* Scan all C-specs for BEGSR..ENDSR groups and compile each as a function.
     * Subroutine specs (control_level == "SR") are also recorded so the normal
     * chains skip them. Must run before the main body so EXSR can resolve. */
    void generate_subroutines(const std::vector<CSpec> &calcs) {
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
        return builder_.CreateICmp(p, f1, f2, "cmp");
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
            int dstlen;
            Value *rptr;
            if (syms_.is_char_field(c.result)) {
                dstlen = syms_.info(c.result)->length;
                rptr = syms_.get_or_create_char_field(c.result, dstlen);
            } else {
                dstlen = c.result_len > 0 ? c.result_len : srclen;
                rptr = syms_.get_or_create_char_field(c.result, dstlen);
            }
            auto *arrTy = ArrayType::get(i8, (unsigned)dstlen);
            Value *dstp = builder_.CreateConstInBoundsGEP2_32(arrTy,
                cast<GlobalVariable>(rptr), 0, 0, c.result+"_dp");
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
            int dstlen = syms_.is_char_field(c.result)
                ? syms_.info(c.result)->length
                : (c.result_len > 0 ? c.result_len : 1);
            Value *rptr = syms_.get_or_create_char_field(c.result, dstlen);
            auto *arrTy = ArrayType::get(i8, (unsigned)dstlen);
            Value *dstp = builder_.CreateConstInBoundsGEP2_32(arrTy,
                cast<GlobalVariable>(rptr), 0, 0, c.result+"_dp");
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
        // Build the runtime call: rpg_rt_lokup(key, arrptr, count, &idx)
        if (!lokup_fn_) {
            lokup_fn_ = Function::Create(
                FunctionType::get(i32,
                    {i64, PointerType::get(i32,0), i32, PointerType::get(i32,0)}, false),
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
        Value *rc = builder_.CreateCall(lokup_fn_,
            {key64, arrp, ConstantInt::get(i32, fi->array_count, true), idxp},
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

    /* TESTB: test the bits named in factor2 against the corresponding bits of
     * the 1-position result field. Factor2 is either a bit-number literal
     * '025' (digits 0-7, 0 = leftmost/high bit) or a 1-position alphameric
     * field whose ON bits form the mask. Sets:
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

        // Build the mask: either from a bit-number literal in factor2 ('025')
        // or from the ON bits of a 1-position field in factor2.
        Value *mask = nullptr;
        Value *fp; int flen;
        if (!c.factor2.empty() && c.factor2.front() == '\'') {
            // Bit-number literal: each digit 0-7 names a bit (0=leftmost=0x80).
            std::string lit = c.factor2.substr(1);
            if (!lit.empty() && lit.back() == '\'') lit.pop_back();
            unsigned m = 0;
            for (char d : lit) {
                if (d >= '0' && d <= '7') m |= (0x80 >> (d - '0'));
            }
            mask = ConstantInt::get(i32, m);
        } else if (syms_.resolve_char_operand(c.factor2, fp, flen)) {
            Value *fb = builder_.CreateLoad(i8, fp, "tb_f");
            mask = builder_.CreateZExt(fb, i32, "tb_mv");
        } else {
            report("input", c.lineno, 33, DiagKind::Error,
                   "TESTB requires a bit-number literal or character field in factor 2");
            return;
        }

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
    llvm::Function               *get_decimal_  = nullptr;
    llvm::Function               *get_packed_   = nullptr;  // Section C
    llvm::Function               *get_binary_   = nullptr;  // Section C
    llvm::Function               *overpunch_in_  = nullptr; // Section C (C10)
    llvm::Function               *overpunch_out_ = nullptr; // Section C (C10)
    // Section D output helpers.
    llvm::Function               *skip_fn_       = nullptr; // D13 skip/page
    llvm::Function               *page_fn_       = nullptr; // D14 page counter
    llvm::Function               *edit_word_fn_  = nullptr; // D16 edit words
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
    // Map of output filename -> its file-id Value (set during cycle entry).
    std::unordered_map<std::string, llvm::Value *> *out_ids_ = nullptr;
    // The program's O-spec records, stashed for calc-time EXCPT emission.
    const std::vector<ORecord> *outputs_ = nullptr;
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
    // True if the most recent op was a DIV whose remainder is available for a
    // following MVR. Cleared by any intervening op or by MVR itself.
    bool last_remainder_ = false;
    // Hidden global holding the DIV remainder, lazily created.
    llvm::GlobalVariable *divrem_slot_ = nullptr;
};

} // namespace

std::unique_ptr<llvm::Module> generate_module(
    const Program &prog,
    const std::string &module_name,
    llvm::LLVMContext &ctx) {

    CodeGen cg(ctx, module_name);
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
    cg.generate_linear(specs, {});
    auto mod = cg.take_module();

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        error("generated module failed verification");
        mod->print(llvm::errs(), nullptr);
    }
    return mod;
}

} // namespace rpgc
