/* ========================================================================== *
 * symbols.h -- symbol table for RPG fields.
 *
 * Fields come in two flavours:
 *   - numeric  : a global i32 (Phase 2-8 behaviour; up to ~9 digits).
 *   - character: a global [N x i8] array (Phase 9; alphanumeric fields declared
 *                on the I-spec with a blank decimal-position column).
 *
 * Literals in factor1/factor2 are recognised at lookup time so the codegen path
 * treats them uniformly. A numeric literal parses to ConstantInt; a quoted
 * character literal 'ABC' parses to a constant [N x i8].
 *
 * Array element access (ARR,INDEX) is also resolved here (Phase 9b).
 * ========================================================================== */
#ifndef RPGC_SYMBOLS_H
#define RPGC_SYMBOLS_H

#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace rpgc {

enum class FieldKind { None, Numeric, Character, ArrayElem, Array };

struct FieldInfo {
    FieldKind kind = FieldKind::None;
    int length = 0;                 // character length (Character kind)
    int decimals = -1;              // -1 = unspecified
    llvm::GlobalVariable *gv = nullptr;
    int array_count = 0;            // number of elements (Array kind)
    // Tables (TAB-prefixed arrays) carry a hidden 1-based "current element"
    // index global, updated by LOKUP. A bare table name resolves to the
    // element this index selects. Section B.
    bool is_table = false;
    llvm::GlobalVariable *shadow_gv = nullptr;
};

class SymbolTable {
public:
    SymbolTable(llvm::Module &m, llvm::IRBuilder<> &b) : mod_(m), builder_(b) {}

    /* Declare a numeric field (i32 global, zero-init). Idempotent. */
    llvm::Value *get_or_create_field(const std::string &name);

    /* Record the decimal-position count for a numeric field (Section C: the
     * stored i32 is a scaled integer, value = true * 10^decimals). Idempotent;
     * creates the field if needed. */
    void set_numeric_attrs(const std::string &name, int decimals);

    /* Decimal positions of a field (0 for integer/undeclared fields). */
    int field_decimals(const std::string &name) const;

    /* Declare a character field of `length` bytes ([length x i8], space-init).
     * Idempotent. */
    llvm::Value *get_or_create_char_field(const std::string &name, int length);

    /* Declare a numeric array of `count` i32 elements, initialised from
     * `init` (zeros if empty). When `is_table` the array is a table and also
     * gets a hidden current-element index global (default 1). Idempotent. */
    llvm::Value *get_or_create_array(const std::string &name, int count,
                                     const std::vector<long> &init,
                                     bool is_table = false);

    bool has_field(const std::string &name) const {
        return fields_.find(name) != fields_.end();
    }
    bool is_char_field(const std::string &name) const {
        auto it = fields_.find(name);
        return it != fields_.end() && it->second.kind == FieldKind::Character;
    }
    bool is_array(const std::string &name) const {
        auto it = fields_.find(name);
        return it != fields_.end() && it->second.kind == FieldKind::Array;
    }
    bool is_table(const std::string &name) const {
        auto it = fields_.find(name);
        return it != fields_.end() && it->second.is_table;
    }
    const FieldInfo *info(const std::string &name) const {
        auto it = fields_.find(name);
        return it != fields_.end() ? &it->second : nullptr;
    }

    /* Resolve an "ARRAY,INDEX" token. If `token` is "ARR,I", sets is_array_ref
     * true and returns the array base pointer; index_out gets the index token
     * (field name or literal). Returns the array global if it's an array ref,
     * else nullptr and leaves is_array_ref false. */
    bool parse_array_ref(const std::string &token, std::string &arr_name,
                         std::string &idx_token) const;

    llvm::Value *load_field(const std::string &name);

    /* For a table name, return a pointer to the element selected by the
     * table's current-element shadow index (1-based). Returns nullptr if
     * `name` is not a table. */
    llvm::Value *table_elem_ptr(const std::string &name);

    /* Return the shadow-index global for a table (the i32 holding the 1-based
     * current element), or nullptr if `name` is not a table. */
    llvm::GlobalVariable *table_shadow(const std::string &name);

    /* Resolve a factor token to an i32 value (numeric operand). Numeric literal
     * -> ConstantInt; numeric field -> load; character field/array -> nullptr
     * (use resolve_char_operand instead). */
    llvm::Value *resolve_operand(const std::string &token);

    /* Resolve a factor token to (ptr, length) for character operations. A
     * quoted literal 'ABC' becomes a global string; a character field returns
     * its i8* + length. Returns false if the token isn't character-typed. */
    bool resolve_char_operand(const std::string &token,
                              llvm::Value *&ptr, int &length);

private:
    llvm::Module        &mod_;
    llvm::IRBuilder<>   &builder_;
    std::unordered_map<std::string, FieldInfo> fields_;
};

} // namespace rpgc

#endif // RPGC_SYMBOLS_H
