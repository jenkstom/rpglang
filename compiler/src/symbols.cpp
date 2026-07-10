/* ========================================================================== *
 * symbols.cpp -- symbol-table implementation (numeric + character fields).
 * ========================================================================== */
#include "symbols.h"

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
    if (it != fields_.end()) return it->second.gv;

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
                                              const std::vector<long> &init) {
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

llvm::Value *SymbolTable::load_field(const std::string &name) {    auto it = fields_.find(name);
    if (it == fields_.end()) return nullptr;
    if (it->second.kind == FieldKind::Numeric) {
        return builder_.CreateLoad(
            llvm::Type::getInt32Ty(mod_.getContext()), it->second.gv, name+"_v");
    }
    return nullptr;   // character fields have no scalar "load"
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
    auto it = fields_.find(token);
    if (it == fields_.end()) {
        // Auto-create as numeric (Phase 2 behaviour for undeclared fields).
        get_or_create_field(token);
        it = fields_.find(token);
    }
    if (it->second.kind == FieldKind::Numeric) {
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
        auto *arrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(mod_.getContext()),
                                           (unsigned)it->second.length);
        ptr = builder_.CreateConstInBoundsGEP2_32(arrTy, it->second.gv, 0, 0,
                                                  token + "_p");
        length = it->second.length;
        return true;
    }
    return false;
}

} // namespace rpgc
