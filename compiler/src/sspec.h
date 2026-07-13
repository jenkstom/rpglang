// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * sspec.h -- parse display-format (S-spec) header lines and the .dspf file
 *            format as a whole.
 *
 * A .dspf file (referenced by an F-spec's FMTS continuation option, fspec.h)
 * holds one or more display formats: an S-spec header line (format name plus
 * which function/command keys the format enables) followed by its D-spec
 * field lines (dspec.h). This is this project's from-scratch replacement for
 * the manual's SDA/S-D-spec tooling -- see WORKSTN support notes in
 * docs/ARCHITECTURE.md for why no such compiler previously existed.
 * ========================================================================== */
#ifndef RPGC_SSPEC_H
#define RPGC_SSPEC_H

#include "dspec.h"
#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

struct DisplayFormat {
    int                        lineno = 0;
    std::string                name;          // cols 7-14
    int                        reclen = 0;     // max field `to`, computed
    std::vector<std::string>   function_keys;  // cols 16-39: KA-KY, comma-separated
    std::vector<std::string>   command_keys;   // cols 41-70: PRINT/ROLLUP/ROLLDOWN/
                                                // CLEAR/HELP/HOME, comma-separated
    std::vector<DisplayField>  fields;
};

/* Parse an already-loaded .dspf source into its display formats. Reports
 * diagnostics (source name "dspf") for malformed S/D lines, a D-line with no
 * preceding S-line, and duplicate format names. Non-S/D lines are ignored. */
std::vector<DisplayFormat> parse_display_formats(const std::vector<SourceLine> &src);

/* Convenience: find a format by name (case-insensitive), or nullptr. */
const DisplayFormat *find_display_format(const std::vector<DisplayFormat> &fmts,
                                         const std::string &name);

} // namespace rpgc

#endif // RPGC_SSPEC_H
