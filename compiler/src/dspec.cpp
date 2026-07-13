// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * dspec.cpp -- parse Display-format field (D-spec) lines.
 * ========================================================================== */
#include "dspec.h"
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

bool parse_dspec_line(const SourceLine &sl, DisplayField &out) {
    out = DisplayField();
    out.lineno = sl.lineno;
    out.name   = col_trim(sl.text, 53, 58);

    std::string u = upper(col_trim(sl.text, 16, 16));
    if      (u == "I") out.usage = DisplayUsage::Input;
    else if (u == "B") out.usage = DisplayUsage::Both;
    else                out.usage = DisplayUsage::Output;   // 'O' or blank

    std::string r = col_trim(sl.text, 18, 19);
    if (!r.empty()) { try { out.row = std::stoi(r); } catch (...) {} }
    std::string c = col_trim(sl.text, 21, 22);
    if (!c.empty()) { try { out.col = std::stoi(c); } catch (...) {} }

    out.protect = (upper(col_trim(sl.text, 31, 31)) == "P");
    std::string color = upper(col_trim(sl.text, 33, 33));
    if (!color.empty()) out.color = color[0];
    out.reverse = (upper(col_trim(sl.text, 35, 35)) == "R");
    out.blink   = (upper(col_trim(sl.text, 37, 37)) == "B");

    std::string fromtxt = col_trim(sl.text, 44, 47);
    std::string totxt   = col_trim(sl.text, 48, 51);
    if (!fromtxt.empty()) { try { out.from = std::stoi(fromtxt); } catch (...) {} }
    if (!totxt.empty())   { try { out.to   = std::stoi(totxt);   } catch (...) {} }
    if (out.to == 0) out.to = out.from;

    std::string dec = col_trim(sl.text, 52, 52);
    if (!dec.empty() && std::isdigit((unsigned char)dec[0]))
        out.decimals = dec[0] - '0';

    if (out.name.empty()) {
        // Literal/label: quoted text starting at col 60.
        std::string region = col(sl.text, 60, (int)sl.text.size());
        auto notspace = [](unsigned char ch){ return !std::isspace(ch); };
        auto a = std::find_if(region.begin(), region.end(), notspace);
        if (a == region.end() || *a != '\'') {
            report("dspf", sl.lineno, 60, DiagKind::Error,
                   "D-spec line has neither a field name (cols 53-58) nor a "
                   "quoted literal (col 60+ starting with an apostrophe)");
            return false;
        }
        std::string raw(a + 1, region.end());
        auto last = raw.find_last_of('\'');
        if (last != std::string::npos) raw = raw.substr(0, last);
        std::string outc;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\'' && i + 1 < raw.size() && raw[i+1] == '\'') {
                outc += '\''; ++i;
            } else outc += raw[i];
        }
        out.text = outc;
        if (out.from == 0) { out.from = 1; out.to = (int)outc.size(); }
    } else {
        if (out.from <= 0 || out.to < out.from) {
            report("dspf", sl.lineno, 44, DiagKind::Error,
                   "D-spec field '" + out.name + "': cols 44-51 must give a "
                   "valid From <= To buffer position");
            return false;
        }
    }
    return true;
}

} // namespace rpgc
