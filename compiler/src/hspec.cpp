// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * hspec.cpp -- parse the Control (Header) Specification (D1).
 * ========================================================================== */
#include "hspec.h"
#include "diagnostics.h"

#include <cctype>

namespace rpgc {

namespace {
/* Manual 69675-69686: these characters have a special meaning in edit words
 * or edit codes and cannot be used as the currency symbol. */
bool is_reserved_currency_char(char c) {
    switch (std::toupper((unsigned char)c)) {
        case '0': case '*': case ',': case '&': case '.': case '-':
        case 'C': case 'R':
            return true;
        default:
            return false;
    }
}
} // namespace

HSpec parse_hspec(const std::vector<SourceLine> &src) {
    HSpec h;
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'H') continue;

        HSpec cur;
        cur.present = true;
        cur.lineno  = sl.lineno;

        std::string cs = col(sl.text, 18, 18);
        if (!cs.empty() && cs[0] != ' ') {
            if (is_reserved_currency_char(cs[0])) {
                report("input", sl.lineno, 18, DiagKind::Warning,
                       std::string("H-spec col 18: '") + cs[0] +
                       "' has a reserved meaning in edit words/edit codes and "
                       "cannot be the currency symbol (manual 69675-69686); "
                       "defaulting to '$'");
            } else {
                cur.currency_symbol = cs[0];
            }
        }

        std::string df = col(sl.text, 19, 19);
        if (!df.empty()) {
            char c = (char)std::toupper((unsigned char)df[0]);
            if (c == 'M' || c == 'D' || c == 'Y') cur.date_format = c;
        }
        std::string de = col(sl.text, 20, 20);
        if (!de.empty() && de[0] != ' ') cur.date_edit = de[0];

        std::string ip = col(sl.text, 21, 21);
        if (!ip.empty()) {
            char c = (char)std::toupper((unsigned char)ip[0]);
            if (c == 'I' || c == 'J' || c == 'D') cur.inverted_print = c;
        }

        cur.alt_collating = (col_trim(sl.text, 26, 26) == "S");

        std::string inq = col_trim(sl.text, 37, 37);
        cur.inquiry_allowed = (!inq.empty() &&
                               std::toupper((unsigned char)inq[0]) == 'B');

        cur.forms_position_1p = (col_trim(sl.text, 41, 41) == "1");
        std::string ft = col_trim(sl.text, 43, 43);
        cur.file_translation = (!ft.empty() &&
                                std::toupper((unsigned char)ft[0]) == 'F');
        cur.nonprint_bypass = (col_trim(sl.text, 45, 45) == "1");
        cur.transparent_literal = (col_trim(sl.text, 57, 57) == "1");

        cur.program_id = col_trim(sl.text, 75, 80);

        h = cur;   // manual: the control spec should be the first line in the
                   // program; if more than one 'H' line appears, the last one
                   // parsed wins (no documented multi-H-spec merge rule).
    }
    return h;
}

} // namespace rpgc
