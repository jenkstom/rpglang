// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * source.h -- raw RPG source representation.
 *
 * RPG II is column-oriented, so we keep each source line and slice it by
 * fixed column offsets rather than tokenizing on whitespace. This header
 * provides the SourceLine view plus a couple of slicing helpers.
 * ========================================================================== */
#ifndef RPGC_SOURCE_H
#define RPGC_SOURCE_H

#include <string>
#include <vector>

namespace rpgc {

struct SourceLine {
    std::string text;   // physical line, no trailing newline
    int lineno   = 0;   // 1-based line number in the file
    bool comment = false;// true if column 7 is '*' (after the spec letter) -- a comment line
};

/* Load a source file into a vector of SourceLine. Strips CR/LF. Returns false
 * if the file cannot be opened. */
bool load_source(const std::string &path, std::vector<SourceLine> &out);

/* Extract a fixed column range [first, last] (1-based, inclusive) from a line.
 * Out-of-range columns yield spaces (RPG treats missing columns as blank).
 * Never throws. */
std::string col(const std::string &line, int first, int last);

/* Same as col() but with surrounding whitespace removed. */
std::string col_trim(const std::string &line, int first, int last);

/* The form-type letter (column 6), upper-cased; ' ' if the line is blank or
 * too short. */
char form_type(const SourceLine &l);

/* D3: expand /COPY directives (manual Ch. 26, "Auto Report /COPY Statement
 * Specifications", 90360-90450) in place. A /COPY line (cols 7-11 == "/COPY",
 * cols 13-29 == "LIBRARY,MEMBER" or just "MEMBER") is replaced by the spliced
 * contents of a source file named MEMBER (optionally MEMBER.rpg/MEMBER.cpy),
 * looked up in `base_dir` -- the library segment is parsed but not used for
 * path resolution: this compiler has no System/36 library-catalog filesystem
 * to resolve it against, only a plain host directory. Nested /COPY lines in
 * the copied member are expanded recursively; a member that (directly or
 * transitively) copies itself is a hard error rather than infinite recursion.
 * Returns false (with a diagnostic already reported) on a missing member or a
 * copy cycle. */
bool expand_copy_statements(std::vector<SourceLine> &src,
                            const std::string &base_dir);

/* W2: resolve an F-spec FMTS continuation option's display-file name to a
 * path under `base_dir`. Tries the name itself, then `name + ".dspf"`
 * (this project's own extension convention -- there is no legacy DDS/SDA
 * artifact to be compatible with; see WORKSTN support notes in
 * docs/ARCHITECTURE.md). Same "look up a sibling source file by name"
 * shape /COPY uses (D3), factored out here so both share one convention.
 * Returns "" if neither exists. */
std::string resolve_display_file(const std::string &base_dir, const std::string &name);

} // namespace rpgc

#endif // RPGC_SOURCE_H
