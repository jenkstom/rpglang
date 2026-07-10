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
            std::string dec = col_trim(sl.text, 52, 52);
            if (!dec.empty() && std::isdigit((unsigned char)dec[0]))
                fld.decimals = dec[0] - '0';
            fld.name = nametxt;
            fld.control_level = upper(col_trim(sl.text, 59, 60));
            out.fields.push_back(std::move(fld));
        } else {
            // ---- record-identification line ----
            ISpecRec rec;
            rec.lineno = sl.lineno;
            rec.name   = col_trim(sl.text, 7, 14);
            current_file = rec.name;
            std::string ri = col_trim(sl.text, 19, 20);
            if (ri.size() == 2 && std::isdigit((unsigned char)ri[0])
                               && std::isdigit((unsigned char)ri[1])) {
                rec.rec_indicator = (ri[0]-'0')*10 + (ri[1]-'0');
            }
            out.records.push_back(std::move(rec));
        }
    }
    return out;
}

} // namespace rpgc
