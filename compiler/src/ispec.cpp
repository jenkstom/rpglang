/* ========================================================================== *
 * ispec.cpp -- parse Input Specifications.
 *
 * Distinguishing a record-identification line from a field-description line:
 * the field-description line carries a numeric From position in cols 44-47
 * and a field name in cols 53-58. The record-identification line has the
 * filename in cols 7-14 and (usually) a record-identifying indicator in
 * cols 19-20. In practice: if cols 44-47 parse as a number AND cols 53-58
 * hold a name, it's a field line; otherwise it's a record-id line.
 * ========================================================================== */
#include "ispec.h"
#include "cspec.h"      // for parse_indicator_token
#include "diagnostics.h"

#include <algorithm>
#include <cctype>

namespace rpgc {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

bool is_digits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}
} // namespace

ISpecs parse_ispecs(const std::vector<SourceLine> &src) {
    ISpecs out;
    std::string current_file;   // last seen record-id file name
    bool lookahead_mode = false; // set by a '**' record-id line (E19)

    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'I') continue;

        std::string fromtxt = col_trim(sl.text, 44, 47);
        std::string nametxt = col_trim(sl.text, 53, 58);

        if (!fromtxt.empty() && is_digits(fromtxt) && !nametxt.empty()) {
            // ---- field-description line ----
            ISpecField fld;
            fld.lineno = sl.lineno;
            fld.file   = current_file;
            try { fld.from = std::stoi(fromtxt); } catch (...) {}
            std::string totxt = col_trim(sl.text, 48, 51);
            if (!totxt.empty()) {
                try { fld.to = std::stoi(totxt); } catch (...) {}
            }
            if (fld.to == 0) fld.to = fld.from;   // single-position field
            std::string fmt = col_trim(sl.text, 43, 43);
            if (!fmt.empty()) {
                char fc = std::toupper((unsigned char)fmt[0]);
                if (fc == 'P' || fc == 'B') fld.data_format = fc;
            }
            std::string dec = col_trim(sl.text, 52, 52);
            if (!dec.empty() && std::isdigit((unsigned char)dec[0]))
                fld.decimals = dec[0] - '0';
            fld.name = nametxt;
            fld.control_level = upper(col_trim(sl.text, 59, 60));
            // Field-record-relation (cols 63-64) and field indicators
            // (cols 65-70): three 2-char indicator groups (+/-/0). E17/E18.
            fld.record_id = parse_indicator_token(col_trim(sl.text, 63, 64));
            fld.plus_ind  = parse_indicator_token(col_trim(sl.text, 65, 66));
            fld.minus_ind = parse_indicator_token(col_trim(sl.text, 67, 68));
            fld.zero_ind  = parse_indicator_token(col_trim(sl.text, 69, 70));
            if (lookahead_mode) out.lookahead_fields.push_back(std::move(fld));
            else                out.fields.push_back(std::move(fld));
        } else {
            // ---- record-identification line ----
            ISpecRec rec;
            rec.lineno = sl.lineno;
            rec.name   = col_trim(sl.text, 7, 14);
            current_file = rec.name;
            std::string ri = col_trim(sl.text, 19, 20);
            if (ri == "**") {
                rec.is_lookahead = true;   // E19 look-ahead marker
                lookahead_mode = true;
            } else {
                lookahead_mode = false;    // a normal record-id ends look-ahead
                if (ri.size() == 2 && std::isdigit((unsigned char)ri[0])
                                   && std::isdigit((unsigned char)ri[1])) {
                    rec.rec_indicator = (ri[0]-'0')*10 + (ri[1]-'0');
                }
            }
            // Record-identification codes (cols 21-41): three 7-column sets
            // at 21-27 / 28-34 / 35-41, each {pos(4), Not(1), C/Z/D(1), Char(1)}.
            // (E17.) An AND/OR continuation line (cols 14-16) extends the most
            // recent record type with further code-sets.
            auto parse_set = [&](int start) {
                RecCodeSet cs;
                std::string postxt = col_trim(sl.text, start, start + 3);
                if (postxt.empty()) return cs;   // unused set
                try { cs.pos = std::stoi(postxt); } catch (...) {}
                std::string n = col_trim(sl.text, start + 4, start + 4);
                cs.negate = (upper(n) == "N");
                std::string cd = col_trim(sl.text, start + 5, start + 5);
                if (!cd.empty()) cs.czd = std::toupper((unsigned char)cd[0]);
                std::string ch = col(sl.text, start + 6, start + 6);
                if (!ch.empty()) cs.ch = ch[0];
                return cs;
            };
            // AND/OR continuation: if cols 14-16 carry AND/OR, append to the
            // previous record rather than starting a new one.
            std::string rel = upper(col_trim(sl.text, 14, 16));
            bool is_cont = (rel == "AND" || rel == "OR");
            if (is_cont && !out.records.empty() &&
                out.records.back().name == rec.name) {
                ISpecRec &prev = out.records.back();
                for (int start : {21, 28, 35}) {
                    RecCodeSet cs = parse_set(start);
                    if (cs.pos > 0) prev.codes.push_back(cs);
                }
            } else {
                for (int start : {21, 28, 35}) {
                    RecCodeSet cs = parse_set(start);
                    if (cs.pos > 0) rec.codes.push_back(cs);
                }
                out.records.push_back(std::move(rec));
            }
        }
    }
    return out;
}

} // namespace rpgc
