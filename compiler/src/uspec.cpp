// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * uspec.cpp -- parse the Auto Report 'U' (Options) spec line (D3, Ch. 26).
 *
 * Column map (manual 90221-90363; verified against the prose, not the figures,
 * which are mangled in the PDF text extraction):
 *
 *   col  6     form type 'U'
 *   col  7     source:  blank = don't catalog; 'C' = catalog created source
 *   cols 8-24  source member ref "library,member" (only when col 7 == 'C');
 *              lib defaults to #LIBRARY if blank/'F1'
 *   cols 25-26 unused
 *   col  27    date/page suppress: blank = print date+page; 'N' = suppress
 *   col  28    '*' suppress:       blank = print asterisks on totals; 'N' = suppress
 *   col  29    unused
 *   col  30    list options: blank = full listing; 'B' = no listing; 'P' = partial
 *   cols 31-74 unused
 * ========================================================================== */
#include "uspec.h"
#include "diagnostics.h"

#include <cctype>

namespace rpgc {

namespace {

bool valid_name(const std::string &s) {
    // A valid RPG symbol: starts alphabetic, <= 8 chars, alphanumeric (the
    // manual allows '@','#','_' too but source identifiers this compiler sees
    // are plain alphanumeric -- keep it permissive on the tail).
    if (s.empty() || s.size() > 8) return false;
    if (!std::isalpha((unsigned char)s[0])) return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '@' && c != '#' && c != '_')
            return false;
    return true;
}

std::string upper(std::string s) {
    for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

} // namespace

AutoReportOptions parse_uspec(const SourceLine &sl) {
    AutoReportOptions o;
    o.present = true;

    // Col 7 -- catalog created source.
    std::string src = col_trim(sl.text, 7, 7);
    if (upper(src) == "C") {
        o.catalog_source = true;
        // Cols 8-15 -- library; cols 16-24 -- member. The library segment may
        // be blank/'F1' (defaults to #LIBRARY). A comma sits in col 16 as the
        // separator when a member is named (manual figures are mangled; the
        // prose fixes library in 8-15 and member in 16-24).
        std::string lib = col_trim(sl.text, 8, 15);
        std::string mem = col_trim(sl.text, 16, 24);
        // A trailing comma on the library or leading comma on the member is
        // the documented separator; strip a single one either way.
        if (!lib.empty() && lib.back() == ',')  lib.pop_back();
        if (!mem.empty() && mem.front() == ',') mem.erase(mem.begin());
        std::string libu = upper(lib);
        if (libu == "F1") lib = "#LIBRARY";
        if (mem.empty()) {
            report("input", sl.lineno, 16, DiagKind::Error,
                   "U-spec catalog reference must name a member (cols 16-24) "
                   "when col 7 == 'C'");
        } else if (!valid_name(lib))
            report("input", sl.lineno, 8, DiagKind::Error,
                   std::string("U-spec catalog library '") + lib +
                   "' is not a valid name (start alphabetic, <=8 chars)");
        else if (!valid_name(mem))
            report("input", sl.lineno, 16, DiagKind::Error,
                   std::string("U-spec catalog member '") + mem +
                   "' is not a valid name (start alphabetic, <=8 chars)");
        else {
            o.catalog_lib    = lib;
            o.catalog_member = mem;
        }
    }

    // Col 27 -- suppress date/page.
    if (upper(col_trim(sl.text, 27, 27)) == "N") o.suppress_date_page = true;
    // Col 28 -- suppress asterisks on totals.
    if (upper(col_trim(sl.text, 28, 28)) == "N") o.suppress_asterisks = true;

    // Col 30 -- list options.
    std::string listc = upper(col_trim(sl.text, 30, 30));
    if (listc == "B")      o.list_opt = AutoReportOptions::ListOpt::NoListing;
    else if (listc == "P") o.list_opt = AutoReportOptions::ListOpt::Partial;
    // blank (or anything else) => Full (the default)

    return o;
}

} // namespace rpgc
