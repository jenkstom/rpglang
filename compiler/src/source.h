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

} // namespace rpgc

#endif // RPGC_SOURCE_H
