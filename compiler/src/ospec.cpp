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

/* Decode an O-spec skip code (cols 19-20 or 21-22) into an absolute line
 * number. Returns 0 for blank. Encoding (manual p.455): 01-99 = line 1-99;
 * A0-A9 = 100-109; B0-B2 = 110-112. */
int decode_skip(const std::string &s) {
    if (s.empty()) return 0;
    char c0 = s[0];
    if (std::isdigit((unsigned char)c0)) {
        if (s.size() >= 2 && std::isdigit((unsigned char)s[1]))
            return (c0 - '0') * 10 + (s[1] - '0');
        return c0 - '0';
    }
    char cu = std::toupper((unsigned char)c0);
    if (cu == 'A' && s.size() >= 2 && std::isdigit((unsigned char)s[1]))
        return 100 + (s[1] - '0');
    if (cu == 'B' && s.size() >= 2 && std::isdigit((unsigned char)s[1]))
        return 110 + (s[1] - '0');
    return 0;
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
    std::string last_file;        // carries forward across continuation record lines

    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'O') continue;

        const std::string &t = sl.text;

        // F1: an AND/OR continuation line (cols 14-16) extends the most
        // recent record line's conditioning beyond the 3-indicator-per-line
        // limit (manual 88200-88217, 88491-88493). It carries no Type (col
        // 15) of its own -- must be checked before the record/field-line
        // heuristic below, or it falls into the field-line path and is
        // silently dropped (the bug this fixes).
        std::string relAndOr = upper(col_trim(t, 14, 16));
        if (relAndOr == "AND" || relAndOr == "OR") {
            if (!current) {
                report("input", sl.lineno, 14, DiagKind::Warning,
                       "AND/OR output-conditioning line with no preceding "
                       "record line (ignored)");
                continue;
            }
            std::vector<CondInd> grp = parse_o_conditions(t);
            if (relAndOr == "AND") {
                if (current->conditions.empty())
                    current->conditions.push_back({});
                auto &last = current->conditions.back();
                last.insert(last.end(), grp.begin(), grp.end());
            } else {
                current->conditions.push_back(std::move(grp));
            }
            continue;
        }

        // Record line: column 15 has H/D/T/E (or U for an update record, G25).
        std::string typech = upper(col_trim(t, 15, 15));
        // Cols 16-18 may carry ADD / DEL / UPDATE for disk update records (G25).
        std::string op16  = upper(col_trim(t, 16, 18));
        bool is_record = (!typech.empty() &&
                          (typech[0]=='H' || typech[0]=='D' ||
                           typech[0]=='T' || typech[0]=='E' || typech[0]=='U'));

        if (is_record) {
            ORecord rec;
            rec.lineno = sl.lineno;
            // Filename (cols 7-14): present on the first record line per file;
            // may be omitted on later record lines and carries forward. We
            // remember the last named file so blank-file continuation lines
            // (including type-E EXCPT lines) inherit it.
            std::string fname = col_trim(t, 7, 14);
            if (!fname.empty()) last_file = fname;
            rec.file = last_file;
            switch (typech[0]) {
                case 'H': rec.type = OType::Heading;   break;
                case 'D': rec.type = OType::Detail;    break;
                case 'T': rec.type = OType::Total;     break;
                case 'E': rec.type = OType::Exception; break;
                case 'U': rec.type = OType::Detail; rec.rec_op = ORecOp::Update; break;
            }
            // ADD/DEL/UPDATE in cols 16-18 override the record operation (G25).
            if (op16 == "ADD")         rec.rec_op = ORecOp::Add;
            else if (op16 == "DEL")    rec.rec_op = ORecOp::Delete;
            else if (op16 == "UPDATE") rec.rec_op = ORecOp::Update;
            else {
                // F2: col 16 alone (not part of a 3-char ADD/DEL/UPDATE
                // mnemonic) may carry F (fetch overflow) or R (release
                // device), manual 88310-88356. Checked on col 16 in
                // isolation, not the 16-18 window used above, since cols
                // 17-18 independently hold the space-before/after digits.
                std::string c16 = upper(col_trim(t, 16, 16));
                if (c16 == "F") {
                    rec.fetch_overflow = true;
                } else if (c16 == "R") {
                    // Release is meaningful only for a WORKSTN display
                    // station or ICF session (manual: "release the device
                    // ... after output has been written"); this compiler
                    // has no WORKSTN/ICF device support at all (E8 already
                    // hard-errors those F-spec devices), so a real R here
                    // would silently do nothing. Loud error, same E8/E5
                    // precedent, instead of a silent no-op.
                    report("input", sl.lineno, 16, DiagKind::Error,
                           "O-spec col 16 'R' (release device) requires a "
                           "WORKSTN/ICF file, which this compiler does not "
                           "support");
                }
            }
            // Space before/after (cols 17/18): digit 0-3.
            std::string sb = col_trim(t, 17, 17);
            if (!sb.empty() && std::isdigit((unsigned char)sb[0]))
                rec.space_before = sb[0] - '0';
            std::string sa = col_trim(t, 18, 18);
            if (!sa.empty() && std::isdigit((unsigned char)sa[0])) {
                rec.space_after = sa[0] - '0';
            } else if (col_trim(t,17,18).empty()) {
                rec.space_after = 1;   // cols 17-18 blank => single-space after
            } else {
                rec.space_after = 0;
            }
            // Skip before/after (cols 19-22): absolute line number to skip to.
            rec.skip_before = decode_skip(col_trim(t, 19, 20));
            rec.skip_after  = decode_skip(col_trim(t, 21, 22));
            // F1: this line's own indicators are group 0; AND/OR continuation
            // lines (handled above, before this record line is even reached
            // on later iterations) extend or add to this list.
            rec.conditions.push_back(parse_o_conditions(t));
            // EXCPT name (cols 32-37) is only meaningful for type-E records.
            if (rec.type == OType::Exception) {
                rec.except_name = col_trim(t, 32, 37);
            }
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

        // Edit code (col 38) — parsed early so we can tell an edit word (blank
        // col 38 + field name + quoted cols 45-70) from a plain constant.
        std::string ec = col_trim(t, 38, 38);
        if (!ec.empty()) fld.edit_code = ec[0];

        std::string nameArea = col_trim(t, 32, 37);
        // Strip apostrophes / collapse '' -> ' from a quoted region.
        auto unquote = [&](const std::string &region) -> std::string {
            std::string raw = region;
            auto last = raw.find_last_of('\'');
            if (last != std::string::npos && last != 0)
                raw = raw.substr(1, last - 1);
            else
                raw = raw.substr(1);
            std::string outc;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\'' && i + 1 < raw.size() && raw[i+1] == '\'') {
                    outc += '\''; ++i;
                } else outc += raw[i];
            }
            return outc;
        };

        if (fld.edit_code != 0 && !nameArea.empty()) {
            // A13: field name + edit code both present. Columns 45-47 (a
            // narrower region than the 45-70 constant/edit-word area used
            // when no edit code is present) may carry a floating fill
            // character: a bare '*' (asterisk fill) or a quoted currency
            // symbol like '$' (manual 62678-62762, Figures 166-168). Without
            // this branch the quoted fill was previously mis-read as an
            // unrelated standalone constant, silently dropping the field
            // name/edit-code association entirely.
            fld.name = nameArea;
            std::string fillArea = col(t, 45, 47);
            std::string fillTrim = col_trim(t, 45, 47);
            if (fillTrim == "*") {
                fld.fill_char = '*';
            } else if (!fillArea.empty() && fillArea[0] == '\'') {
                std::string un = unquote(fillArea);
                if (!un.empty()) fld.fill_char = un[0];
            }
        } else {
            // Constant? Look for a leading apostrophe in cols 45-70.
            std::string conArea = col(t, 45, 70);
            bool quoted = (!conArea.empty() && conArea[0] == '\'');
            if (quoted && !nameArea.empty()) {
                // Edit word (D16): a numeric field with a quoted pattern in
                // cols 45-70 and no edit code.
                fld.name = nameArea;
                fld.edit_word = unquote(conArea);
            } else if (quoted) {
                // Plain constant.
                fld.is_const = true;
                fld.text = unquote(conArea);
            } else if (!nameArea.empty()) {
                // D3: *AUTO (manual Ch. 26) requests Auto Report Feature
                // field expansion, which this compiler does not implement
                // (see uspec.h). Left unchecked, *AUTO is just an ordinary
                // (nonexistent) field name that silently prints nothing --
                // catch it here with a clear diagnostic instead, the same
                // *LIKE-precedent pattern cspec.cpp's DEFN parsing uses.
                if (upper(nameArea) == "*AUTO") {
                    report("input", sl.lineno, 32, DiagKind::Error,
                           "*AUTO (Auto Report Feature field expansion, "
                           "manual Ch. 26) is not implemented; use an "
                           "explicit field name instead");
                    continue;
                }
                fld.name = nameArea;
            } else {
                // Neither constant nor field name; ignore.
                continue;
            }
        }

        // End position (cols 40-43), right-justified.
        std::string ep = col_trim(t, 40, 43);
        if (!ep.empty()) {
            try { fld.end_pos = std::stoi(ep); } catch (...) {}
        }
        // Blank after (col 39).
        std::string ba = upper(col_trim(t, 39, 39));
        fld.blank_after = (ba == "B");

        current->fields.push_back(std::move(fld));
    }
    return out;
}

} // namespace rpgc
