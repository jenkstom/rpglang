// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * dspec.h -- parse Display-format field (D-spec) lines within a .dspf file.
 *
 * A D-spec line describes one field or literal on a display format: its
 * position in the WORKSTN record buffer (From/To, reusing the exact column
 * numbers I-spec field lines use for the same purpose -- ispec.h cols
 * 44-51/52/53-58), plus screen presentation (row/column, usage, and a small
 * attribute set: protect/color/reverse/blink). See sspec.h for the enclosing
 * format (S-spec) and docs/SPEC_MAP.md for the full column map.
 * ========================================================================== */
#ifndef RPGC_DSPEC_H
#define RPGC_DSPEC_H

#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

enum class DisplayUsage { Input, Output, Both };

struct DisplayField {
    int          lineno   = 0;
    std::string  name;                 // cols 53-58; blank => literal (see `text`)
    DisplayUsage usage     = DisplayUsage::Output;   // col 16: I/O/B
    int          row       = 1;        // cols 18-19, 1-based screen row
    int          col       = 1;        // cols 21-22, 1-based screen column
    int          from      = 0;        // cols 44-47: buffer start position
    int          to        = 0;        // cols 48-51: buffer end position
    int          decimals  = -1;       // col 52 (-1 = alphameric)
    bool         protect   = false;    // col 31 == 'P': display-only, no cursor stop
    char         color     = 0;        // col 33: B/R/G/W/T/Y/P; 0 = terminal default
    bool         reverse   = false;    // col 35 == 'R': reverse image
    bool         blink     = false;    // col 37 == 'B': blinking
    std::string  text;                 // literal text (cols 60+, quoted) if name is blank
};

/* Parse one D-spec line into a DisplayField. Caller has already verified
 * form_type(sl) == 'D'. Returns false (with a diagnostic reported) if the
 * line is malformed (e.g. neither a field name nor literal text present, or
 * From/To describe a zero/negative-length buffer range for a named field). */
bool parse_dspec_line(const SourceLine &sl, DisplayField &out);

} // namespace rpgc

#endif // RPGC_DSPEC_H
