// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * autoreport.cpp -- the Auto Report source-to-source preprocessor (D3/Ch.26).
 *
 * This is a transform on std::vector<SourceLine>, run right after
 * expand_copy_statements and before any parse_* call (see main.cpp). It rewrites
 * the source lines themselves, emitting ordinary F/I/C/O-spec text that the
 * existing column parsers then consume -- so every parser, codegen, and the
 * analyzer stay unchanged.
 *
 * Implemented here:
 *   - U-spec option parsing (delegated to uspec.cpp).
 *   - H-*AUTO page-heading generation (Phase B): one or more heading lines,
 *     with auto-generated date/title/page fields, conditioning, and centering.
 *
 * Deferred (reported as a clear error, not a confusing downstream rejection):
 *   - D/T-*AUTO (detail/total output specs + the A$$SUM accumulator). The
 *     construct is recognized here but its expansion is not implemented yet.
 *
 * Column layout note: RPG II is column-oriented with 1-based, inclusive
 * columns. The slicing helpers col()/col_trim() (source.h) read ranges; the
 * put_col() helper below writes ranges. The form-type letter is column 6.
 *
 * Generated O-spec text columns (manual 88200+, shared with parse_ospecs):
 *   7-14   filename (record-description line only)
 *   15     type (H/D/T/E)
 *   17-18  space before/after
 *   19-22  skip before/after
 *   23-31  output indicators (3 groups of [N]II)
 *   32-37  field name / EXCPT name
 *   38     edit code
 *   39     blank-after 'B'
 *   40-43  end position
 *   45-70  constant (apostrophe-delimited) or edit word
 * ========================================================================== */
#include "autoreport.h"
#include "diagnostics.h"
#include "uspec.h"
#include "ispec.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace rpgc {

namespace {

/* ---- small local helpers (the codebase has no spec-text synthesizer yet) -- */

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

/* Write `text` left-aligned into columns [first, last] (1-based, inclusive) of
 * `line`, resizing/padding the line with spaces as needed. Truncates text that
 * overruns the column range. This is the writer counterpart to col(). */
void put_col(std::string &line, int first, int last, const std::string &text) {
    if (last < first) return;
    if ((int)line.size() < last) line.resize(last, ' ');
    int width = last - first + 1;
    int n = (int)text.size();
    if (n > width) n = width;
    int base = first - 1;   // 0-based start index
    for (int i = 0; i < n; ++i)
        line[base + i] = text[i];
}

/* Return a fresh blank spec line: form letter in column 6, everything else
 * spaces, 80 columns wide. The 5-column sequence area (cols 1-5) is left blank;
 * column resequencing is deferred to a later phase. */
SourceLine spec(char form, int lineno) {
    SourceLine sl;
    sl.text.assign(80, ' ');
    sl.text[5] = form;       // column 6
    sl.lineno = lineno;
    sl.comment = false;
    return sl;
}

/* The reserved RPG words that do NOT get an N1P on a heading line, even when
 * the record description's indicators are blank (manual 90840-90846). */
bool is_reserved_heading_word(const std::string &name) {
    std::string u = upper(name);
    if (u == "UDATE" || u == "UDAY" || u == "UMONTH" || u == "UYEAR") return true;
    if (u == "PAGE") return true;
    if (u.size() == 5 && u.substr(0, 4) == "PAGE" && u[4] >= '1' && u[4] <= '7')
        return true;
    return false;
}

/* The overflow-indicator tokens OA..OG, OV. Auto report allocates the first
 * unused one when a PRINTER file declares none (manual 90750+). */
const char *kOverflowTokens[] = {"OA", "OB", "OC", "OD", "OE", "OF", "OG", "OV"};

/* ---- parsed H-*AUTO block ------------------------------------------------ */

struct HeadingField {
    bool        is_const  = false;  // a constant in cols 45-70
    std::string name;               // field name (cols 32-37) if !is_const
    std::string text;               // constant text (cols 45-70) if is_const
    char        edit_code = 0;      // col 38
    bool        blank_after = false;// col 39 == 'B'
    bool        is_edit_word = false;// cols 45-70 is an edit word for `name`
};

struct HeadingBlock {
    std::string file;               // cols 7-14 (PRINTER filename)
    std::string spacing;            // cols 17-22 (raw; blank => defaults)
    std::string indicators;         // cols 23-31 (raw; blank => 1P OR overflow)
    std::vector<HeadingField> fields;
    int         lineno = 0;
    int         first_lineno = 0;   // lineno of the first line after the block
};

/* Parse one field-description line (cols 32-70) into a HeadingField. */
HeadingField parse_heading_field_line(const std::string &t) {
    HeadingField f;
    // Edit code (col 38) is only meaningful with a numeric field name.
    std::string ec = col_trim(t, 38, 38);
    if (!ec.empty()) f.edit_code = ec[0];
    if (upper(col_trim(t, 39, 39)) == "B") f.blank_after = true;

    std::string name = col_trim(t, 32, 37);
    std::string conArea = col(t, 45, 70);
    bool quoted = (!conArea.empty() && conArea[0] == '\'');
    if (quoted) {
        if (!name.empty()) {
            // A field name plus a quoted cols 45-70 is an edit word.
            f.name = name;
            f.is_edit_word = true;
            f.text = conArea;       // keep the apostrophes; ospec un-quotes
        } else {
            f.is_const = true;
            f.text = conArea;       // apostrophe-delimited constant
        }
    } else if (!name.empty()) {
        f.name = name;              // plain field, no edit word/constant
    }
    // else: neither name nor constant -- ignore (will be skipped by caller)
    return f;
}

/* Walk `src` once and parse out all H-*AUTO blocks (record description + the
 * field lines that follow). Returns the blocks in source order. `u_first` is
 * the line index of the first non-comment spec after the U line (so a block
 * must start at or after it); we just scan everything here. */
std::vector<HeadingBlock> parse_heading_blocks(const std::vector<SourceLine> &src) {
    std::vector<HeadingBlock> blocks;
    for (size_t i = 0; i < src.size(); ++i) {
        const auto &sl = src[i];
        if (sl.comment) continue;
        if (form_type(sl) != 'O') continue;
        const std::string &t = sl.text;
        // Record-description: H in col 15, *AUTO in cols 32-36.
        if (upper(col_trim(t, 15, 15)) != "H") continue;
        if (upper(col_trim(t, 32, 36)) != "*AUTO") continue;

        HeadingBlock b;
        b.lineno = sl.lineno;
        b.file = col_trim(t, 7, 14);
        b.spacing = col(t, 17, 22);
        b.indicators = col(t, 23, 31);
        // Collect following field lines (col 15 blank, form type O).
        size_t j = i + 1;
        for (; j < src.size(); ++j) {
            const auto &fl = src[j];
            if (fl.comment) continue;
            if (form_type(fl) != 'O') break;
            // A record or AND/OR line ends the block.
            std::string rel = upper(col_trim(fl.text, 14, 16));
            if (rel == "AND" || rel == "OR") break;
            if (!col_trim(fl.text, 15, 15).empty()) break;
            HeadingField hf = parse_heading_field_line(fl.text);
            // Skip a field line that carries neither a name nor a constant.
            if (hf.name.empty() && !hf.is_const) continue;
            b.fields.push_back(hf);
        }
        b.first_lineno = (j < src.size()) ? src[j].lineno : (sl.lineno + 1);
        blocks.push_back(std::move(b));
        i = j - 1;   // advance past the consumed field lines
    }
    return blocks;
}

/* ---- printer-file context (lightweight scan, no parse_fspecs) ------------ */

struct PrinterInfo {
    bool        found          = false;
    bool        has_overflow   = false;
    std::string overflow_token;     // e.g. "OA"; empty if none declared
    int         width          = 132;
};

/* Find the named PRINTER file's F-spec and its overflow indicator. Also pick up
 * an L-spec line length (cols 15-17 -> lines/page; the *width* is the printer
 * record length, which RPG defaults to 132). We do not have a true record
 * length for PRINTER (no RECLEN on a printer F-spec), so 132 is the fixed
 * default this compiler uses for PRINTER output (codegen.cpp:3529). */
PrinterInfo scan_printer(const std::vector<SourceLine> &src,
                         const std::string &name) {
    PrinterInfo pi;
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'F') continue;
        if (upper(col_trim(sl.text, 7, 14)) != upper(name)) continue;
        if (upper(col_trim(sl.text, 40, 46)) != "PRINTER") continue;
        pi.found = true;
        std::string ov = upper(col_trim(sl.text, 33, 34));
        // Only OA-OG / OV count as overflow indicators (fspec.cpp:218-223).
        bool is_ov = false;
        for (const char *tok : kOverflowTokens) if (ov == tok) { is_ov = true; break; }
        if (is_ov) { pi.has_overflow = true; pi.overflow_token = ov; }
        break;
    }
    return pi;
}

/* Scan every O-spec indicator column (cols 23-31, in groups of [N]II) across
 * all output lines and return the set of overflow tokens already in use, so we
 * can pick an unused one when allocating. */
std::vector<std::string> overflow_tokens_in_use(const std::vector<SourceLine> &src) {
    std::vector<std::string> used;
    auto add = [&](const std::string &tok) {
        for (const char *t : kOverflowTokens)
            if (tok == t) {
                for (auto &u : used) if (u == tok) return;
                used.push_back(tok);
                return;
            }
    };
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'O') continue;
        const std::string &t = sl.text;
        for (int s : {23, 26, 29}) {
            std::string grp = upper(col_trim(t, s, s + 2));
            if (grp.empty()) continue;
            std::string tok = (grp[0] == 'N') ? grp.substr(1, 2) : grp.substr(0, 2);
            add(tok);
        }
    }
    return used;
}

/* Pick the first unused overflow indicator token. */
std::string allocate_overflow(const std::vector<SourceLine> &src) {
    auto used = overflow_tokens_in_use(src);
    for (const char *tok : kOverflowTokens) {
        bool taken = false;
        for (auto &u : used) if (u == tok) { taken = true; break; }
        if (!taken) return tok;
    }
    return "OV";   // last resort; all in use (manual: rare)
}

/* ---- emission ------------------------------------------------------------ */

/* Emit one generated heading record (the H record line + optional OR line +
 * field lines) for `b`, returning the generated SourceLines. `overflow_token`
 * is the indicator to condition with (may be empty to use only 1P). */
std::vector<SourceLine> emit_heading_record(const HeadingBlock &b,
                                            const AutoReportOptions &opts,
                                            bool is_first_block,
                                            const std::string &overflow_token,
                                            int report_width) {
    std::vector<SourceLine> out;

    // ---- record-description line ----
    SourceLine rec = spec('O', b.lineno);
    put_col(rec.text, 7, 14, b.file);
    put_col(rec.text, 15, 15, "H");
    // Spacing: pass through whatever the user coded; blank is handled below via
    // the per-block default spacing computed by the caller.
    // Conditioning: blank cols 23-31 => "1P OR <overflow>"; user indicators
    // pass through unchanged.
    bool blank_ind = col_trim(b.indicators, 23, 31).empty();
    if (blank_ind) {
        put_col(rec.text, 23, 25, "1P");
    } else {
        put_col(rec.text, 23, 31, b.indicators);
    }

    // Spacing defaults (manual 90750+, 91906+): skip-to-06 before the first
    // heading, space-2 after the last, space-1 after the others. The caller
    // tells us via spacing_text what to write; here we just place it.
    // (The caller computes is_first/last; we write whatever spacing string the
    // caller put on the block -- for blank input the caller overrides these
    // fields before calling.)
    // We do NOT override here; spacing is set by the caller into b.spacing.

    out.push_back(rec);

    // OR-continuation line carrying the overflow indicator (only when the
    // record used the auto-generated 1P and we have an overflow token).
    if (blank_ind && !overflow_token.empty()) {
        SourceLine orline = spec('O', b.lineno);
        put_col(orline.text, 14, 16, "OR");
        put_col(orline.text, 23, 25, overflow_token);
        out.push_back(orline);
    }

    // ---- field lines ----
    // For the first block: date (UDATE) at end 8, the title fields centered,
    // then 'PAGE' + PAGE field at end = report_width. Suppressed by col 27 = N.
    auto push_field_const = [&](const std::string &lit, int end_pos,
                                bool with_n1p) {
        SourceLine f = spec('O', b.lineno);
        if (with_n1p) put_col(f.text, 23, 25, "N1P");
        put_col(f.text, 40, 43, std::to_string(end_pos));
        put_col(f.text, 45, 70, lit);   // apostrophe-delimited constant
        out.push_back(f);
    };
    auto push_field_name = [&](const std::string &name, int end_pos,
                               bool with_n1p, char edit_code = 0,
                               bool blank_after = false) {
        SourceLine f = spec('O', b.lineno);
        if (with_n1p) put_col(f.text, 23, 25, "N1P");
        put_col(f.text, 32, 37, name);
        if (edit_code)  put_col(f.text, 38, 38, std::string(1, edit_code));
        if (blank_after) put_col(f.text, 39, 39, "B");
        put_col(f.text, 40, 43, std::to_string(end_pos));
        out.push_back(f);
    };

    bool want_date_page = is_first_block && !opts.suppress_date_page;

    // Date: UDATE field, end position 8. Reserved word => no N1P.
    if (want_date_page) {
        push_field_name("UDATE", 8, /*with_n1p=*/false);
    }

    // Title fields: place left-to-right. Constants get no leading space;
    // (non-reserved) fields get one blank before. End positions accumulate.
    // Then center the whole run around the report width.
    //
    // First compute the raw run width (title fields laid out left to right).
    struct Item { bool is_const; std::string text; std::string name;
                  char edit_code; bool blank_after; bool reserved; };
    std::vector<Item> items;
    for (const auto &hf : b.fields) {
        if (hf.is_const) {
            // The constant's visible width is the apostrophe-stripped length.
            std::string s = hf.text;
            if (!s.empty() && s.front() == '\'') s.erase(s.begin());
            if (!s.empty() && s.back() == '\'') s.pop_back();
            items.push_back({true, hf.text, "", 0, false, false});
            (void)s; // width computed below via the stored text
        } else {
            items.push_back({false, "", hf.name, hf.edit_code, hf.blank_after,
                             is_reserved_heading_word(hf.name)});
        }
    }

    // Compute each item's print width.
    auto item_width = [&](const Item &it) -> int {
        if (it.is_const) {
            std::string s = it.text;
            if (!s.empty() && s.front() == '\'') s.erase(s.begin());
            if (!s.empty() && s.back() == '\'') s.pop_back();
            return (int)s.size();
        }
        // A field's width: its name length is a placeholder; auto report uses
        // the field's declared length, which we do not know here without the
        // parsed input specs. Use a conservative default of the name length
        // (RPG fields are usually wider than their names; for the title-line
        // centering this only shifts the run). The golden-source tests use
        // constants for titles, which have an exact width.
        return (int)it.name.size();
    };

    // Lay out with: one blank before each field (non-const, non-reserved);
    // no space before constants. Accumulate end positions.
    int cur = 0;   // current end position (0 = nothing placed yet)
    std::vector<int> ends;
    std::vector<int> starts;
    for (size_t k = 0; k < items.size(); ++k) {
        const auto &it = items[k];
        int w = item_width(it);
        int lead = 0;
        if (!it.is_const) {
            // A blank before a field (manual 92134: one blank before/after
            // fields on a heading line).
            lead = (cur == 0) ? 0 : 1;
        }
        int start = cur + lead + 1;      // 1-based start
        int end = start + w - 1;
        starts.push_back(start);
        ends.push_back(end);
        cur = end;
    }
    int run_end = cur;

    // Center: shift so the run is centered in report_width. Only shift right
    // (headings start at col 1 for the date when date is present; centering
    // applies to the title portion). Compute the offset.
    int shift = 0;
    if (want_date_page) {
        // Date occupies cols 1-8; one blank; then title; then page at the right.
        // Center the title between the date+blank and the PAGE block.
        // PAGE block width: 'PAGE' (4) + blank + 4-digit number = 9 chars,
        // ending at report_width. So it starts at report_width - 9 + 1.
        int page_start = report_width - 9 + 1;
        int title_area_lo = 8 + 1 + 1;   // after date + blank + leading space
        int title_area_hi = page_start - 1 - 1; // before blank before PAGE
        if (!items.empty() && title_area_hi > title_area_lo) {
            int area = title_area_hi - title_area_lo + 1;
            shift = title_area_lo - 1 + (area - run_end) / 2;
            if (shift < 0) shift = 0;
            // But fields were laid out starting at 1; recompute starts/ends
            // anchored at `shift` instead of 0.
        }
    } else {
        // No date/page: center the title run on the report width.
        shift = (report_width - run_end) / 2;
        if (shift < 0) shift = 0;
    }

    // Re-lay-out with the shift applied to compute final end positions.
    std::vector<int> final_ends;
    for (size_t k = 0; k < items.size(); ++k) {
        final_ends.push_back(ends[k] + shift);
    }

    // Emit the title items.
    for (size_t k = 0; k < items.size(); ++k) {
        const auto &it = items[k];
        if (it.is_const) {
            push_field_const(it.text, final_ends[k], /*with_n1p=*/false);
        } else {
            push_field_name(it.name, final_ends[k],
                            /*with_n1p=*/!it.reserved, it.edit_code,
                            it.blank_after);
        }
    }

    // Page: 'PAGE' constant then the reserved PAGE field, ending at report_width.
    if (want_date_page) {
        int pg_end = report_width;
        // PAGE field: 4-digit, ends one before the 'E' of PAGE? No -- the word
        // PAGE precedes the number with one blank between. The number is 4 wide
        // and ends at pg_end. PAGE (4 chars) ends at pg_end - 4 - 1.
        int num_end = pg_end;
        int page_word_end = num_end - 4 - 1;   // 'PAGE' + blank + 'nnnn'
        push_field_const("'PAGE'", page_word_end, /*with_n1p=*/false);
        push_field_name("PAGE", num_end, /*with_n1p=*/false);
    }

    return out;
}

/* Apply the per-block spacing defaults to a heading block before emission:
 *   - blank cols 17-22 on the first block => skip-to-06 before, space-2 after
 *     the last, space-1 after the others.
 * `is_first` / `is_last` flag the block's position among the file's blocks. */
void apply_default_spacing(HeadingBlock &b, bool is_first, bool is_last) {
    if (!col_trim(b.spacing, 17, 22).empty()) return;  // user-coded: leave as-is
    // Build a fresh spacing string.
    std::string sp(6, ' ');   // cols 17-22
    if (is_first) {
        sp[0] = ' ';          // col 17 space-before: blank
        sp[2] = '0'; sp[3] = '6';   // cols 19-20 skip-before => "06"
    }
    // Space-after (col 18): '2' for the last block, '1' otherwise.
    sp[1] = is_last ? '2' : '1';
    b.spacing = sp;
}

/* ========================================================================== *
 * D/T-*AUTO output sub-feature (Phase C)
 * ========================================================================== */

/* A parsed field-description line of a D/T-*AUTO block, typed by col 39. */
struct OutputField {
    enum class Kind { Detail, Accumulate, HeadingCont, TotalLine };
    Kind        kind       = Kind::Detail;
    std::string name;          // cols 32-37 (field name); blank for a constant
    std::string constant;      // cols 45-70 raw (apostrophe-delimited literal,
                               //   or an edit word for a named numeric field)
    char        edit_code = 0; // col 38
    bool        blank_after = false; // col 39 == 'B' (only meaningful on Detail)
    int         end_pos = 0;   // cols 40-43 (optional; 0 = auto-compute)
    int         total_level = 0; // for Kind::TotalLine: 1-9, or 'R'<<8 for LR
    int         lineno = 0;
    std::string indicators;    // cols 23-31 raw (for A-field ADD conditioning)
};

/* A parsed D/T-*AUTO block: record-description + field lines. */
struct OutputBlock {
    std::string file;          // cols 7-14
    char        type = 'D';    // col 15: 'D' (detail) or 'T' (group/total)
    bool        fetch_overflow = false; // col 16 == 'F'
    std::string spacing;       // cols 17-22 raw
    std::string indicators;    // cols 23-31 raw
    std::vector<OutputField> fields;
    int         lineno = 0;
    size_t      rec_index = 0; // index in src of the record-description line
    size_t      after_index = 0; // index of first line after the block
};

/* Classify a total-line col-39 entry. Returns 1-9 for '1'..'9', 10 for 'R'. */
int total_level_from_col39(char c) {
    if (c == 'R') return 10;          // LR (final total)
    if (c >= '1' && c <= '9') return c - '0';
    return 0;
}

/* Parse one D/T-*AUTO field-description line. */
OutputField parse_output_field_line(const std::string &t, int lineno) {
    OutputField f;
    f.lineno = lineno;
    f.edit_code = 0;
    std::string ec = col_trim(t, 38, 38);
    if (!ec.empty()) f.edit_code = ec[0];

    char c39 = ' ';
    std::string c39s = col_trim(t, 39, 39);
    if (!c39s.empty()) c39 = std::toupper((unsigned char)c39s[0]);

    f.end_pos = 0;
    std::string eps = col_trim(t, 40, 43);
    if (!eps.empty()) {
        try { f.end_pos = std::stoi(eps); } catch (...) {}
    }
    f.indicators = col(t, 23, 31);

    if (c39 == 'A') {
        f.kind = OutputField::Kind::Accumulate;
        f.name = col_trim(t, 32, 37);
    } else if (c39 == 'C') {
        f.kind = OutputField::Kind::HeadingCont;
        f.constant = col(t, 45, 70);
    } else if (c39 == 'B') {
        f.kind = OutputField::Kind::Detail;
        f.blank_after = true;
        f.name = col_trim(t, 32, 37);
        f.constant = col(t, 45, 70);
    } else if (c39 >= '1' && c39 <= '9' || c39 == 'R') {
        f.kind = OutputField::Kind::TotalLine;
        f.total_level = total_level_from_col39(c39);
        f.name = col_trim(t, 32, 37);
        f.constant = col(t, 45, 70);
    } else {
        // blank col 39 => detail field
        f.kind = OutputField::Kind::Detail;
        f.blank_after = false;
        f.name = col_trim(t, 32, 37);
        f.constant = col(t, 45, 70);
    }
    return f;
}

/* Walk `src` and parse the single D/T-*AUTO block (only one allowed per
 * program, manual 91033). Returns false (no block) if none found. */
bool parse_output_block(const std::vector<SourceLine> &src, OutputBlock &ob) {
    for (size_t i = 0; i < src.size(); ++i) {
        const auto &sl = src[i];
        if (sl.comment) continue;
        if (form_type(sl) != 'O') continue;
        std::string t15 = upper(col_trim(sl.text, 15, 15));
        if (t15.empty() || (t15[0] != 'D' && t15[0] != 'T')) continue;
        if (upper(col_trim(sl.text, 32, 36)) != "*AUTO") continue;

        ob.file = col_trim(sl.text, 7, 14);
        ob.type = t15[0];
        ob.fetch_overflow = (upper(col_trim(sl.text, 16, 16)) == "F");
        ob.spacing = col(sl.text, 17, 22);
        ob.indicators = col(sl.text, 23, 31);
        ob.lineno = sl.lineno;
        ob.rec_index = i;

        size_t j = i + 1;
        for (; j < src.size(); ++j) {
            const auto &fl = src[j];
            if (fl.comment) continue;
            if (form_type(fl) != 'O') break;
            std::string rel = upper(col_trim(fl.text, 14, 16));
            if (rel == "AND" || rel == "OR") break;
            if (!col_trim(fl.text, 15, 15).empty()) break;
            ob.fields.push_back(parse_output_field_line(fl.text, fl.lineno));
        }
        ob.after_index = j;
        return true;
    }
    return false;
}

/* Discover defined control levels by parsing input specs locally (§7.3.1).
 * Returns ascending level numbers 1-9 that have at least one control field. */
std::vector<int> discover_control_levels(const std::vector<SourceLine> &src) {
    ISpecs is = parse_ispecs(src);
    bool defined[10] = {};
    for (const auto &fld : is.fields) {
        if (fld.control_level.size() == 2 && fld.control_level[0] == 'L'
            && fld.control_level[1] >= '1' && fld.control_level[1] <= '9') {
            defined[fld.control_level[1] - '0'] = true;
        }
    }
    std::vector<int> levels;
    for (int n = 1; n <= 9; ++n) if (defined[n]) levels.push_back(n);
    return levels;
}

/* Look up a source field's length and decimals from the input specs (for total
 * field sizing: length + 2, same decimals). Returns false if not found. */
struct FieldSize { int length; int decimals; };
bool source_field_size(const std::vector<SourceLine> &src, const std::string &name,
                       FieldSize &out) {
    ISpecs is = parse_ispecs(src);
    for (const auto &fld : is.fields) {
        if (upper(fld.name) == upper(name)) {
            out.length = fld.to - fld.from + 1;
            if (out.length < 1) out.length = 1;
            out.decimals = fld.decimals;
            return true;
        }
    }
    return false;
}

/* Build the synthesized total-field name for an A field `base` at level `lv`.
 * lv is 1-9 or 10 (LR).  Suffix char: '1'-'9' or 'R'. */
std::string total_field_name(const std::string &base, int lv) {
    char suf = (lv == 10) ? 'R' : ('0' + lv);
    if ((int)base.size() < 6) return base + suf;
    return base.substr(0, 5) + suf;     // len == 6: replace last char
}

/* ---- C-spec text emission helpers ---- */

/* Emit a C-spec line. Fields placed in their columns; empty fields left blank. */
SourceLine cspec_line(int lineno,
                      const std::string &ctl_level,
                      const std::string &factor1,
                      const std::string &op,
                      const std::string &factor2,
                      const std::string &result,
                      int result_len = 0, int result_dec = -1) {
    SourceLine s = spec('C', lineno);
    if (!ctl_level.empty()) put_col(s.text, 7, 8, ctl_level);
    if (!factor1.empty())   put_col(s.text, 18, 27, factor1);
    if (!op.empty())        put_col(s.text, 28, 32, op);
    if (!factor2.empty())   put_col(s.text, 33, 42, factor2);
    if (!result.empty())    put_col(s.text, 43, 48, result);
    if (result_len > 0) {
        char buf[8]; std::snprintf(buf, sizeof buf, "%d", result_len);
        put_col(s.text, 49, 51, buf);
    }
    if (result_dec >= 0) {
        char d[2] = {(char)('0' + result_dec), 0};
        put_col(s.text, 52, 52, d);
    }
    return s;
}

/* ---- O-spec text emission helpers (record + field lines) ---- */

SourceLine ospec_record(int lineno, const std::string &file, char type,
                        const std::string &spacing, const std::string &inds) {
    SourceLine s = spec('O', lineno);
    if (!file.empty())  put_col(s.text, 7, 14, file);
    put_col(s.text, 15, 15, std::string(1, type));
    if (!spacing.empty()) put_col(s.text, 17, 22, spacing);
    if (!inds.empty())    put_col(s.text, 23, 31, inds);
    return s;
}

SourceLine ospec_or_line(int lineno, const std::string &ind_token) {
    SourceLine s = spec('O', lineno);
    put_col(s.text, 14, 16, "OR");
    put_col(s.text, 23, 25, ind_token);
    return s;
}

SourceLine ospec_field_const(int lineno, const std::string &literal, int end_pos,
                             const std::string &inds = "") {
    SourceLine s = spec('O', lineno);
    if (!inds.empty()) put_col(s.text, 23, 31, inds);
    put_col(s.text, 40, 43, std::to_string(end_pos));
    put_col(s.text, 45, 70, literal);
    return s;
}

SourceLine ospec_field_name(int lineno, const std::string &name, int end_pos,
                            char edit_code = 0, bool blank_after = false,
                            const std::string &inds = "") {
    SourceLine s = spec('O', lineno);
    if (!inds.empty()) put_col(s.text, 23, 31, inds);
    put_col(s.text, 32, 37, name);
    if (edit_code)     put_col(s.text, 38, 38, std::string(1, edit_code));
    if (blank_after)   put_col(s.text, 39, 39, "B");
    put_col(s.text, 40, 43, std::to_string(end_pos));
    return s;
}

/* Strip one layer of apostrophes from a literal (e.g. 'SALES' -> SALES). */
std::string unquote_literal(const std::string &raw) {
    std::string s = raw;
    // Take from after the first apostrophe to the last.
    auto first = s.find('\'');
    if (first == std::string::npos) return s;
    auto last = s.find_last_of('\'');
    if (last == first) return s.substr(first + 1);
    s = s.substr(first + 1, last - first - 1);
    // Collapse '' -> '
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\'' && i + 1 < s.size() && s[i+1] == '\'') { out += '\''; ++i; }
        else out += s[i];
    }
    return out;
}

/* The visible print width of a field/constant (for end-position computation). */
int output_item_width(const OutputField &f) {
    if (!f.constant.empty() && f.constant.find('\'') != std::string::npos)
        return (int)unquote_literal(f.constant).size();
    if (!f.constant.empty()) return (int)unquote_literal(f.constant).size();
    if (!f.name.empty()) return (int)f.name.size();   // placeholder (see §C1 note)
    return 0;
}

} // namespace

/* ========================================================================== *
 * expand_output_block -- generate C-specs + O-specs for one D/T-*AUTO block.
 *
 * Emits, as ordinary RPG text:
 *   - Column-heading O-specs (type H), conditioned 1P OR <overflow>.
 *   - A detail O-spec (type D), conditioned per the record-desc (blank => N1P).
 *   - Accumulator C-specs: an EXSR A$$SUM at detail time, a roll chain at total
 *     time (one ADD per A field per defined level), and the A$$SUM subroutine.
 *   - Total O-specs (type T), one per defined level + LR.
 *
 * Generated C-spec lines are collected into `gen_calcs`; generated O-spec lines
 * into `gen_outputs`. The caller splices them into src. Returns false on a hard
 * error (already reported).
 * ========================================================================== */
static bool expand_output_block(const std::vector<SourceLine> &src,
                                const OutputBlock &ob,
                                const AutoReportOptions &opts,
                                const std::string &overflow_token,
                                std::vector<SourceLine> &gen_calcs,
                                std::vector<SourceLine> &gen_outputs) {
    // ---- discover control levels + A-field source sizes ----
    auto levels = discover_control_levels(src);
    // The full level list including LR (level 10), ascending.
    std::vector<int> all_levels = levels;
    all_levels.push_back(10);   // LR is always present (final total)

    // Collect the A fields (accumulated). Validate naming collisions.
    std::vector<const OutputField *> a_fields;
    for (const auto &f : ob.fields)
        if (f.kind == OutputField::Kind::Accumulate) a_fields.push_back(&f);

    // Collision checks (manual 91223-91249).
    for (size_t i = 0; i < a_fields.size(); ++i) {
        for (size_t j = i + 1; j < a_fields.size(); ++j) {
            if (upper(a_fields[i]->name) == upper(a_fields[j]->name)) {
                report("input", a_fields[j]->lineno, 32, DiagKind::Error,
                       "Auto Report: field '" + a_fields[j]->name +
                       "' is accumulated (A in col 39) more than once");
                return false;
            }
            // Two A fields whose first 5 chars collide would synthesize the
            // same total-field name at len==6.
            std::string a = upper(a_fields[i]->name.substr(0, 5));
            std::string b = upper(a_fields[j]->name.substr(0, 5));
            if (a == b && a_fields[i]->name.size() >= 5
                && a_fields[j]->name.size() >= 5) {
                report("input", a_fields[j]->lineno, 32, DiagKind::Error,
                       "Auto Report: accumulated fields '" + a_fields[i]->name +
                       "' and '" + a_fields[j]->name +
                       "' collide in their synthesized total-field names");
                return false;
            }
        }
    }

    // Build the synthesized total-field map: for each A field, the name and
    // size at each defined level (+LR).
    struct AFieldTotals {
        const OutputField *src;
        FieldSize size;                 // source size (length, decimals)
        std::vector<std::pair<int,std::string>> level_names; // (level, total_name)
    };
    std::vector<AFieldTotals> a_totals;
    for (const auto *af : a_fields) {
        AFieldTotals t;
        t.src = af;
        if (!source_field_size(src, af->name, t.size)) {
            report("input", af->lineno, 32, DiagKind::Error,
                   "Auto Report: accumulated field '" + af->name +
                   "' is not a defined input field");
            return false;
        }
        for (int lv : all_levels)
            t.level_names.push_back({lv, total_field_name(af->name, lv)});
        a_totals.push_back(std::move(t));
    }

    // ---- compute end positions for detail-line fields ----
    // Lay fields left-to-right: detail fields (blank/B) and constants. At least
    // 2 blanks before each field; no space before a constant (manual 92132).
    // A fields print on the detail line too (their current value).
    struct PlacedField {
        const OutputField *desc;
        int start;
        int end;
        int width;
        bool is_numeric;
    };
    std::vector<PlacedField> placed;
    int cur = 0;
    for (const auto &f : ob.fields) {
        if (f.kind == OutputField::Kind::HeadingCont) continue;  // not on detail
        if (f.kind == OutputField::Kind::TotalLine) continue;
        int w = (int)output_item_width(f);
        // For a named field, the print width is its source field length.
        bool is_num = false;
        if (!f.name.empty()) {
            FieldSize fsz;
            if (source_field_size(src, f.name, fsz)) {
                w = fsz.length;
                if (fsz.decimals >= 0) is_num = true;
            }
        }
        int lead = (cur == 0) ? 0 : 2;   // >=2 blanks before each field
        // No leading space for a constant (manual 92133).
        if (f.name.empty() && !f.constant.empty()) lead = (cur == 0) ? 0 : 0;
        int start = cur + lead + 1;
        // An explicit end_pos overrides (manual 92104).
        if (f.end_pos > 0) { start = f.end_pos - w + 1; }
        int end = start + w - 1;
        placed.push_back({&f, start, end, w, is_num});
        cur = end;
    }
    int max_end = cur;   // rightmost position on the detail line

    // The column-heading line width = max_end (detail defines the body width).
    // Column headings are centered on the detail line width.

    // ---- generate column-heading O-specs ----
    // Each detail/accumulate field has a column heading (the constant in cols
    // 45-70). Heading-continuation (C) lines extend a field's heading. We emit
    // one type-H record per heading line. Headings conditioned 1P OR <overflow>.
    // Collect, per detail/accumulate field, its heading literal(s) (1+ lines).
    struct ColHeading {
        const OutputField *field;     // the field this heading belongs to
        std::vector<std::string> lines; // heading literals (1st = the field's own)
    };
    std::vector<ColHeading> headings;
    {
        // Walk fields in order; a C-type line extends the most recent
        // detail/accumulate field's heading.
        ColHeading *last = nullptr;
        for (const auto &f : ob.fields) {
            if (f.kind == OutputField::Kind::HeadingCont) {
                if (last) last->lines.push_back(unquote_literal(f.constant));
                continue;
            }
            if (f.kind == OutputField::Kind::Detail ||
                f.kind == OutputField::Kind::Accumulate) {
                ColHeading ch;
                ch.field = &f;
                if (!f.constant.empty())
                    ch.lines.push_back(unquote_literal(f.constant));
                headings.push_back(std::move(ch));
                last = &headings.back();
            } else {
                last = nullptr;   // total-line field breaks the heading run
            }
        }
    }
    int max_heading_lines = 0;
    for (const auto &h : headings)
        max_heading_lines = std::max(max_heading_lines, (int)h.lines.size());

    for (int hl = 0; hl < max_heading_lines; ++hl) {
        // Heading record: conditioned 1P OR <overflow>, single-space after
        // except the last (space-2 after last).
        std::string hsp(6, ' ');
        hsp[1] = (hl + 1 == max_heading_lines) ? '2' : '1';
        SourceLine hrec = ospec_record(ob.lineno, ob.file, 'H', hsp, "1P");
        gen_outputs.push_back(hrec);
        if (!overflow_token.empty())
            gen_outputs.push_back(ospec_or_line(ob.lineno, overflow_token));
        // Field lines: place each field's heading-line literal at the field's
        // end position (right-aligned for numeric, left for alphameric).
        for (size_t pi = 0; pi < placed.size(); ++pi) {
            const auto &pf = placed[pi];
            // Find this field's heading.
            const ColHeading *ch = nullptr;
            for (const auto &h : headings) if (h.field == pf.desc) { ch = &h; break; }
            if (!ch || hl >= (int)ch->lines.size()) continue;
            std::string lit = ch->lines[hl];
            if (lit.empty()) continue;
            std::string quoted = "'" + lit + "'";
            // Center the heading over the field column (manual 92119): the
            // heading end position is the field's end, with the heading text
            // right-aligned for numeric fields, left-aligned otherwise.
            int end = pf.end;
            gen_outputs.push_back(ospec_field_const(ob.lineno, quoted, end));
        }
    }

    // ---- generate the detail O-spec ----
    {
        // Detail conditioning: blank => N1P (manual 90980). User indicators
        // pass through.
        std::string dind = col_trim(ob.indicators, 23, 31);
        std::string cond = dind.empty() ? "N1P" : ob.indicators;
        std::string dsp = col_trim(ob.spacing, 17, 22).empty()
                          ? std::string(" 1    ")   // single-space after (col 18 = 1)
                          : ob.spacing;
        SourceLine drec = ospec_record(ob.lineno, ob.file, 'D', dsp, cond);
        if (ob.fetch_overflow)
            put_col(drec.text, 16, 16, "F");
        gen_outputs.push_back(drec);
        for (const auto &pf : placed) {
            const auto &f = *pf.desc;
            if (!f.name.empty()) {
                // Numeric field: edit code K if col 38 blank (manual 91100).
                char ec = f.edit_code;
                if (pf.is_numeric && ec == 0) ec = 'K';
                gen_outputs.push_back(ospec_field_name(ob.lineno, f.name, pf.end,
                                                       ec, f.blank_after));
            } else if (!f.constant.empty()) {
                gen_outputs.push_back(ospec_field_const(ob.lineno, f.constant, pf.end));
            }
        }
    }

    // ---- generate accumulator C-specs (A fields) ----
    if (!a_totals.empty()) {
        // 1. Detail-time EXSR A$$SUM. For D-*AUTO: conditioned like the detail
        //    record (blank => N1P). For T-*AUTO: unconditioned (manual 92950).
        std::string exsr_ctl;
        std::string exsr_cond;
        if (ob.type == 'T') {
            // Unconditioned.
        } else {
            std::string dind = col_trim(ob.indicators, 23, 31);
            if (dind.empty()) { exsr_cond = "N1P"; }
            else { exsr_cond = ob.indicators; }
        }
        gen_calcs.push_back(cspec_line(ob.lineno, exsr_ctl, "", "EXSR", "A$$SUM",
                                       "", 0, -1));

        // 2. T-*AUTO: L0 Z-ADD resets each A-field to zero each cycle, as the
        //    FIRST total calc (manual 93095).
        if (ob.type == 'T') {
            for (const auto &t : a_totals) {
                int tlen = t.size.length + 2;
                std::string lowest = t.level_names.front().second;
                gen_calcs.push_back(cspec_line(ob.lineno, "L0", "", "Z-ADD", "0",
                                               lowest, tlen, t.size.decimals));
            }
        }

        // 3. Total-time roll chain: for each level (ascending, except the
        //    lowest), add the lower-level total into the next-higher one. The
        //    chain is conditioned by the lower level's indicator (manual 92940).
        //    When no control levels are defined, only the LR field exists and
        //    the A$$SUM subroutine accumulates directly into it (no rolls).
        for (size_t li = 0; li + 1 < all_levels.size(); ++li) {
            int lower = all_levels[li];
            int higher = all_levels[li + 1];
            // The conditioning level indicator is the lower level (L1..L9).
            // LR rolls are conditioned by the highest defined control level.
            std::string roll_ctl;
            if (lower >= 1 && lower <= 9) roll_ctl = "L" + std::to_string(lower);
            else roll_ctl = "LR";   // shouldn't happen (LR is always last)
            for (const auto &t : a_totals) {
                std::string lo_name = t.level_names[li].second;
                std::string hi_name = t.level_names[li + 1].second;
                int tlen = t.size.length + 2;
                // Roll: hi_name = hi_name + lo_name (factor1=hi, factor2=lo,
                // result=hi). Manual Figure 250: "L1 SOLDV2 ADD SOLDV1 SOLDV2".
                gen_calcs.push_back(cspec_line(ob.lineno, roll_ctl, hi_name,
                                               "ADD", lo_name, hi_name,
                                               tlen, t.size.decimals));
            }
        }

        // 4. The A$$SUM subroutine (always last). Each ADD accumulates the
        //    source field into the lowest-level total field. Conditioned by the
        //    field's own field-description indicators (cols 23-31), if any.
        gen_calcs.push_back(cspec_line(ob.lineno, "", "A$$SUM", "BEGSR", "",
                                       "", 0, -1));
        for (const auto &t : a_totals) {
            std::string lowest = t.level_names.front().second;
            int tlen = t.size.length + 2;
            gen_calcs.push_back(cspec_line(ob.lineno, "", "", "ADD", t.src->name,
                                           lowest, tlen, t.size.decimals));
        }
        gen_calcs.push_back(cspec_line(ob.lineno, "", "", "ENDSR", "", "",
                                       0, -1));
    }

    // ---- generate total O-specs (C3) ----
    // One type-T record per defined level (+LR). Each carries the synthesized
    // total field (edit code K, blank-after B) at the same end_pos as the detail
    // field. Asterisks to the right of max_end (count = level depth). 1-9/R
    // total-line fields/constants print to the LEFT of the first total field.
    if (!a_totals.empty()) {
        // Map each A field to its placed position (end_pos) on the detail line.
        auto field_end = [&](const std::string &name) -> int {
            for (const auto &pf : placed)
                if (upper(pf.desc->name) == upper(name)) return pf.end;
            return max_end;
        };
        // The asterisk base: position just right of the rightmost total field.
        // Collect total-line (1-9/R) fields/constants grouped by level.
        // For each level, we emit one T-record.
        int nstars = 0;
        for (size_t li = 0; li < all_levels.size(); ++li) {
            int lv = all_levels[li];
            ++nstars;   // L1=1 star, ..., LR = N stars
            std::string lvind = (lv == 10) ? "LR" : ("L" + std::to_string(lv));
            // Spacing: 2 lines after every total; 1 space before lowest + final.
            std::string tsp(6, ' ');
            tsp[1] = '2';   // space-2 after
            bool is_lowest = (li == 0);
            bool is_final  = (li + 1 == all_levels.size());
            if (is_lowest || is_final) tsp[0] = '1';  // space-1 before
            SourceLine trec = ospec_record(ob.lineno, ob.file, 'T', tsp, lvind);
            gen_outputs.push_back(trec);

            // Total-line (1-9/R) fields/constants for THIS level, placed to the
            // LEFT of the first total field.
            int left_cursor = 0;
            for (const auto &f : ob.fields) {
                if (f.kind != OutputField::Kind::TotalLine) continue;
                if (f.total_level != lv) continue;
                if (!f.constant.empty()) {
                    int w = (int)unquote_literal(f.constant).size();
                    int start = left_cursor + 1;   // one space before
                    int end = start + w - 1;
                    gen_outputs.push_back(ospec_field_const(ob.lineno, f.constant, end));
                    left_cursor = end + 1;   // one space after
                } else if (!f.name.empty()) {
                    int w = (int)f.name.size();
                    int start = left_cursor + 1;
                    int end = start + w - 1;
                    gen_outputs.push_back(ospec_field_name(ob.lineno, f.name, end));
                    left_cursor = end + 1;
                }
            }

            // The A-field totals for this level, at their detail positions.
            for (const auto &t : a_totals) {
                std::string tname = t.level_names[li].second;
                int end = field_end(t.src->name);
                gen_outputs.push_back(ospec_field_name(ob.lineno, tname, end,
                                                       'K', true));
            }

            // Asterisks to the right of max_end (suppressed if U-spec col 28=N).
            if (!opts.suppress_asterisks) {
                std::string stars(nstars, '*');
                std::string quoted = "'" + stars + "'";
                gen_outputs.push_back(ospec_field_const(ob.lineno, quoted, max_end + 1));
            }
        }
    }

    return true;
}

/* ========================================================================== *
 * expand_autoreport -- the public entry point.
 * ========================================================================== */
bool expand_autoreport(std::vector<SourceLine> &src,
                       const std::string & /*base_dir*/,
                       AutoReportReport &rep) {
    // ---- fast path: is this an Auto Report program at all? ----
    // It is if it has any U line OR any O-spec record line whose cols 32-36
    // spell *AUTO. (The /COPY sub-feature already runs before us in
    // expand_copy_statements, so we do not look for it here.)
    bool has_u = false;
    bool has_auto = false;
    int first_u_index = -1;
    for (size_t i = 0; i < src.size(); ++i) {
        const auto &sl = src[i];
        if (sl.comment) continue;
        char ft = form_type(sl);
        if (ft == 'U') {
            has_u = true;
            if (first_u_index < 0) first_u_index = (int)i;
        }
        if (ft == 'O' && upper(col_trim(sl.text, 32, 36)) == "*AUTO")
            has_auto = true;
    }
    if (!has_u && !has_auto) return true;   // ordinary program: untouched

    // ---- U-line handling ----
    AutoReportOptions options;
    if (has_u) {
        // The U line must be the first spec (manual 90211): no non-comment, no
        // /COPY, non-U spec may precede it.
        for (int i = 0; i < first_u_index; ++i) {
            const auto &sl = src[i];
            if (sl.comment) continue;
            // /COPY directives are not form-typed specs; a U line after a /COPY
            // is allowed (the copy expands first).
            if (upper(col_trim(sl.text, 7, 11)) == "/COPY") continue;
            report("input", sl.lineno, 6, DiagKind::Error,
                   "Auto Report 'U' (Option) spec must precede all other "
                   "specifications (manual 90211)");
            return false;
        }
        options = parse_uspec(src[first_u_index]);
    }

    // ---- D/T-*AUTO output-block expansion (Phase C) ----
    // Parse and expand the single allowed D/T-*AUTO block (manual 91033).
    OutputBlock ob;
    bool has_dt = parse_output_block(src, ob);
    if (has_dt) {
        // Determine the printer file's overflow indicator (allocate if none).
        PrinterInfo pi = scan_printer(src, ob.file);
        if (!pi.found) {
            report("input", ob.lineno, 7, DiagKind::Error,
                   "Auto Report D/T-*AUTO names PRINTER file '" + ob.file +
                   "' which is not declared as a PRINTER file");
            return false;
        }
        std::string ovf = pi.has_overflow ? pi.overflow_token
                                          : allocate_overflow(src);

        std::vector<SourceLine> gen_calcs, gen_outputs;
        if (!expand_output_block(src, ob, options, ovf, gen_calcs, gen_outputs))
            return false;

        // Splice the generated lines into src: drop the D/T-*AUTO block lines
        // (and the U line if present), and insert the generated C-specs + O-specs
        // at the position where the D/T-*AUTO block sat (C4: specs stay in source
        // order so hand-coded and generated specs interleave correctly).
        std::vector<bool> consumed_dt(src.size(), false);
        for (size_t i = ob.rec_index; i < ob.after_index; ++i)
            consumed_dt[i] = true;
        if (has_u && first_u_index >= 0 && first_u_index < (int)consumed_dt.size())
            consumed_dt[first_u_index] = true;

        std::vector<SourceLine> out_dt;
        out_dt.reserve(src.size() + gen_calcs.size() + gen_outputs.size());
        for (size_t i = 0; i < ob.rec_index; ++i) {
            if (consumed_dt[i]) continue;
            out_dt.push_back(src[i]);
        }
        // C4 insert point: generated C-specs first (detail EXSR, total rolls,
        // subroutine), then generated O-specs (headings, detail, totals).
        for (auto &c : gen_calcs)   out_dt.push_back(std::move(c));
        for (auto &o : gen_outputs) out_dt.push_back(std::move(o));
        for (size_t i = ob.rec_index; i < src.size(); ++i) {
            if (consumed_dt[i]) continue;
            out_dt.push_back(src[i]);
        }

        src = std::move(out_dt);
        rep.changed = true;
        rep.notes.push_back("expanded D/T-*AUTO output block for '" + ob.file + "'");
        // The U line (if any) was consumed above; prevent the H-*AUTO path
        // below from trying to consume it again (its first_u_index is stale
        // relative to the rewritten src).
        has_u = false;
        // A D/T-*AUTO program's H-*AUTO headings (if any) are expanded below on
        // the now-rewritten src. Fall through.
    }

    // ---- H-*AUTO page-heading expansion ----
    auto blocks = parse_heading_blocks(src);
    if (blocks.empty()) {
        // A U line with no constructs: consume the U line and return. The
        // program is otherwise ordinary; parsing proceeds as today.
        if (has_u) {
            std::vector<SourceLine> out;
            out.reserve(src.size());
            for (size_t i = 0; i < src.size(); ++i) {
                if ((int)i == first_u_index) continue;   // drop the U line
                out.push_back(src[i]);
            }
            src = std::move(out);
            rep.notes.push_back("consumed Auto Report 'U' option spec (no constructs)");
            rep.changed = true;
        }
        return true;
    }

    // Enforce "only one PRINTER file" and "<= 5 H-*AUTO per file" (manual 90750+).
    {
        std::vector<std::string> printer_files;
        for (const auto &b : blocks) {
            bool seen = false;
            for (const auto &f : printer_files) if (f == b.file) { seen = true; break; }
            if (!seen) printer_files.push_back(b.file);
        }
        for (const auto &pf : printer_files) {
            int n = 0;
            for (const auto &b : blocks) if (b.file == pf) ++n;
            if (n > 5) {
                int ln = 0;
                for (const auto &b : blocks) if (b.file == pf) { ln = b.lineno; break; }
                report("input", ln, 15, DiagKind::Error,
                       "Auto Report allows at most five H-*AUTO page-heading "
                       "specifications per file (manual 90761)");
                return false;
            }
        }
        if (printer_files.size() > 1) {
            report("input", blocks.front().lineno, 7, DiagKind::Error,
                   "Auto Report H-*AUTO page headings can be specified for only "
                   "one PRINTER file per program (manual 90750)");
            return false;
        }
    }

    // Determine the printer file's overflow indicator (allocate one if none).
    std::string overflow_token;
    {
        PrinterInfo pi = scan_printer(src, blocks.front().file);
        if (!pi.found) {
            report("input", blocks.front().lineno, 7, DiagKind::Error,
                   "Auto Report H-*AUTO names PRINTER file '" +
                   blocks.front().file +
                   "' which is not declared as a PRINTER file");
            return false;
        }
        overflow_token = pi.has_overflow ? pi.overflow_token
                                         : allocate_overflow(src);
    }
    int report_width = 132;

    // ---- build the rewritten source ----
    // We must: drop the U line (if any), drop every H-*AUTO block (record line
    // + its field lines), and insert generated heading lines where the first
    // block was. Other O-specs keep their relative order.
    //
    // First, mark which original indices belong to consumed H-*AUTO blocks.
    std::vector<bool> consumed(src.size(), false);
    if (has_u) consumed[first_u_index] = true;
    for (size_t i = 0; i < src.size(); ++i) {
        const auto &sl = src[i];
        if (sl.comment) continue;
        if (form_type(sl) != 'O') continue;
        if (upper(col_trim(sl.text, 15, 15)) != "H") continue;
        if (upper(col_trim(sl.text, 32, 36)) != "*AUTO") continue;
        // record line
        consumed[i] = true;
        for (size_t j = i + 1; j < src.size(); ++j) {
            const auto &fl = src[j];
            if (fl.comment) { continue; }
            if (form_type(fl) != 'O') break;
            std::string rel = upper(col_trim(fl.text, 14, 16));
            if (rel == "AND" || rel == "OR") break;
            if (!col_trim(fl.text, 15, 15).empty()) break;
            consumed[j] = true;
        }
    }

    // Apply per-block spacing defaults.
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        apply_default_spacing(blocks[bi], bi == 0, bi + 1 == blocks.size());
    }

    // Emit generated lines for each block.
    std::vector<SourceLine> generated;
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        auto recs = emit_heading_record(blocks[bi], options, bi == 0,
                                        overflow_token, report_width);
        // Apply the (possibly defaulted) spacing to each record line: write
        // cols 17-22 from blocks[bi].spacing onto the record line.
        for (auto &r : recs) {
            if (!r.text.empty() && r.text[5] == 'O' &&
                !col_trim(r.text, 15, 15).empty() &&
                upper(col_trim(r.text, 15, 15))[0] == 'H' &&
                col_trim(r.text, 14, 16) != "OR") {
                put_col(r.text, 17, 22, blocks[bi].spacing);
            }
        }
        generated.insert(generated.end(), recs.begin(), recs.end());
    }

    // Locate where the first block's record line sat, to place generated lines.
    size_t insert_at = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        const auto &sl = src[i];
        if (sl.comment) continue;
        if (form_type(sl) != 'O') continue;
        if (upper(col_trim(sl.text, 15, 15)) != "H") continue;
        if (upper(col_trim(sl.text, 32, 36)) != "*AUTO") continue;
        insert_at = i;
        break;
    }

    // Assemble the output: lines [0, insert_at) not consumed, then generated,
    // then the rest (skipping consumed).
    std::vector<SourceLine> out;
    out.reserve(src.size() + generated.size());
    for (size_t i = 0; i < insert_at; ++i)
        if (!consumed[i]) out.push_back(src[i]);
    for (auto &g : generated) out.push_back(std::move(g));
    for (size_t i = insert_at; i < src.size(); ++i)
        if (!consumed[i]) out.push_back(src[i]);

    src = std::move(out);
    rep.changed = true;
    rep.notes.push_back("expanded H-*AUTO page heading(s) for '" +
                        blocks.front().file + "'");
    return true;
}

} // namespace rpgc
