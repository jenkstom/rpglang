// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * sspec.cpp -- parse a .dspf display-format file (S-spec headers + D-spec
 * field lines).
 * ========================================================================== */
#include "sspec.h"
#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace rpgc {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

/* Split a comma-separated token list, trimming whitespace, dropping empties. */
std::vector<std::string> split_tokens(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        std::string t = upper(tok);
        auto notspace = [](unsigned char c){ return !std::isspace(c); };
        auto a = std::find_if(t.begin(), t.end(), notspace);
        auto b = std::find_if(t.rbegin(), t.rend(), notspace).base();
        if (a < b) out.emplace_back(a, b);
    }
    return out;
}
} // namespace

std::vector<DisplayFormat> parse_display_formats(const std::vector<SourceLine> &src) {
    std::vector<DisplayFormat> out;
    DisplayFormat *current = nullptr;

    for (const auto &sl : src) {
        if (sl.comment) continue;
        char ft = form_type(sl);
        if (ft != 'S' && ft != 'D') continue;

        if (ft == 'S') {
            DisplayFormat fmt;
            fmt.lineno = sl.lineno;
            fmt.name   = col_trim(sl.text, 7, 14);
            if (fmt.name.empty()) {
                report("dspf", sl.lineno, 7, DiagKind::Error,
                       "S-spec line with no format name in cols 7-14");
                current = nullptr;
                continue;
            }
            if (find_display_format(out, fmt.name)) {
                report("dspf", sl.lineno, 7, DiagKind::Error,
                       "display format '" + fmt.name + "' is defined more "
                       "than once in this file");
                current = nullptr;
                continue;
            }
            fmt.function_keys = split_tokens(col(sl.text, 16, 39));
            fmt.command_keys  = split_tokens(col(sl.text, 41, 70));
            out.push_back(std::move(fmt));
            current = &out.back();
            continue;
        }

        // D-spec field line.
        if (!current) {
            report("dspf", sl.lineno, 6, DiagKind::Error,
                   "D-spec line with no preceding S-spec format header");
            continue;
        }
        DisplayField fld;
        if (!parse_dspec_line(sl, fld)) continue;
        if (!fld.name.empty()) {
            for (const auto &existing : current->fields) {
                if (!existing.name.empty() && upper(existing.name) == upper(fld.name)) {
                    report("dspf", sl.lineno, 53, DiagKind::Error,
                           "field '" + fld.name + "' is defined more than "
                           "once in format '" + current->name + "'");
                }
            }
        }
        current->reclen = std::max(current->reclen, fld.to);
        current->fields.push_back(std::move(fld));
    }
    return out;
}

const DisplayFormat *find_display_format(const std::vector<DisplayFormat> &fmts,
                                         const std::string &name) {
    std::string un = upper(name);
    for (const auto &f : fmts)
        if (upper(f.name) == un) return &f;
    return nullptr;
}

} // namespace rpgc
