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
     *   1..99   => general indicator slot
     */
    llvm::Value *load_resolved(int idx) {
        if (idx == -1)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), lr_, "lr");
        if (idx <= -2 && idx >= -10)
            return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_),
                                       ctl_[-idx - 1], "l" + std::to_string(-idx-1));
        return load(idx);
    }
    void store_resolved(int idx, llvm::Value *v) {
        if (idx == -1) { builder_.CreateStore(v, lr_); return; }
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

    llvm::GlobalVariable *lr() { return lr_; }
    llvm::GlobalVariable *in() { return in_; }

private:
    llvm::LLVMContext  &ctx_;
    llvm::Module       &mod_;
    llvm::IRBuilder<>  &builder_;
    llvm::GlobalVariable *in_ = nullptr;
    llvm::GlobalVariable *lr_ = nullptr;
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
        // Create globals for every E-spec array (numeric arrays only for now).
        for (const auto &a : prog.arrays) {
            if (a.decimals >= 0)
                syms_.get_or_create_array(a.name, a.entries, a.init_data);
        }
        // Compile subroutines first so EXSR calls in main can resolve.
        generate_subroutines(prog.calcs);
        if (find_primary_input(prog.files)) {
            return generate_cycle(prog);
        }
        return generate_linear(prog.calcs);
    }

    /* Linear form: C-specs run once, in order (Phase 2 behaviour). */
    bool generate_linear(const std::vector<CSpec> &specs) {
        using namespace llvm;

        // Build main.
        auto *i32 = Type::getInt32Ty(mod_->getContext());
        FunctionType *ft = FunctionType::get(i32, false);
        Function *main = Function::Create(ft, Function::ExternalLinkage,
                                          "main", mod_.get());
        BasicBlock *entry = BasicBlock::Create(mod_->getContext(), "entry", main);
        builder_.SetInsertPoint(entry);

        // Route through emit_spec_chain so structured ops (IF/ELSE/END/DOW/DOU)
        // and GOTO/TAG work in linear programs too. Level "" picks up all specs
        // whose control_level is blank.
        BasicBlock *prev = emit_spec_chain(main, entry, specs, "");

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

        get_decimal_ = Function::Create(
            FunctionType::get(i64, {ptr, i32, i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_get_decimal", mod_.get());

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
        emit_line_ = Function::Create(
            FunctionType::get(voidTy, {i32, i32}, false),
            Function::ExternalLinkage, "rpg_rt_emit_line", mod_.get());

        // Phase 10 edit-code formatter.
        edit_fn_ = Function::Create(
            FunctionType::get(i32, {i64, i32, i32, ptr, i32}, false),
            Function::ExternalLinkage, "rpg_rt_edit", mod_.get());
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
    bool generate_cycle(const Program &prog) {
        using namespace llvm;
        auto &c  = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);
        auto *i64 = Type::getInt64Ty(c);

        const FSpec *pf = find_primary_input(prog.files);
        if (!pf) return generate_linear(prog.calcs); // defensive

        int reclen = pf->reclen > 0 ? pf->reclen : 80;

        // Function + entry block.
        FunctionType *ft = FunctionType::get(i32, false);
        Function *main = Function::Create(ft, Function::ExternalLinkage,
                                          "main", mod_.get());
        BasicBlock *entry = BasicBlock::Create(c, "entry", main);
        builder_.SetInsertPoint(entry);

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
        for (const auto &fld : prog.in_fields) {
            if (fld.name.empty()) continue;
            if (fld.decimals < 0) {
                // Character field: copy raw record bytes [from-1 .. to-1].
                int len = fld.to - fld.from + 1;
                if (len < 1) continue;
                Value *dst = syms_.get_or_create_char_field(fld.name, len);
                auto *arrTy = ArrayType::get(Type::getInt8Ty(c), (unsigned)len);
                Value *dstp = builder_.CreateConstInBoundsGEP2_32(arrTy,
                    cast<GlobalVariable>(dst), 0, 0, fld.name+"_dp");
                // src = &rec_buf[from-1]
                Value *srcp = builder_.CreateInBoundsGEP(
                    Type::getInt8Ty(c), bufPtr,
                    ConstantInt::get(i32, fld.from - 1, true), fld.name+"_sp");
                builder_.CreateMemCpy(dstp, llvm::MaybeAlign(),
                    srcp, llvm::MaybeAlign(), (unsigned)len);
            } else {
                Value *v = builder_.CreateCall(get_decimal_,
                    {bufPtr,
                     ConstantInt::get(i32, reclen, true),
                     ConstantInt::get(i32, fld.from, true),
                     ConstantInt::get(i32, fld.to, true)},
                    fld.name + "_in");
                Value *v32 = builder_.CreateTrunc(v, i32, fld.name + "_i32");
                builder_.CreateStore(v32, syms_.get_or_create_field(fld.name));
            }
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
    llvm::BasicBlock *emit_output(llvm::Function *main, llvm::BasicBlock *prev,
                     const std::vector<ORecord> &records, OType want) {
        using namespace llvm;
        builder_.SetInsertPoint(prev);
        auto &c   = mod_->getContext();
        auto *i32 = Type::getInt32Ty(c);

        for (const auto &rec : records) {
            // Skip heading lines for now (1P/heading timing deferred). We only
            // handle D at detail time and T at total time.
            if (rec.type == OType::Heading) continue;
            if (rec.type != want) continue;

            // Resolve the output file id for this record.
            auto it = out_ids_->find(rec.file);
            if (it == out_ids_->end()) continue;   // no output file: skip
            Value *fid = it->second;

            // Conditioning: branch around the line if conditions don't hold.
            Value *cond = eval_conditions(inds_, builder_, rec.conditions);
            BasicBlock *dob = BasicBlock::Create(c, "out_do", main);
            BasicBlock *nxt = BasicBlock::Create(c, "out_after", main);
            builder_.CreateCondBr(cond, dob, nxt);

            builder_.SetInsertPoint(dob);
            // Build the line: line_begin(reclen). Use 132 for PRINTER default.
            builder_.CreateCall(line_begin_, {ConstantInt::get(i32, 132)});
            // Place each field/constant.
            for (const auto &f : rec.fields) {
                // Per-field conditioning (skip the field if its indicators are off);
                // Phase 7 ignores per-field conditioning for simplicity (always emit).
                if (f.is_const) {
                    Value *str = builder_.CreateGlobalStringPtr(f.text, "oconst");
                    builder_.CreateCall(line_put_str_, {
                        str,
                        ConstantInt::get(i32, (int)f.text.size(), true),
                        ConstantInt::get(i32, f.end_pos, true)});
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
                        // Numeric field.
                        Value *v = syms_.load_field(f.name);
                        Value *v64 = builder_.CreateSExt(v, Type::getInt64Ty(c), f.name+"_o");
                        if (f.edit_code != 0) {
                            // Edit code: format via runtime into a temp buffer.
                            Value *buf = builder_.CreateAlloca(
                                Type::getInt8Ty(c),
                                ConstantInt::get(i32, 64, false), f.name+"_eb");
                            Value *n = builder_.CreateCall(edit_fn_, {
                                v64,
                                ConstantInt::get(i32, (int)f.edit_code, true),
                                ConstantInt::get(i32, f.end_pos, true),
                                buf,
                                ConstantInt::get(i32, 64, true)}, f.name+"_en");
                            builder_.CreateCall(line_put_str_, {buf, n,
                                ConstantInt::get(i32, f.end_pos, true)});
                        } else {
                            builder_.CreateCall(line_put_num_, {v64,
                                ConstantInt::get(i32, f.end_pos, true)});
                        }
                    }
                }
            }
            builder_.CreateCall(emit_line_,
                {fid, ConstantInt::get(i32, rec.space_after, true)});
            builder_.CreateBr(nxt);
            prev = nxt;
            builder_.SetInsertPoint(nxt);
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
            Op op;                       // IF, DOW, or DOU
            llvm::BasicBlock *header;    // DOW loop-back target (test-at-top)
            llvm::BasicBlock *exit;      // block after the construct (merge/exit)
            const CSpec *spec;           // the opening spec (for DOU bottom-test)
            bool has_else;               // IF: an ELSE was seen
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

            if (c->op == Op::IF || c->op == Op::DOW || c->op == Op::DOU) {
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
                           "END without matching IF/DOW/DOU");
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

            // Regular op (incl. TAG/GOTO/arith/CASE later).
            prev = emit_spec(main, prev, *c);
        }

        // Unmatched openers: report but keep going.
        if (!frames.empty()) {
            error("unbalanced IFxx/DOWxx/DOUxx (missing END)");
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
                          c.op == Op::ELSE || c.op == Op::END || c.op == Op::CAS);
        if (!is_div && !is_mvr && !is_struct) last_remainder_ = false;

        switch (c.op) {
            case Op::ADD:
            case Op::SUB:
            case Op::MULT:
            case Op::DIV:  emit_binop(c);  return false;
            case Op::MVR:  emit_mvr(c);    return false;
            case Op::ZADD: emit_zadd(c);   return false;
            case Op::SETON:case Op::SETOF: emit_seton(c); return false;
            case Op::COMP:  emit_comp(c);  return false;
            case Op::MOVE:
            case Op::MOVEL: emit_move(c);  return false;
            case Op::TAG:   return false;   // marker; nothing to emit
            case Op::GOTO:  emit_goto(c);   return true;  // branches away
            case Op::EXSR:  emit_exsr(c);   return false;  // call subroutine
            case Op::XFOOT: emit_xfoot(c);  return false;
            case Op::SQRT:  emit_sqrt(c);   return false;
            case Op::LOKUP: emit_lokup(c);  return false;
            case Op::MOVEA: emit_movea(c);  return false;
            default:
                // Structured ops (IF/DOU/DOW/ELSE/END/CAS) are handled at the
                // chain level, not as individual op bodies. Unknown ops were
                // reported at parse time.
                return false;
        }
    }

    /* ADD/SUB/MULT/DIV share the "factor1 optional, result substitutes" rule.
     * Computes r = (f1 present ? f1 OP f2 : r OP f2) using signed integer math.
     *
     * Per the manual: SUB = F1-F2, MULT = F1*F2 (result len = F1+F2, but we use
     * i32 so overflow wraps silently), DIV = F1/F2 (signed integer quotient;
     * decimal-place retention is deferred with packed/decimal support). The
     * remainder of a DIV is kept in last_remainder_ for a following MVR. */
    void emit_binop(const CSpec &c) {
        using namespace llvm;
        auto *i32 = Type::getInt32Ty(mod_->getContext());

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

        // Left operand: factor1 if present, else the result field's current
        // value (the accumulate-into-result rule). For an array-element result,
        // load from that element.
        Value *lhs;
        if (!c.factor1.empty()) {
            lhs = syms_.resolve_operand(c.factor1);
        } else {
            lhs = builder_.CreateLoad(i32, rptr, c.result + "_cur");
        }

        Value *res;
        switch (c.op) {
            case Op::ADD:  res = builder_.CreateAdd (lhs, f2, "add");  break;
            case Op::SUB:  res = builder_.CreateSub (lhs, f2, "sub");  break;
            case Op::MULT: res = builder_.CreateMul (lhs, f2, "mul");  break;
            case Op::DIV: {
                // Signed integer division. Record the remainder for a possible
                // following MVR. We stash it in a hidden global rather than an
                // SSA value so MVR (which may land in a different basic block
                // inside an IF/DO body) can load it without dominance issues.
                res = builder_.CreateSDiv(lhs, f2, "div");
                Value *rem = builder_.CreateSRem(lhs, f2, "rem");
                builder_.CreateStore(rem, get_divrem_slot());
                last_remainder_ = true;   // signal "remainder saved"
                break;
            }
            default: return;
        }
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

    /* Z-ADD: r = f2 (clears result first, i.e. result = 0 + f2). */
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
        builder_.CreateStore(f2, rptr);
        emit_arith_result_indicators(inds_, builder_, c, f2);
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
        if (c.result.empty()) {
            report("input", c.lineno, 43, DiagKind::Error,
                   "MOVE requires a result field");
            return;
        }
        // Character -> character MOVE/MOVEL: right/left justified byte copy.
        Value *srcp; int srclen;
        if (syms_.resolve_char_operand(c.factor2, srcp, srclen)) {
            int dstlen;
            Value *rptr;
            if (syms_.is_char_field(c.result)) {
                dstlen = syms_.info(c.result)->length;
                rptr = syms_.get_or_create_char_field(c.result, dstlen);
            } else {
                dstlen = c.result_len > 0 ? c.result_len : srclen;
                rptr = syms_.get_or_create_char_field(c.result, dstlen);
            }
            auto *arrTy = ArrayType::get(Type::getInt8Ty(ctx), (unsigned)dstlen);
            Value *dstp = builder_.CreateConstInBoundsGEP2_32(arrTy,
                cast<GlobalVariable>(rptr), 0, 0, c.result+"_dp");
            int copylen = std::min(srclen, dstlen);
            int srcoff = 0, dstoff = 0;
            if (c.op == Op::MOVE) {
                dstoff = dstlen - copylen;
                srcoff = srclen - copylen;
            }
            auto *i8 = Type::getInt8Ty(ctx);
            Value *d = builder_.CreateInBoundsGEP(i8, dstp,
                ConstantInt::get(Type::getInt32Ty(ctx), dstoff, true));
            Value *s = builder_.CreateInBoundsGEP(i8, srcp,
                ConstantInt::get(Type::getInt32Ty(ctx), srcoff, true));
            builder_.CreateMemCpy(d, MaybeAlign(), s, MaybeAlign(),
                                  (unsigned)copylen);
            return;
        }
        // Numeric MOVE: simple value store (Phase 4 behaviour).
        Value *f2 = syms_.resolve_operand(c.factor2);
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
    llvm::Function               *get_decimal_  = nullptr;
    // Record buffer global for the cycle.
    llvm::GlobalVariable         *rec_buf_      = nullptr;
    // Runtime output-function declarations.
    llvm::Function               *open_output_  = nullptr;
    llvm::Function               *line_begin_   = nullptr;
    llvm::Function               *line_put_str_ = nullptr;
    llvm::Function               *line_put_num_ = nullptr;
    llvm::Function               *emit_line_    = nullptr;
    // Phase 10 edit-code formatter.
    llvm::Function               *edit_fn_      = nullptr;
    // Map of output filename -> its file-id Value (set during cycle entry).
    std::unordered_map<std::string, llvm::Value *> *out_ids_ = nullptr;
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
    cg.generate_linear(specs);
    auto mod = cg.take_module();

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        error("generated module failed verification");
        mod->print(llvm::errs(), nullptr);
    }
    return mod;
}

} // namespace rpgc
