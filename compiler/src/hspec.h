// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * hspec.h -- parse the Control (Header) Specification (D1).
 *
 * Exactly one H-spec line is meaningful per program (manual Ch. 18, "should
 * always be the first specification in the program"); if more than one is
 * present, the last one wins. If none is present at all, every field keeps
 * its documented blank-column default -- "If you omit the control
 * specification from the source program, the compiler creates a blank
 * control specification" (manual 68847-68849).
 *
 * Three columns currently change generated code (see codegen.cpp):
 *   - col 15 (debug) gates whether DEBUG C-spec lines actually run.
 *   - col 18 (currency symbol) feeds the floating-currency-symbol detection
 *     in rpg_rt_edit_word (previously hardcoded to '$').
 *   - cols 75-80 (program identification) feed the LLVM module identifier
 *     when non-blank.
 * Every other column is parsed and retained but does not change codegen --
 * documented per-field below, instead of the previous silent drop of the
 * entire form type. Most of these (date format/Y edit code/UDATE, inquiry,
 * 1P forms-alignment prompting, file translation, nonprint-character halts,
 * transparent literals) have no analog anywhere else in this batch compiler
 * to plug into; wiring them in is future work once/if that analog exists.
 * ========================================================================== */
#ifndef RPGC_HSPEC_H
#define RPGC_HSPEC_H

#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

struct HSpec {
    bool        present = false;        // an H-spec line was actually seen
    int         lineno = 0;

    bool        debug_enabled = false;  // col 15 == '1' (manual 69625-69634);
                                         // gates the DEBUG operation (codegen.cpp)
    char        currency_symbol = '$';  // col 18 (default '$', manual 69661-69686)
    char        date_format = 0;        // col 19: 'M'/'D'/'Y'/0=blank
    char        date_edit = 0;          // col 20: the date-field separator char, 0=blank(default per date_format)
    char        inverted_print = 0;     // col 21: 'I'/'J'/'D'/0=blank
    bool        alt_collating = false;  // col 26 == 'S'
    bool        inquiry_allowed = false;// col 37 == 'B'
    bool        forms_position_1p = false; // col 41 == '1'
    bool        file_translation = false;  // col 43 == 'F'
    bool        nonprint_bypass = false;   // col 45 == '1'
    bool        transparent_literal = false; // col 57 == '1'
    std::string program_id;             // cols 75-80, blank => "RPGOBJ" default
};

/* Parse all H-specs, returning the last one seen (or a default-valued,
 * `present=false` HSpec if the program has none). */
HSpec parse_hspec(const std::vector<SourceLine> &src);

} // namespace rpgc

#endif // RPGC_HSPEC_H
