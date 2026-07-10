/* ========================================================================== *
 * ospec.h -- parse Output Specifications (O-specs).
 *
 * O-specs describe what a program writes: headings (H), detail lines (D),
 * total lines (T). A section begins with a record line (filename, type,
 * spacing, line conditioning indicators) followed by field lines that name
 * fields or constants and their end positions on the output line.
 *
 * Phase 7 implements the common subset: PRINTER output, field names and
 * quoted constants, D (detail) and T (LR-total) timing, space-after. See
 * docs/SPEC_MAP.md for the full column map.
 * ========================================================================== */
#ifndef RPGC_OSPEC_H
#define RPGC_OSPEC_H

#include "source.h"
#include "cspec.h"      // for CondInd
#include <string>
#include <vector>

namespace rpgc {

enum class OType { Heading, Detail, Total, Exception };

/* One field/constant on an output line. */
struct OField {
    int         lineno    = 0;
    bool        is_const  = false;     // true => constant in `text`
    std::string name;                  // field name (cols 32-37) if !is_const
    std::string text;                  // constant text (cols 45-70) if is_const
    std::string edit_word;             // cols 45-70 quoted edit word (D16); empty=none
    int         end_pos   = 0;         // cols 40-43: rightmost position (0 => pack after prev)
    std::vector<CondInd> conditions;   // cols 23-31, per-field conditioning
    bool        blank_after = false;   // col 39 == 'B'
    char        edit_code = 0;         // col 38 (0 = none)
};

/* One output record line + the fields that belong on it. */
struct ORecord {
    int         lineno = 0;
    std::string file;                  // cols 7-14 (the owning output file)
    OType       type = OType::Detail;  // col 15
    int         space_after = 1;       // col 18 (default single-space)
    int         space_before = 0;      // col 17
    int         skip_before = 0;       // cols 19-20 skip-to line (D13); 0=none
    int         skip_after  = 0;       // cols 21-22 skip-to line (D13); 0=none
    std::vector<CondInd> conditions;   // cols 23-31, line conditioning
    std::vector<OField> fields;        // field lines that follow this record line
    std::string except_name;           // cols 32-37, EXCPT name (type-E only)
};

std::vector<ORecord> parse_ospecs(const std::vector<SourceLine> &src);

} // namespace rpgc

#endif // RPGC_OSPEC_H
