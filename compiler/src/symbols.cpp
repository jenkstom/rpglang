/* ========================================================================== *
 * symbols.cpp -- symbol-table implementation (numeric + character fields).
 * ========================================================================== */
#include "symbols.h"
#include "diagnostics.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>

#include <cctype>

namespace rpgc {

namespace {

bool is_integer_literal(const std::string &s) {
    if (s.empty()) return false;
    std::size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}

} // namespace

llvm::Value *SymbolTable::get_or_create_field(const std::string &name) {
    auto it = fields_.find(name);
    if (it != fields_.end()) {
        // D2: a numeric data-structure subfield has no native i32 storage to
        // store into (see FieldInfo::is_ds_field) -- every write call site in
        // codegen.cpp funnels through get_or_create_field() to obtain the
        // destination pointer before CreateStore, so this is the single
        // choke point that can catch every one of them without touching each
        // opcode's own codegen individually.
        if (it->second.is_ds_field && it->second.kind == FieldKind::Numeric) {
            rpgc::error("field '" + name + "' is a numeric data-structure "
                        "subfield; this compiler supports numeric DS "
                        "subfields as read operands only (no i32-to-zoned-"
                        "decimal encoder exists to write one back) -- it "
                        "cannot be used as a calc result field");
            return ds_write_guard();
        }
        return it->second.gv;
    }

    auto *i32 = llvm::Type::getInt32Ty(mod_.getContext());
    auto *gv = new llvm::GlobalVariable(
        mod_, i32, /*isConstant=*/false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i32, 0), "rpg_" + name);
    gv->setAlignment(llvm::Align(4));
    FieldInfo fi; fi.kind = FieldKind::Numeric; fi.gv = gv;
    fields_[name] = fi;
    return gv;
}

void SymbolTable::set_numeric_attrs(const std::string &name, int decimals) {
    get_or_create_field(name);            // ensure the field exists
    auto it = fields_.find(name);
    if (it != fields_.end() && it->second.kind == FieldKind::Numeric)
        it->second.decimals = decimals;
}

int SymbolTable::field_decimals(const std::string &name) const {
    auto it = fields_.find(name);
    if (it == fields_.end()) return 0;
    return it->second.decimals < 0 ? 0 : it->second.decimals;
}

llvm::Value *SymbolTable::get_or_create_char_field(const std::string &name,
                                                   int length) {
    auto it = fields_.find(name);
    if (it != fields_.end()) return it->second.gv;
    if (length < 1) length = 1;
    auto *arrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(mod_.getContext()),
                                       (unsigned)length);
    // Initialise with spaces (0x20).
    std::vector<llvm::Constant *> init(length,
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(mod_.getContext()), 0x20));
    auto *gv = new llvm::GlobalVariable(
        mod_, arrTy, /*isConstant=*/false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(arrTy, init), "rpgc_" + name);
    FieldInfo fi; fi.kind = FieldKind::Character; fi.length = length; fi.gv = gv;
    fields_[name] = fi;
    return gv;
}

llvm::Value *SymbolTable::get_or_create_array(const std::string &name, int count,
                                              const std::vector<long> &init,
                                              bool is_table) {
    auto it = fields_.find(name);
    if (it != fields_.end()) return it->second.gv;
    if (count < 1) count = 1;
    auto *i32 = llvm::Type::getInt32Ty(mod_.getContext());
    auto *arrTy = llvm::ArrayType::get(i32, (unsigned)count);
    std::vector<llvm::Constant *> inits;
    for (int i = 0; i < count; ++i) {
        long v = (i < (int)init.size()) ? init[i] : 0;
        inits.push_back(llvm::ConstantInt::get(i32, (uint32_t)v, true));
    }
    auto *gv = new llvm::GlobalVariable(
        mod_, arrTy, /*isConstant=*/false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(arrTy, inits), "rpga_" + name);
    gv->setAlignment(llvm::Align(4));
    FieldInfo fi; fi.kind = FieldKind::Array; fi.array_count = count; fi.gv = gv;
    // Tables track the 1-based index of the element selected by the most
    // recent successful LOKUP (defaults to 1, the first element).
    if (is_table) {
        fi.is_table = true;
        fi.shadow_gv = new llvm::GlobalVariable(
            mod_, i32, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i32, 1), "rpgs_" + name);
        fi.shadow_gv->setAlignment(llvm::Align(4));
    }
    fields_[name] = fi;
    return gv;
}

llvm::Value *SymbolTable::get_or_create_char_array(
        const std::string &name, int count, int entry_len,
        const std::vector<std::string> &init, bool is_table) {
    auto it = fields_.find(name);
    if (it != fields_.end()) return it->second.gv;
    if (count < 1) count = 1;
    if (entry_len < 1) entry_len = 1;
    auto &ctx = mod_.getContext();
    auto *i8 = llvm::Type::getInt8Ty(ctx);
    auto *elemTy = llvm::ArrayType::get(i8, (unsigned)entry_len);
    auto *arrTy = llvm::ArrayType::get(elemTy, (unsigned)count);
    std::vector<llvm::Constant *> elems;
    for (int i = 0; i < count; ++i) {
        std::string s = (i < (int)init.size()) ? init[i] : std::string();
        if ((int)s.size() < entry_len) s.resize((size_t)entry_len, ' ');
        else if ((int)s.size() > entry_len) s.resize((size_t)entry_len);
        std::vector<llvm::Constant *> bytes;
        bytes.reserve(entry_len);
        for (char c : s)
            bytes.push_back(llvm::ConstantInt::get(i8, (uint8_t)c));
        elems.push_back(llvm::ConstantArray::get(elemTy, bytes));
    }
    auto *gv = new llvm::GlobalVariable(
        mod_, arrTy, /*isConstant=*/false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(arrTy, elems), "rpgca_" + name);
    FieldInfo fi;
    fi.kind = FieldKind::Array;
    fi.array_count = count;
    fi.length = entry_len;
    fi.is_char_array = true;
    fi.gv = gv;
    if (is_table) {
        auto *i32 = llvm::Type::getInt32Ty(ctx);
        fi.is_table = true;
        fi.shadow_gv = new llvm::GlobalVariable(
            mod_, i32, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i32, 1), "rpgs_" + name);
        fi.shadow_gv->setAlignment(llvm::Align(4));
    }
    fields_[name] = fi;
    return gv;
}

bool SymbolTable::parse_array_ref(const std::string &token,
                                  std::string &arr_name,
                                  std::string &idx_token) const {
    auto comma = token.find(',');
    if (comma == std::string::npos) return false;
    arr_name = token.substr(0, comma);
    // trim
    auto notspace = [](unsigned char ch){ return !std::isspace(ch); };
    auto a = std::find_if(arr_name.begin(), arr_name.end(), notspace);
    auto b = std::find_if(arr_name.rbegin(), arr_name.rend(), notspace).base();
    arr_name.assign(a, b);
    idx_token = token.substr(comma + 1);
    a = std::find_if(idx_token.begin(), idx_token.end(), notspace);
    b = std::find_if(idx_token.rbegin(), idx_token.rend(), notspace).base();
    idx_token.assign(a, b);
    return is_array(arr_name);
}

llvm::Value *SymbolTable::load_field(const std::string &name) {
    auto it = fields_.find(name);
    if (it == fields_.end()) return nullptr;
    if (it->second.kind == FieldKind::Numeric) {
        if (it->second.is_ds_field) return decode_ds_numeric(it->second, name);
        return builder_.CreateLoad(
            llvm::Type::getInt32Ty(mod_.getContext()), it->second.gv, name+"_v");
    }
    return nullptr;   // character fields have no scalar "load"
}

llvm::Value *SymbolTable::decode_ds_numeric(const FieldInfo &fi,
                                            const std::string &name) {
    auto &ctx  = mod_.getContext();
    auto *i32  = llvm::Type::getInt32Ty(ctx);
    auto *i64  = llvm::Type::getInt64Ty(ctx);
    auto *ptrTy = llvm::PointerType::get(ctx, 0);
    auto fnTy = llvm::FunctionType::get(i64, {ptrTy, i32, i32, i32}, false);
    auto callee = mod_.getOrInsertFunction("rpg_rt_get_decimal", fnTy);
    auto *dsArrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx),
                                         (unsigned)fi.ds_len);
    llvm::Value *base = builder_.CreateConstInBoundsGEP2_32(
        dsArrTy, fi.ds_base, 0, 0, name + "_dsp");
    llvm::Value *v64 = builder_.CreateCall(callee, {
        base,
        llvm::ConstantInt::get(i32, fi.ds_len, true),
        llvm::ConstantInt::get(i32, fi.ds_offset + 1, true),
        llvm::ConstantInt::get(i32, fi.ds_offset + fi.length, true)},
        name + "_dsv");
    return builder_.CreateTrunc(v64, i32, name + "_dsv32");
}

llvm::GlobalVariable *SymbolTable::ds_write_guard() {
    if (!ds_write_guard_gv_) {
        auto *i32 = llvm::Type::getInt32Ty(mod_.getContext());
        ds_write_guard_gv_ = new llvm::GlobalVariable(
            mod_, i32, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i32, 0), "rpg_ds_write_guard");
    }
    return ds_write_guard_gv_;
}

void SymbolTable::declare_ds_char_subfield(const std::string &name,
                                           llvm::GlobalVariable *ds_base,
                                           int ds_len, int ds_offset,
                                           int length) {
    if (fields_.find(name) != fields_.end()) return;
    FieldInfo fi;
    fi.kind = FieldKind::Character;
    fi.length = length;
    fi.is_ds_field = true;
    fi.ds_base = ds_base;
    fi.ds_len = ds_len;
    fi.ds_offset = ds_offset;
    fields_[name] = fi;
}

void SymbolTable::declare_ds_numeric_subfield(const std::string &name,
                                              llvm::GlobalVariable *ds_base,
                                              int ds_len, int ds_offset,
                                              int length, int decimals) {
    if (fields_.find(name) != fields_.end()) return;
    FieldInfo fi;
    fi.kind = FieldKind::Numeric;
    fi.decimals = decimals;
    fi.length = length;
    fi.is_ds_field = true;
    fi.ds_base = ds_base;
    fi.ds_len = ds_len;
    fi.ds_offset = ds_offset;
    fields_[name] = fi;
}

llvm::GlobalVariable *SymbolTable::table_shadow(const std::string &name) {
    auto it = fields_.find(name);
    return (it != fields_.end() && it->second.is_table) ? it->second.shadow_gv
                                                        : nullptr;
}

llvm::Value *SymbolTable::table_elem_ptr(const std::string &name) {
    auto it = fields_.find(name);
    if (it == fields_.end() || !it->second.is_table) return nullptr;
    // A9: this path assumes i32 elements; an alphameric table's current
    // element isn't addressable this way (no numeric LOKUP/element ops on
    // char arrays yet -- see emit_lokup's explicit rejection).
    if (it->second.is_char_array) return nullptr;
    auto *i32   = llvm::Type::getInt32Ty(mod_.getContext());
    auto *arrTy = llvm::ArrayType::get(i32, (unsigned)it->second.array_count);
    // current index is 1-based; subtract one for the 0-based GEP.
    llvm::Value *cur = builder_.CreateLoad(i32, it->second.shadow_gv, name+"_ci");
    llvm::Value *idx0 = builder_.CreateSub(cur, llvm::ConstantInt::get(i32, 1, true),
                                           name+"_ci0");
    return builder_.CreateInBoundsGEP(arrTy, it->second.gv,
        {llvm::ConstantInt::get(i32, 0), idx0}, name+"_ep");
}

llvm::Value *SymbolTable::resolve_operand(const std::string &token) {
    if (token.empty()) return nullptr;
    if (is_integer_literal(token)) {
        int v = std::stoi(token);
        return llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(mod_.getContext()), (uint32_t)v, true);
    }
    // Array element access: "ARR,INDEX".
    std::string an, it_tok;
    if (parse_array_ref(token, an, it_tok)) {
        auto *i32 = llvm::Type::getInt32Ty(mod_.getContext());
        const FieldInfo *fi = info(an);
        // A9: an alphameric array has no numeric element form; the caller
        // (e.g. eval_cmp_op) should fall back to resolve_char_operand.
        if (!fi || fi->is_char_array) return nullptr;
        auto *arrTy = llvm::ArrayType::get(i32, (unsigned)fi->array_count);
        // Index is 1-based in RPG.
        llvm::Value *idx;
        if (is_integer_literal(it_tok)) {
            idx = llvm::ConstantInt::get(i32, (uint32_t)(std::stoi(it_tok) - 1), true);
        } else {
            // field name holding the index
            llvm::Value *fv = resolve_operand(it_tok);
            idx = fv ? builder_.CreateSub(fv, llvm::ConstantInt::get(i32, 1, true),
                                          "idx0")
                     : llvm::ConstantInt::get(i32, 0);
        }
        llvm::Value *gep = builder_.CreateInBoundsGEP(
            arrTy, fi->gv, {llvm::ConstantInt::get(i32, 0), idx}, an+"_el");
        return builder_.CreateLoad(i32, gep, an+"_v");
    }
    // A bare table name (no explicit index) resolves to the table's current
    // element, selected by its LOKUP shadow index.
    if (auto *tep = table_elem_ptr(token))
        return builder_.CreateLoad(llvm::Type::getInt32Ty(mod_.getContext()),
                                   tep, token+"_tv");
    auto it = fields_.find(token);
    if (it == fields_.end()) {
        // Auto-create as numeric (Phase 2 behaviour for undeclared fields).
        get_or_create_field(token);
        it = fields_.find(token);
    }
    if (it->second.kind == FieldKind::Numeric) {
        if (it->second.is_ds_field) return decode_ds_numeric(it->second, token);
        return builder_.CreateLoad(
            llvm::Type::getInt32Ty(mod_.getContext()), it->second.gv, token+"_v");
    }
    return nullptr;   // character field used as numeric: caller's problem
}

bool SymbolTable::resolve_char_operand(const std::string &token,
                                       llvm::Value *&ptr, int &length) {
    if (token.empty()) return false;
    if (token.front() == '\'') {
        // Quoted literal: strip quotes, collapse '' -> '.
        std::string s = token.substr(1);
        if (!s.empty() && s.back() == '\'') s.pop_back();
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\'' && i+1 < s.size() && s[i+1] == '\'') { out += '\''; ++i; }
            else out += s[i];
        }
        length = (int)out.size();
        if (length < 1) { out = " "; length = 1; }
        // Create a global [length x i8] with the bytes (no NUL needed; length known).
        auto *arrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(mod_.getContext()),
                                           (unsigned)length);
        std::vector<llvm::Constant *> bytes;
        for (char c : out)
            bytes.push_back(llvm::ConstantInt::get(
                llvm::Type::getInt8Ty(mod_.getContext()), (uint8_t)c));
        auto *gv = new llvm::GlobalVariable(
            mod_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, bytes), "lit");
        ptr = builder_.CreateConstInBoundsGEP2_32(arrTy, gv, 0, 0, "litp");
        return true;
    }
    auto it = fields_.find(token);
    if (it != fields_.end() && it->second.kind == FieldKind::Character) {
        if (it->second.is_ds_field) {
            // D2: an alphameric DS subfield is a byte-range VIEW into the
            // parent DS buffer, not independent storage -- GEP the parent's
            // base pointer by this subfield's offset. Every caller of
            // resolve_char_operand (MOVE/MOVEL/MOVEA/TESTB/BITON/O-spec
            // print/etc.) already treats the returned ptr as a plain i8*, so
            // this makes read AND write on DS subfields work for free.
            auto *dsArrTy = llvm::ArrayType::get(
                llvm::Type::getInt8Ty(mod_.getContext()), (unsigned)it->second.ds_len);
            llvm::Value *base = builder_.CreateConstInBoundsGEP2_32(
                dsArrTy, it->second.ds_base, 0, 0, token + "_dsp");
            ptr = builder_.CreateInBoundsGEP(
                llvm::Type::getInt8Ty(mod_.getContext()), base,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(mod_.getContext()),
                                       (uint32_t)it->second.ds_offset, true),
                token + "_p");
            length = it->second.length;
            return true;
        }
        auto *arrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(mod_.getContext()),
                                           (unsigned)it->second.length);
        ptr = builder_.CreateConstInBoundsGEP2_32(arrTy, it->second.gv, 0, 0,
                                                  token + "_p");
        length = it->second.length;
        return true;
    }
    // A9: a bare alphameric array/table name used as a character operand (the
    // common MOVEA-into-a-table idiom) is addressed as one flat contiguous
    // byte buffer -- [count x [entry_len x i8]] storage is laid out exactly
    // that way in memory.
    if (it != fields_.end() && it->second.kind == FieldKind::Array &&
        it->second.is_char_array) {
        auto &ctx = mod_.getContext();
        auto *i32 = llvm::Type::getInt32Ty(ctx);
        auto *elemTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx),
                                            (unsigned)it->second.length);
        auto *arrTy = llvm::ArrayType::get(elemTy, (unsigned)it->second.array_count);
        llvm::Value *idx[] = {
            llvm::ConstantInt::get(i32, 0),
            llvm::ConstantInt::get(i32, 0),
            llvm::ConstantInt::get(i32, 0),
        };
        ptr = builder_.CreateInBoundsGEP(arrTy, it->second.gv, idx, token + "_cap");
        length = it->second.array_count * it->second.length;
        return true;
    }
    return false;
}

} // namespace rpgc
