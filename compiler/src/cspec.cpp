// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * cspec.cpp -- parse Calculation Specifications.
 * ========================================================================== */
#include "cspec.h"
#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace rpgc {

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

/* Parse a 2-character indicator token into an index:
 *   "01".."99" -> 1..99
 *   "LR"       -> -1  (last record; resolved at codegen)
 *   "L1".."L9" -> -2..-10 (control levels; resolved at codegen)
 *   "1P"       -> -11 (first-page indicator, Section D)
 *   "MR"       -> -12 (matching-record indicator, Section F)
 *   "OA".."OG" -> -13..-19 (overflow indicators, Section F)
 *   "OV"       -> -20 (overflow indicator, Section F)
 *   "U1".."U8" -> -21..-28 (external indicators)
 *   "H1".."H9" -> -29..-37 (halt indicators)
 *   "KA".."KY" -> -38..-62 (function-key indicators)
 *   anything else -> 0 (not present / unsupported in this phase).
 *
 * U1-U8/H1-H9/KA-KY must resolve to a real (nonzero) index so that a calc
 * conditioned solely on one of them still participates in the AND-chain
 * (A4): parse_conditions drops any condition whose index is 0, which would
 * otherwise silently turn "condition on U1" into "unconditional".
 */
int ind_token(const std::string &raw) {
    std::string s = upper(raw);
    // trim
    auto notspace = [](unsigned char c){ return !std::isspace(c); };
    auto a = std::find_if(s.begin(), s.end(), notspace);
    auto b = std::find_if(s.rbegin(), s.rend(), notspace).base();
    if (a >= b) return 0;
    s.assign(a, b);

    if (s == "LR") return -1;
    if (s == "L0") return 0;   // L0 is always-on; not storable
    if (s == "1P") return -11; // first-page indicator (Section D, D12)
    if (s == "MR") return -12; // matching-record indicator (Section F, F20)
    // Overflow indicators OA-OG, OV (Section F, F22).
    if (s.size() == 2 && s[0] == 'O' && s[1] >= 'A' && s[1] <= 'G')
        return -13 - (s[1] - 'A');   // OA -> -13, ... OG -> -19
    if (s == "OV") return -20;
    if (s.size() == 2 && s[0] == 'L' && s[1] >= '1' && s[1] <= '9')
        return -1 - (s[1] - '0');   // L1 -> -2, L2 -> -3, ... L9 -> -10
    // External indicators U1-U8 (H-spec/F-spec-set switches).
    if (s.size() == 2 && s[0] == 'U' && s[1] >= '1' && s[1] <= '8')
        return -21 - (s[1] - '1');   // U1 -> -21, ... U8 -> -28
    // Halt indicators H1-H9.
    if (s.size() == 2 && s[0] == 'H' && s[1] >= '1' && s[1] <= '9')
        return -29 - (s[1] - '1');   // H1 -> -29, ... H9 -> -37
    // Function-key indicators KA-KY.
    if (s.size() == 2 && s[0] == 'K' && s[1] >= 'A' && s[1] <= 'Y')
        return -38 - (s[1] - 'A');   // KA -> -38, ... KY -> -62
    if (s.size() == 2 && std::isdigit((unsigned char)s[0])
                      && std::isdigit((unsigned char)s[1])) {
        int v = (s[0]-'0')*10 + (s[1]-'0');
        if (v >= 1 && v <= 99) return v;
    }
    return 0; // unsupported indicator token
}

/* Parse the three conditioning groups (cols 9-17). Each group is 3 cols wide:
 * [N]II. */
std::vector<CondInd> parse_conditions(const std::string &line) {
    std::vector<CondInd> out;
    // group starts: col 9, 12, 15 (1-based). Each spans 3 cols.
    const int starts[3] = {9, 12, 15};
    for (int s : starts) {
        std::string grp = col(line, s, s + 2); // [s..s+2] inclusive => 3 cols
        // grp[0] optional N, grp[1..2] indicator
        if (grp.size() < 3) continue;
        bool neg = (grp[0] == 'N' || grp[0] == 'n');
        std::string tok = grp.substr(neg ? 1 : 0, 2);
        int idx = ind_token(tok);
        if (idx != 0) {
            CondInd ci; ci.indicator = idx; ci.negate = neg;
            out.push_back(ci);
        }
    }
    return out;
}

/* Parse the operation opcode plus, for IFxx/DOWxx/DOUxx/CASxx, the trailing
 * two-letter comparison operator. Returns the Op and sets `cmp_out`. */
Op parse_op(const std::string &s, CmpOp *cmp_out) {
    std::string u = upper(s);
    if (cmp_out) *cmp_out = CmpOp::NONE;

    if (u == "ADD")   return Op::ADD;
    if (u == "Z-ADD") return Op::ZADD;
    if (u == "ZADD")  return Op::ZADD;   // tolerate no-dash
    if (u == "Z-SUB") return Op::ZSUB;
    if (u == "ZSUB")  return Op::ZSUB;   // tolerate no-dash
    if (u == "SETON") return Op::SETON;
    if (u == "SETOF") return Op::SETOF;
    // Phase 4:
    if (u == "COMP")  return Op::COMP;
    if (u == "GOTO")  return Op::GOTO;
    if (u == "TAG")   return Op::TAG;
    if (u == "MOVE")  return Op::MOVE;
    if (u == "MOVEL") return Op::MOVEL;
    // Phase 6a arithmetic:
    if (u == "SUB")   return Op::SUB;
    if (u == "MULT")  return Op::MULT;
    if (u == "DIV")   return Op::DIV;
    if (u == "MVR")   return Op::MVR;

    // Phase 6b structured ops with xx suffix. Strip the base, keep the suffix.
    auto starts = [&](const char *p) { return u.rfind(p, 0) == 0; };
    auto suffix = [&](int len) {
        std::string sx = u.substr(len);
        CmpOp c = CmpOp::NONE;
        if (sx.empty())            c = CmpOp::NONE;        // valid only for CAS
        else if (sx == "EQ")       c = CmpOp::EQ;
        else if (sx == "NE")       c = CmpOp::NE;
        else if (sx == "GT")       c = CmpOp::GT;
        else if (sx == "LT")       c = CmpOp::LT;
        else if (sx == "GE")       c = CmpOp::GE;
        else if (sx == "LE")       c = CmpOp::LE;
        else                       c = (CmpOp)-1;          // invalid suffix
        if (cmp_out) *cmp_out = c;
        return c;
    };
    if (starts("IF"))  { suffix(2); return Op::IF;  }
    if (starts("DOW")) { suffix(3); return Op::DOW; }
    if (starts("DOU")) { suffix(3); return Op::DOU; }
    if (starts("CAS")) { suffix(3); return Op::CAS; }
    // DO (counted loop) must come after DOW/DOU so it doesn't shadow them.
    if (u == "DO")     return Op::DO;
    if (u == "ELSE")   return Op::ELSE;
    if (u == "END")    return Op::END;
    if (u == "EXSR")   return Op::EXSR;
    if (u == "BEGSR")  return Op::BEGSR;
    if (u == "ENDSR")  return Op::ENDSR;
    if (u == "EXCPT")  return Op::EXCPT;
    // Phase 9b array operations:
    if (u == "XFOOT")  return Op::XFOOT;
    if (u == "SQRT")   return Op::SQRT;
    if (u == "LOKUP" || u == "LOOKUP") return Op::LOKUP;
    if (u == "MOVEA")  return Op::MOVEA;
    if (u == "TESTZ")  return Op::TESTZ;
    if (u == "TESTB")  return Op::TESTB;
    // Section G (G24) file access:
    if (u == "CHAIN")  return Op::CHAIN;
    if (u == "SETLL")  return Op::SETLL;
    if (u == "READE")  return Op::READE;
    if (u == "READP")  return Op::READP;
    if (u == "READ")   return Op::READ;
    // Group C: additional operation codes.
    if (u == "BITON")  return Op::BITON;
    if (u == "BITOF")  return Op::BITOF;
    if (u == "DEFN")   return Op::DEFN;
    if (u == "SORTA")  return Op::SORTA;
    if (u == "TIME")   return Op::TIME;
    if (u == "MHHZO")  return Op::MHHZO;
    if (u == "MHLZO")  return Op::MHLZO;
    if (u == "MLHZO")  return Op::MLHZO;
    if (u == "MLLZO")  return Op::MLLZO;
    // Program linkage.
    if (u == "PLIST")  return Op::PLIST;
    if (u == "PARM")   return Op::PARM;
    if (u == "CALL")   return Op::CALL;
    if (u == "EXIT")   return Op::EXIT;
    if (u == "RLABL")  return Op::RLABL;
    if (u == "RETRN")  return Op::RETRN;
    if (u == "FREE")   return Op::FREE;
    // W5: WORKSTN device operations.
    if (u == "ACQ")    return Op::ACQ;
    if (u == "REL")    return Op::REL;
    if (u == "NEXT")   return Op::NEXT;
    if (u == "POST")   return Op::POST;
    if (u == "SHTDN")  return Op::SHTDN;
    return Op::Unknown;
}

} // namespace

int parse_indicator_token(const std::string &s) {
    return ind_token(s);
}

std::vector<CSpec> parse_cspecs(const std::vector<SourceLine> &src) {
    std::vector<CSpec> out;

    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'C') continue;

        CSpec c;
        c.lineno = sl.lineno;

        c.control_level = upper(col_trim(sl.text, 7, 8));
        c.conditions    = parse_conditions(sl.text);
        c.factor1       = col_trim(sl.text, 18, 27);

        std::string optext = col_trim(sl.text, 28, 32);
        c.op_text = optext;
        c.op      = parse_op(optext, &c.cmp);

        c.factor2  = col_trim(sl.text, 33, 42);
        c.result   = col_trim(sl.text, 43, 48);

        // length (49-51), decimals (52), half-adjust (53)
        std::string len = col_trim(sl.text, 49, 51);
        if (!len.empty()) {
            try { c.result_len = std::stoi(len); }
            catch (...) { c.result_len = 0; }
        }
        std::string dec = col_trim(sl.text, 52, 52);
        if (!dec.empty() && std::isdigit((unsigned char)dec[0])) {
            c.result_dec = dec[0] - '0';
        }
        std::string ha = col_trim(sl.text, 53, 53);
        c.half_adjust = (upper(ha) == "H");

        // Resulting indicators: 54-55 (HI), 56-57 (LO), 58-59 (EQ).
        c.hi.indicator = ind_token(col(sl.text, 54, 55));
        c.lo.indicator = ind_token(col(sl.text, 56, 57));
        c.eq.indicator = ind_token(col(sl.text, 58, 59));

        if (c.op == Op::Unknown) {
            // Report only if there actually is an op token -- blanks are fine.
            // An unknown but present operation is an error (H29): silently
            // dropping a calculation hides bugs.
            if (!optext.empty()) {
                report(sl.text.empty() ? "<rpgc>" : "input", sl.lineno, 28,
                       DiagKind::Error,
                       "unsupported or unknown operation '" + optext + "'");
            }
            continue;
        }

        // Per-op validation.
        if (c.op == Op::TAG) {
            // TAG label is in factor 1 (cols 18-27).
            if (c.factor1.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "TAG requires a label name in factor 1");
            }
            // TAG cannot carry conditioning indicators (cols 9-17).
            if (!c.conditions.empty()) {
                report("input", sl.lineno, 9, DiagKind::Error,
                       "TAG cannot have conditioning indicators");
                c.conditions.clear();
            }
        }
        if (c.op == Op::GOTO && c.factor2.empty()) {
            report("input", sl.lineno, 33, DiagKind::Error,
                   "GOTO requires a target label in factor 2");
        }
        if (c.op == Op::DEFN) {
            // *LIKE DEFN: factor1 must literally be "*LIKE"; conditioning and
            // resulting indicators are not permitted (manual 106341-106375).
            if (upper(c.factor1) != "*LIKE") {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "DEFN requires '*LIKE' in factor 1");
            }
            if (c.factor2.empty() || c.result.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "*LIKE DEFN requires a source field in factor 2 and a "
                       "new field name in the result field");
            }
            if (!c.conditions.empty()) {
                report("input", sl.lineno, 9, DiagKind::Error,
                       "*LIKE DEFN cannot have conditioning indicators");
                c.conditions.clear();
            }
            if (c.hi.indicator || c.lo.indicator || c.eq.indicator) {
                report("input", sl.lineno, 54, DiagKind::Error,
                       "*LIKE DEFN cannot have resulting indicators");
            }
        }
        if (c.op == Op::SORTA) {
            // SORTA: only factor2 (the array name) is used; factor1 and the
            // result/half-adjust/resulting-indicator columns must be blank
            // (manual 124481-124515).
            if (c.factor2.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "SORTA requires an array name in factor 2");
            }
            if (!c.factor1.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "SORTA factor 1 must be blank");
            }
            if (!c.result.empty() || c.half_adjust ||
                c.hi.indicator || c.lo.indicator || c.eq.indicator) {
                report("input", sl.lineno, 43, DiagKind::Error,
                       "SORTA result field, half-adjust, and resulting "
                       "indicators must be blank");
            }
        }
        if ((c.op == Op::BITON || c.op == Op::BITOF)) {
            // BITON/BITOF: factor1, decimal positions, and half-adjust must
            // be blank (manual 105336-105362, 105207-105233).
            const char *nm = c.op == Op::BITON ? "BITON" : "BITOF";
            if (!c.factor1.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       std::string(nm) + " factor 1 must be blank");
            }
            if (c.factor2.empty() || c.result.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       std::string(nm) + " requires a bit mask in factor 2 and "
                       "a result field");
            }
        }

        if (c.op == Op::PLIST) {
            // PLIST: factor1 = list name or *ENTRY (manual 123455-123561);
            // result must be blank -- PARM lines carry the actual fields.
            if (c.factor1.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "PLIST requires a list name (or *ENTRY) in factor 1");
            }
            if (!c.result.empty()) {
                report("input", sl.lineno, 43, DiagKind::Error,
                       "PLIST result field must be blank");
            }
        }
        if (c.op == Op::PARM) {
            if (c.result.empty()) {
                report("input", sl.lineno, 43, DiagKind::Error,
                       "PARM requires a parameter field in the result field");
            }
        }
        if (c.op == Op::CALL) {
            if (c.factor2.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "CALL requires a program name in factor 2");
            }
        }
        if (c.op == Op::EXIT) {
            if (c.factor2.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "EXIT requires a subroutine name (SUBRnnnnn) in factor 2");
            } else {
                std::string u2 = upper(c.factor2);
                if (u2.rfind("SUBR", 0) != 0 || u2.size() < 5 || u2.size() > 6) {
                    report("input", sl.lineno, 33, DiagKind::Warning,
                           "EXIT target '" + c.factor2 + "' does not follow the "
                           "SUBRnnnnn naming rule (manual: 5-6 chars, first four "
                           "'SUBR')");
                }
            }
        }
        if (c.op == Op::RLABL) {
            if (c.result.empty()) {
                report("input", sl.lineno, 43, DiagKind::Error,
                       "RLABL requires a field/array/table/indicator name in "
                       "the result field");
            }
            // Manual: cols 9-17, 18-27, 33-42, 53, 54-59 must all be blank.
            if (!c.conditions.empty() || !c.factor1.empty() ||
                !c.factor2.empty() || c.half_adjust ||
                c.hi.indicator || c.lo.indicator || c.eq.indicator) {
                report("input", sl.lineno, 9, DiagKind::Error,
                       "RLABL allows only the result field (and its length/"
                       "decimals) -- conditioning, factor 1/2, half-adjust, and "
                       "resulting indicators must be blank");
            }
        }
        if (c.op == Op::FREE) {
            if (c.factor2.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "FREE requires a program name in factor 2");
            }
        }

        // W5: WORKSTN device operations (manual Chapter 27's own tables).
        if (c.op == Op::ACQ) {
            if (c.factor2.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "ACQ requires a WORKSTN file name in factor 2");
            }
        }
        if (c.op == Op::REL) {
            if (c.factor1.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "REL requires a device in factor 1");
            }
            if (c.factor2.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "REL requires a WORKSTN file name in factor 2");
            }
        }
        if (c.op == Op::NEXT) {
            if (c.factor1.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "NEXT requires a device ID in factor 1");
            }
            if (c.factor2.empty()) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "NEXT requires a WORKSTN file name in factor 2");
            }
        }
        if (c.op == Op::POST) {
            if (c.factor1.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "POST requires a device ID in factor 1");
            }
            if (c.result.empty()) {
                report("input", sl.lineno, 43, DiagKind::Error,
                       "POST requires an INFDS data structure name in the "
                       "result field");
            }
            // Manual: "Columns 33 through 42, 49 through 55, and 58 and 59
            // must be blank for a POST operation." Cols 56-57 (c.lo) are
            // excluded on purpose -- that's POST's own error-indicator slot
            // ("Columns 56 and 57 can specify an indicator that is set on
            // if an error occurs"), not one of the required-blank ranges.
            if (!c.factor2.empty() || c.result_len != 0 || c.result_dec != -1 ||
                c.half_adjust || c.hi.indicator || c.eq.indicator) {
                report("input", sl.lineno, 33, DiagKind::Error,
                       "POST factor 2, result length/decimals/half-adjust, "
                       "column 54-55, and columns 58-59 must be blank");
            }
        }
        if (c.op == Op::SHTDN) {
            if (!c.factor1.empty() || !c.factor2.empty() || !c.result.empty()) {
                report("input", sl.lineno, 18, DiagKind::Error,
                       "SHTDN factor 1, factor 2, and the result field must "
                       "be blank");
            }
            if (c.hi.indicator == 0) {
                report("input", sl.lineno, 54, DiagKind::Error,
                       "SHTDN requires a resulting indicator in columns 54-55");
            }
        }

        out.push_back(std::move(c));
    }

    // Cross-check: every GOTO target must have a matching TAG, and GOTO must
    // not cross a subroutine or control-level boundary (H27). A GOTO may only
    // target a TAG in the same scope: same subroutine (or both in the main
    // body), and a compatible control-level timing.
    //
    // First compute each spec's scope: the subroutine it lives in ("" = main
    // body) by scanning BEGSR..ENDSR ranges.
    std::vector<std::string> sub_of(out.size());
    {
        std::string cur_sub;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].op == Op::BEGSR) cur_sub = upper(out[i].factor1);
            sub_of[i] = cur_sub;
            if (out[i].op == Op::ENDSR) cur_sub.clear();
        }
    }
    // Map each TAG name to its scope {subroutine, control_level}. A duplicate
    // TAG name in the same scope is allowed (last wins for resolution); for the
    // boundary check any in-scope TAG satisfies the GOTO.
    struct TagScope { std::string sub; std::string level; };
    std::unordered_map<std::string, TagScope> tag_scope;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].op == Op::TAG && !out[i].factor1.empty())
            tag_scope[upper(out[i].factor1)] = { sub_of[i], out[i].control_level };
    }
    // Control-level "kind": detail (blank/L0) vs total (L1-L9) vs LR. A GOTO
    // must not jump between detail and total/LR timing.
    auto level_kind = [](const std::string &lv) -> char {
        if (lv.empty() || lv == "L0") return 'D';   // detail
        if (lv == "LR") return 'R';                 // last record
        return 'T';                                 // L1-L9 total
    };
    for (size_t i = 0; i < out.size(); ++i) {
        const auto &c = out[i];
        if (c.op != Op::GOTO || c.factor2.empty()) continue;
        std::string tgt = upper(c.factor2);
        auto it = tag_scope.find(tgt);
        if (it == tag_scope.end()) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "GOTO target '" + c.factor2 + "' has no matching TAG");
            continue;
        }
        // Subroutine boundary: a GOTO may not jump into or out of a subroutine.
        if (sub_of[i] != it->second.sub) {
            std::string from = sub_of[i].empty() ? "main body" : "subroutine " + sub_of[i];
            std::string to   = it->second.sub.empty() ? "main body" : "subroutine " + it->second.sub;
            report("input", c.lineno, 33, DiagKind::Error,
                   "GOTO may not cross a subroutine boundary (" + from + " -> " + to + ")");
        }
        // Control-level timing: detail vs total vs LR must match.
        char gk = level_kind(c.control_level);
        char tk = level_kind(it->second.level);
        if (gk != tk) {
            report("input", c.lineno, 33, DiagKind::Error,
                   std::string("GOTO may not cross a control-level boundary (")
                   + std::string(1, gk) + " -> " + std::string(1, tk) + ")");
        }
    }

    return out;
}

std::vector<ParamList> group_param_lists(const std::vector<CSpec> &calcs) {
    std::vector<ParamList> out;
    bool seen_entry = false;
    for (size_t i = 0; i < calcs.size(); ) {
        if (calcs[i].op != Op::PLIST) { ++i; continue; }

        ParamList pl;
        pl.lineno = calcs[i].lineno;
        std::string f1 = upper(calcs[i].factor1);
        pl.is_entry = (f1 == "*ENTRY");
        pl.name = pl.is_entry ? std::string("*ENTRY") : calcs[i].factor1;
        if (pl.is_entry) {
            if (seen_entry) {
                report("input", calcs[i].lineno, 18, DiagKind::Error,
                       "only one *ENTRY PLIST is allowed per program");
            }
            seen_entry = true;
        }

        size_t j = i + 1;
        for (; j < calcs.size() && calcs[j].op == Op::PARM; ++j) {
            ParmDecl pd;
            pd.lineno  = calcs[j].lineno;
            pd.factor1 = calcs[j].factor1;
            pd.factor2 = calcs[j].factor2;
            pd.name    = calcs[j].result;
            pd.len     = calcs[j].result_len;
            pd.dec     = calcs[j].result_dec;
            pl.parms.push_back(std::move(pd));
        }
        if (pl.parms.empty()) {
            report("input", calcs[i].lineno, 18, DiagKind::Error,
                   "PLIST '" + pl.name + "' has no following PARM lines");
        }
        out.push_back(std::move(pl));
        i = j;
    }
    return out;
}

std::vector<ExitDecl> group_exit_decls(const std::vector<CSpec> &calcs) {
    std::vector<ExitDecl> out;
    for (size_t i = 0; i < calcs.size(); ) {
        if (calcs[i].op != Op::EXIT) { ++i; continue; }

        ExitDecl ed;
        ed.lineno    = calcs[i].lineno;
        ed.subr_name = upper(calcs[i].factor2);

        size_t j = i + 1;
        for (; j < calcs.size() && calcs[j].op == Op::RLABL; ++j) {
            RlablDecl rd;
            rd.lineno = calcs[j].lineno;
            rd.name   = calcs[j].result;
            rd.len    = calcs[j].result_len;
            rd.dec    = calcs[j].result_dec;
            ed.labels.push_back(std::move(rd));
        }
        out.push_back(std::move(ed));
        i = j;
    }
    return out;
}

} // namespace rpgc
