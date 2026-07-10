/* ========================================================================== *
 * cspec.cpp -- parse Calculation Specifications.
 * ========================================================================== */
#include "cspec.h"
#include "diagnostics.h"

#include <algorithm>
#include <cctype>
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
 *   "LR"       -> -1 (last record; resolved at codegen)
 *   "L1".."L9" -> -2..-10 (control levels; resolved at codegen)
 *   anything else -> 0 (not present / unsupported in this phase).
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
    if (s.size() == 2 && s[0] == 'L' && s[1] >= '1' && s[1] <= '9')
        return -1 - (s[1] - '0');   // L1 -> -2, L2 -> -3, ... L9 -> -10
    if (s.size() == 2 && std::isdigit((unsigned char)s[0])
                      && std::isdigit((unsigned char)s[1])) {
        int v = (s[0]-'0')*10 + (s[1]-'0');
        if (v >= 1 && v <= 99) return v;
    }
    return 0; // unsupported indicator for now (H1.., etc.)
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
    if (u == "ELSE")   return Op::ELSE;
    if (u == "END")    return Op::END;
    if (u == "EXSR")   return Op::EXSR;
    if (u == "BEGSR")  return Op::BEGSR;
    if (u == "ENDSR")  return Op::ENDSR;
    // Phase 9b array operations:
    if (u == "XFOOT")  return Op::XFOOT;
    if (u == "SQRT")   return Op::SQRT;
    if (u == "LOKUP" || u == "LOOKUP") return Op::LOKUP;
    if (u == "MOVEA")  return Op::MOVEA;
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
            if (!optext.empty()) {
                report(sl.text.empty() ? "<rpgc>" : "input", sl.lineno, 28,
                       DiagKind::Warning,
                       "unsupported or unknown operation '" + optext +
                       "' (ignored)");
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

        out.push_back(std::move(c));
    }

    // Cross-check: every GOTO target must have a matching TAG.
    std::unordered_set<std::string> tags;
    for (const auto &c : out)
        if (c.op == Op::TAG) tags.insert(upper(c.factor1));
    for (const auto &c : out) {
        if (c.op == Op::GOTO && !c.factor2.empty()
                              && tags.find(upper(c.factor2)) == tags.end()) {
            report("input", c.lineno, 33, DiagKind::Error,
                   "GOTO target '" + c.factor2 + "' has no matching TAG");
        }
    }

    return out;
}

} // namespace rpgc
