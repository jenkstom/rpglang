// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * cspec.h -- parse Calculation Specifications into typed structs.
 *
 * The C-spec layout (see docs/SPEC_MAP.md) is column-oriented. Each parsed
 * CSpec carries the original line number for diagnostics plus the fields we
 * need for code generation.
 * ========================================================================== */
#ifndef RPGC_CSPEC_H
#define RPGC_CSPEC_H

#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

/* One conditioning indicator: an index into the indicator array (see IndMap)
 * plus a negation flag. An "empty" condition has indicator == 0. */
struct CondInd {
    int  indicator = 0;   // 0 = not present
    bool negate    = false;
};

/* One resulting-indicator slot (HI/LO/EQ on arithmetic, or the three SETON
 * slots). indicator == 0 means the slot is empty. */
struct ResultInd {
    int indicator = 0;
};

enum class Op {
    Unknown,
    ADD,    // r = f1 ? f1+f2 : r+f2
    ZADD,   // r = f2
    ZSUB,   // r = -f2 (zero and subtract)
    SETON,  // turn on indicators named in result slots
    SETOF,  // turn off indicators named in result slots
    // Phase 4:
    COMP,   // compare factor1 vs factor2; set HI/LO/EQ indicators
    GOTO,   // branch to the TAG named in factor2
    TAG,    // label marker; factor1 holds the label name
    MOVE,   // right-justified copy of factor2 into result
    MOVEL,  // left-justified copy of factor2 into result
    // Phase 6a arithmetic:
    SUB,    // r = f1 ? f1-f2 : r-f2
    MULT,   // r = f1*f2 (f1 optional: r = r*f2)
    DIV,    // r = f1/f2 quotient (f1 optional: r = r/f2)
    MVR,    // r = remainder of the immediately preceding DIV
    // Phase 6b structured ops:
    IF, DOW, DOU, DO, ELSE, END, CAS,  // with .cmp holding the xx operator
    // Phase 8b subroutines:
    EXSR,   // call subroutine named in factor2
    BEGSR,  // begin subroutine (factor1 = name)
    ENDSR,  // end subroutine (return)
    // Exception output:
    EXCPT,  // write type-E O-records named in factor2 (blank => unnamed E lines)
    // Phase 9b array operations:
    XFOOT,  // sum array (factor2) into result
    SQRT,   // square root of factor2 into result
    // Phase 10:
    LOKUP,  // search array (factor2) for factor1; set HI/LO/EQ
    MOVEA,  // move array <-> field (left-justified)
    // Compare/test (zone & bit):
    TESTZ,  // test zone of leftmost char of result; set HI/LO/EQ
    TESTB,  // test bits of result field per factor2 mask; set HI/LO/EQ
    // Section G (G24) file access:
    CHAIN,  // random read: f1=key/RRN, f2=file; cols 54-55 = no-record indicator
    SETLL,  // position f2 at key >= f1 (lower limit)
    READE,  // read next from f2 if key == f1; cols 58-59 = EOF/unequal indicator
    READ,   // read next from f2 (full-proc/demand); cols 58-59 = EOF indicator
    READP,  // read prior from f2; cols 58-59 = beginning-of-file indicator
    // Group C: additional operation codes.
    BITON,  // set on bits named in factor2 within the result field
    BITOF,  // set off bits named in factor2 within the result field
    DEFN,   // *LIKE DEFN: define result with factor2's attributes (+/- len delta)
    SORTA,  // sort array (factor2) in place per its E-spec sequence flag
    TIME,   // store time-of-day (and optionally date) into result
    MHHZO,  // move zone: leftmost byte of f2 -> leftmost byte of result
    MHLZO,  // move zone: leftmost byte of f2 -> rightmost byte of result
    MLHZO,  // move zone: rightmost byte of f2 -> leftmost byte of result
    MLLZO,  // move zone: rightmost byte of f2 -> rightmost byte of result
    // Program linkage:
    PLIST,  // declare a named parameter list (factor1 = name or *ENTRY); grouped
            // with the PARM lines that follow it, see group_param_lists()
    PARM,   // one parameter within the preceding PLIST; result = the parameter
            // field, factor1/factor2 = optional copy-in/copy-out shim fields
    CALL,   // call another compiled RPG program by name (factor2); result
            // optionally names the PLIST supplying the parameters
    EXIT,   // call an external (non-RPG) subroutine named in factor2
            // (SUBRnnnnn); grouped with the RLABL lines that follow it
    RLABL,  // one parameter passed to the preceding EXIT's external subroutine
    RETRN,  // return control to the caller (or end the program if not called)
    FREE,   // drop a called program's "initialized" state (factor2 = name)
};

/* For IFxx/DOWxx/DOUxx/CASxx, the comparison operator suffix (xx). NONE marks
 * an unconditional CASxx (blank xx). */
enum class CmpOp { NONE, EQ, NE, GT, LT, GE, LE };

/* Convert a CmpOp to its two-letter text, or "" for NONE. */
inline const char *cmp_text(CmpOp c) {
    switch (c) {
        case CmpOp::EQ: return "EQ"; case CmpOp::NE: return "NE";
        case CmpOp::GT: return "GT"; case CmpOp::LT: return "LT";
        case CmpOp::GE: return "GE"; case CmpOp::LE: return "LE";
        case CmpOp::NONE: return "";
    }
    return "";
}

struct CSpec {
    int lineno = 0;

    // "when to run"
    std::string control_level;          // cols 7-8 (L1..L9 / LR / AN / OR / blank)
    std::vector<CondInd> conditions;    // cols 9-17, AND of all non-empty

    // "what to do"
    std::string factor1;                // cols 18-27 (trimmed)
    Op          op = Op::Unknown;       // cols 28-32
    std::string op_text;                // raw op text for diagnostics
    CmpOp       cmp = CmpOp::NONE;      // xx suffix for IFxx/DOWxx/DOUxx/CASxx
    std::string factor2;                // cols 33-42
    std::string result;                 // cols 43-48
    int         result_len = 0;         // cols 49-51
    int         result_dec = -1;        // col 52 (-1 = unspecified)
    bool        half_adjust = false;    // col 53 == 'H'

    // "how to test the result"
    ResultInd hi, lo, eq;               // cols 54-55 / 56-57 / 58-59
};

/* One PARM line within a PLIST. `name` is the
 * parameter field itself (the by-address parameter, cols 43-48); factor1/
 * factor2 are the optional copy-in/copy-out shim fields (cols 18-27/33-42). */
struct ParmDecl {
    int lineno = 0;
    std::string factor1;   // optional copy-in source (caller) / extra copy-in
    std::string factor2;   // optional copy-out target (caller) / extra copy-out
    std::string name;      // the parameter field (by-address)
    int len = 0;
    int dec = -1;
};

/* A PLIST and the PARM lines grouped under it. `is_entry` marks the single
 * `*ENTRY PLIST` a called program declares for its own formal parameters. */
struct ParamList {
    int lineno = 0;
    std::string name;      // list name, or "*ENTRY"
    bool is_entry = false;
    std::vector<ParmDecl> parms;
};

/* One RLABL line following an EXIT. */
struct RlablDecl {
    int lineno = 0;
    std::string name;      // field/DS/array/table/indicator name
    int len = 0;
    int dec = -1;
};

/* An EXIT and the RLABL lines grouped under it. */
struct ExitDecl {
    int lineno = 0;
    std::string subr_name; // SUBRnnnnn, upper-cased
    std::vector<RlablDecl> labels;
};

/* Parse all C-specs out of a source file. Non-C lines are ignored. Returns the
 * vector by value; on hard parse errors it still returns what it could and
 * reports via diagnostics. */
std::vector<CSpec> parse_cspecs(const std::vector<SourceLine> &src);

/* Group PLIST/PARM runs into ParamList records (same shape as CASxx...END
 * grouping in codegen.cpp). Reports a hard error for a PLIST with no PARM
 * lines, and for a second `*ENTRY PLIST`. Call once per program after
 * parse_cspecs(). */
std::vector<ParamList> group_param_lists(const std::vector<CSpec> &calcs);

/* Group EXIT/RLABL runs into ExitDecl records. Call once per program after
 * parse_cspecs(). */
std::vector<ExitDecl> group_exit_decls(const std::vector<CSpec> &calcs);

/* Helper: turn a column text like "47" or "LR" into an indicator index. 0 if
 * blank/unknown. 'LR' is reserved index -1 here and resolved at codegen. */
int parse_indicator_token(const std::string &s);

} // namespace rpgc

#endif // RPGC_CSPEC_H
