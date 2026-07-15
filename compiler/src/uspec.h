// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * uspec.h -- parse the Auto Report 'U' (Options) spec line (D3, Ch. 26).
 *
 * A 'U' line is the entry point of an Auto Report source program (manual
 * 89988-90360): it and the D/O-specs it implies are expanded by the
 * auto-report preprocessor (see autoreport.{h,cpp}) into ordinary F/I/C/O-specs
 * before the RPG compiler proper ever runs. The 'U' line itself only carries
 * option flags -- none of the expansion logic.
 *
 * This module parses those flags into AutoReportOptions. The expansion lives in
 * autoreport.cpp; this parser is the piece that knows the U-spec's column
 * layout.
 * ========================================================================== */
#ifndef RPGC_USPEC_H
#define RPGC_USPEC_H

#include "autoreport.h"
#include "source.h"

namespace rpgc {

/* Parse a single 'U' form-type line into options. Reports a diagnostic (and
 * leaves the offending field at its default) on a malformed catalog reference
 * (col 7 == 'C' but cols 8-24 are not a well-formed "lib,member"). Sets
 * options.present = true. Call only on a line whose form_type() == 'U'. */
AutoReportOptions parse_uspec(const SourceLine &sl);

} // namespace rpgc

#endif // RPGC_USPEC_H
