/* ========================================================================== *
 * ospec.cpp -- parse Output Specifications.
 *
 * Discriminating record vs field lines: a record line has a non-blank Type
 * (col 15 = H/D/T/E); a field line leaves cols 7-31 blank and carries a field
 * name (32-37) or a constant (45-70) with an end position (40-43).
 * ========================================================================== */
#include "ospec.h"
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
} // namespace

/* Parse the three conditioning groups (cols 23-31). Each group is 3 cols.
 * Note: lines may be short (trailing whitespace trimmed), so a group may be
 * fewer than 3 chars — handle that gracefully. */
static std::vector<CondInd> parse_o_conditions(const std::string &line) {
    std::vector<CondInd> out;
    const int starts[3] = {23, 26, 29};
    for (int s : starts) {
        std::string grp = col_trim(line, s, s + 2);
        if (grp.empty()) continue;
        bool neg = (grp[0] == 'N' || grp[0] == 'n');
        std::string tok = neg ? grp.substr(1, 2) : grp.substr(0, 2);
        int idx = parse_indicator_token(tok);
        if (idx != 0) {
            CondInd ci; ci.indicator = idx; ci.negate = neg;
            out.push_back(ci);
        }
    }
    return out;
}

std::vector<ORecord> parse_ospecs(const std::vector<SourceLine> &src) {
    std::vector<ORecord> out;
    ORecord *current = nullptr;   // record line that subsequent field lines attach to

    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'O') continue;

        const std::string &t = sl.text;

        // Record line: column 15 has H/D/T/E.
        std::string typech = upper(col_trim(t, 15, 15));
        bool is_record = (!typech.empty() &&
                          (typech[0]=='H' || typech[0]=='D' ||
                           typech[0]=='T' || typech[0]=='E'));

        if (is_record) {
            ORecord rec;
            rec.lineno = sl.lineno;
            rec.file   = col_trim(t, 7, 14);
            switch (typech[0]) {
                case 'H': rec.type = OType::Heading;   break;
                case 'D': rec.type = OType::Detail;    break;
                case 'T': rec.type = OType::Total;     break;
                case 'E': rec.type = OType::Exception; break;
            }
            // Space before/after (cols 17/18): digit 0-3.
            std::string sb = col_trim(t, 17, 17);
            if (!sb.empty() && std::isdigit((unsigned char)sb[0]))
                rec.space_before = sb[0] - '0';
            std::string sa = col_trim(t, 18, 18);
            if (!sa.empty() && std::isdigit((unsigned char)sa[0])) {
                rec.space_after = sa[0] - '0';
            } else if (col_trim(t,17,22).empty()) {
                rec.space_after = 1;   // all blank => single-space after
            } else {
                rec.space_after = 0;
            }
            rec.conditions = parse_o_conditions(t);
            out.push_back(std::move(rec));
            current = &out.back();
            continue;
        }

        // Field line: attach to the most recent record line.
        if (!current) {
            report("input", sl.lineno, 0, DiagKind::Warning,
                   "O-spec field line with no preceding record line (ignored)");
            continue;
        }

        OField fld;
        fld.lineno = sl.lineno;
        fld.conditions = parse_o_conditions(t);

        // Constant? Look for a leading apostrophe in cols 45-70.
        std::string conArea = col(t, 45, 70);
        std::string nameArea = col_trim(t, 32, 37);
        if (!conArea.empty() && conArea[0] == '\'') {
            // Strip surrounding apostrophes; collapse '' -> '.
            std::string raw = conArea;
            // take from first ' to the last ' on the line region
            auto last = raw.find_last_of('\'');
            if (last != std::string::npos && last != 0) {
                raw = raw.substr(1, last - 1);
            } else {
                raw = raw.substr(1);
            }
            std::string outc;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\'' && i + 1 < raw.size() && raw[i+1] == '\'') {
                    outc += '\''; ++i;
                } else outc += raw[i];
            }
            fld.is_const = true;
            fld.text = outc;
        } else if (!nameArea.empty()) {
            fld.name = nameArea;
        } else {
            // Neither constant nor field name; ignore.
            continue;
        }

        // End position (cols 40-43), right-justified.
        std::string ep = col_trim(t, 40, 43);
        if (!ep.empty()) {
            try { fld.end_pos = std::stoi(ep); } catch (...) {}
        }
        // Blank after (col 39).
        std::string ba = upper(col_trim(t, 39, 39));
        fld.blank_after = (ba == "B");
        // Edit code (col 38).
        std::string ec = col_trim(t, 38, 38);
        if (!ec.empty()) fld.edit_code = ec[0];

        current->fields.push_back(std::move(fld));
    }
    return out;
}

} // namespace rpgc
