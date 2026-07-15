// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * autoreport.h -- the Auto Report source-to-source preprocessor (D3/Ch. 26).
 *
 * Auto Report is an IBM System/36-era *preprocessor* that runs before the RPG
 * II compiler proper. It takes an "Auto Report source program" -- ordinary RPG
 * specs plus one of three terse constructs -- and expands it into complete,
 * ordinary RPG (H/F/E/L/I/C/O specs), which is then compiled normally.
 *
 * There are three independent sub-features (manual Ch. 26):
 *   - /COPY     : splice a library member.            [done in source.cpp]
 *   - H-*AUTO   : generate page-heading O-specs.       [Phase B of this module]
 *   - D/T-*AUTO : generate C-specs + O-specs from a    [deferred -- not yet
 *                 field list (detail/total report).     implemented]
 *
 * The entry point is a transform on std::vector<SourceLine>, run right after
 * expand_copy_statements and before any parse_* call (see main.cpp). It rewrites
 * the source lines themselves, emitting ordinary spec text the existing column
 * parsers then consume -- so every parser, codegen, and the analyzer stay
 * unchanged. The 'U' option line (form type 'U') is the entry point and a
 * handful of option flags; it carries none of the expansion logic.
 *
 * Safe to call unconditionally on every program: it is a no-op (returns true,
 * changed=false) for programs containing no U/*AUTO constructs.
 * ========================================================================== */
#ifndef RPGC_AUTOREPORT_H
#define RPGC_AUTOREPORT_H

#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

/* Options parsed from the U-spec line (cols 6-30). Defaults = all-blank U
 * line. Filled by uspec.cpp's parse_uspec(); consumed by the heading/output
 * generators for date/page and asterisk suppression. */
struct AutoReportOptions {
    bool present          = false;  // a U line exists
    bool catalog_source   = false;  // col 7 == 'C'  (write generated src -- non-goal)
    std::string catalog_lib;        // cols 8-15  (library,  before the comma)
    std::string catalog_member;     // cols 16-24 (member,   after  the comma)
    bool suppress_date_page = false; // col 27 == 'N'
    bool suppress_asterisks = false; // col 28 == 'N'
    enum class ListOpt { Full, NoListing, Partial } list_opt = ListOpt::Full; // col 30
};

/* What expand_autoreport found and did. Mirrors CleanReport (clean.h): a list
 * of short human-readable notes (one per construct expanded) plus a flag for
 * whether any specs were generated/rewritten. */
struct AutoReportReport {
    std::vector<std::string> notes;
    bool changed = false;
};

/* The entry point. Rewrites `src` in place: replaces *AUTO constructs with
 * ordinary specs. Returns false (after reporting a diagnostic) on a hard Auto
 * Report error. No-op (returns true, changed=false) if the program contains no
 * U/*AUTO constructs -- so it is safe to call unconditionally on every program.
 *
 * `base_dir` is passed only for symmetry with expand_copy_statements; the copy
 * function already runs separately before us. */
bool expand_autoreport(std::vector<SourceLine> &src,
                       const std::string &base_dir,
                       AutoReportReport &rep);

} // namespace rpgc

#endif // RPGC_AUTOREPORT_H
