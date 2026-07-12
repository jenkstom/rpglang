/* ========================================================================== *
 * ir.h -- ProgramIR: the shared representation every analysis module reads.
 *
 * Wraps the compiler's `rpgc::Program` (see compiler/src/program.h) with a
 * lenient parse (never aborts; hard errors become diagnostics, see
 * TOOLS_IDEAS.md §7.1) plus derived tables (symbol table, indicator table,
 * subroutine table, control-flow graph) computed once and shared by every
 * module, so no module re-derives them independently.
 * ========================================================================== */
#ifndef RPGANALYZE_IR_H
#define RPGANALYZE_IR_H

#include "program.h"
#include "source.h"
#include "diagnostics.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace analyze {

struct Diagnostic {
    std::string file;
    int line = 0;
    int col = 0;
    rpgc::DiagKind kind;
    std::string message;
};

/* ---- Symbol table ---------------------------------------------------- */

enum class SymbolKind { Input, LookAhead, DSSubfield, Array, Table, Inline, Unknown };
enum class DataFormat { Zoned, Packed, Binary, Alpha, Unknown };

const char *symbol_kind_text(SymbolKind k);
const char *data_format_text(DataFormat d);

/* One reference site to a symbol or indicator: which spec line touched it,
 * in what role, and whether it was a read or a write. */
struct RefSite {
    char        spec = ' ';       // 'C', 'I', 'O', 'F'
    int         line = 0;
    std::string role;             // "factor1" | "factor2" | "result" | "condition" | "output" | "field" | "set" | "clear" | "test"
    bool        is_write = false;
    bool        negate = false;   // for "condition" roles: N-prefixed (test-if-off)
};

/* One definition event for a symbol (a field can legally be declared more
 * than once, e.g. under two record-identification groups in the same file;
 * fields module compares these to flag inconsistent re-declarations). */
struct DefSite {
    int         line = 0;
    DataFormat  data_format = DataFormat::Unknown;
    int         length = 0;
    int         decimals = -1;
    std::string owner;
};

struct SymbolInfo {
    std::string name;             // upper-cased key
    SymbolKind  kind = SymbolKind::Unknown;
    DataFormat  data_format = DataFormat::Unknown;
    int         length = 0;       // byte length (character) or digit count (numeric)
    int         decimals = -1;    // -1 = alphameric / unspecified
    std::string owner;            // owning file or DS name; blank if none
    int         def_line = 0;
    std::vector<RefSite> refs;
    std::vector<DefSite> defs;    // every definition site (see DefSite above)
};

/* ---- Indicator table ---------------------------------------------------
 * Keyed by display label: "01".."99", "L1".."L9", "LR", "1P", "MR",
 * "OA".."OG", "OV", "U1".."U8", "H1".."H9", "KA".."KY". */

enum class IndicatorClass {
    General, ControlLevel, LastRecord, FirstPage, Matching,
    Overflow, External, Halt, FunctionKey
};

const char *indicator_class_text(IndicatorClass c);
/* Reverses rpgc::parse_indicator_token's encoding back to a display label. */
std::string indicator_label(int encoded);
IndicatorClass classify_indicator(const std::string &label);

struct IndicatorInfo {
    std::string label;
    IndicatorClass klass = IndicatorClass::General;
    std::vector<RefSite> sets;    // SETON / arithmetic HI-LO-EQ / overflow / etc.
    std::vector<RefSite> clears;  // SETOF
    std::vector<RefSite> tests;   // conditioning use (C-spec cols 9-17, O-spec conditions)
};

/* ---- Subroutine table --------------------------------------------------- */

struct Subroutine {
    std::string name;             // upper-cased
    int begin_line = 0;
    int end_line = 0;             // 0 if unterminated
    int begin_idx = -1;           // index into ProgramIR::prog.calcs
    int end_idx = -1;
    std::vector<std::string> calls;      // EXSR/CASxx targets, in call order
    std::vector<std::string> indicators_tested;  // labels tested inside the body
    std::vector<std::string> indicators_set;     // labels set inside the body
    std::vector<std::string> fields_written;     // result-field writes inside the body
};

/* ---- Control-flow graph -------------------------------------------------
 * One node per C-spec index (ProgramIR::prog.calcs). Edges are fallthrough
 * (sequential) and GOTO->TAG jumps; EXSR is not a CFG edge (it's a call that
 * returns), recorded instead in SubroutineTable. */

struct CfgEdge { int to; bool is_goto; };

struct ControlFlowGraph {
    std::vector<std::vector<CfgEdge>> succ;   // succ[i] = outgoing edges from calcs[i]
    std::vector<bool> reachable;              // reachable[i]
    std::unordered_map<std::string, int> tag_index; // TAG label -> calcs index
};

/* ---- ProgramIR ----------------------------------------------------------- */

struct ProgramIR {
    std::string path;
    std::string program_id;
    std::vector<rpgc::SourceLine> raw_lines;
    rpgc::Program prog;
    std::vector<Diagnostic> diagnostics;   // captured parse-time diagnostics
    bool load_failed = false;              // couldn't even read the file

    // Derived, built once by build():
    std::unordered_map<std::string, SymbolInfo> symbols;   // key = upper name
    std::vector<std::string> symbol_order;                 // first-seen order

    std::map<std::string, IndicatorInfo> indicators;        // key = label, sorted

    std::vector<Subroutine> subroutines;
    std::unordered_map<std::string, int> subroutine_index;  // name -> index into subroutines

    ControlFlowGraph cfg;

    static ProgramIR build(const std::string &path);

    const SymbolInfo *find_symbol(const std::string &name) const;
    const IndicatorInfo *find_indicator(const std::string &label) const;
    /* Line -> spec type ('C','F','I','O','E','H','L') from raw_lines, ' ' if OOB. */
    char spec_type_at(int line) const;
    /* Original source text of a line (1-based), empty if OOB. */
    std::string line_text(int line) const;
};

} // namespace analyze

#endif // RPGANALYZE_IR_H
