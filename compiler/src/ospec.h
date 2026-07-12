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

/* Record operation for DISK/update files (O-spec cols 16-18, Section G, G25).
 * Write is the plain printer/printer-file case; Add/Update/Delete drive the
 * corresponding runtime record operation on a type-U (update) or output DISK
 * file. */
enum class ORecOp { Write, Add, Update, Delete };

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
    // A13: floating fill character following an edit code (cols 45-47) -- a
    // bare '*' (asterisk fill) or a quoted currency symbol like '$'. 0 = none.
    char        fill_char = 0;
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
    ORecOp      rec_op = ORecOp::Write; // cols 16-18 ADD/UPDATE/DEL (G25)
    // F2: col 16 F (fetch overflow) / R (release device), single-character
    // forms distinct from the 3-char ADD/DEL/UPDATE mnemonic above (manual
    // 88310-88356). Release is WORKSTN/ICF-only (unsupported, hard error);
    // fetch overflow polls this file's overflow latch immediately after this
    // record prints instead of waiting for the normal cycle-time check.
    bool        fetch_overflow = false; // col 16 == 'F'
    bool        release_device = false; // col 16 == 'R'
    // F1: cols 23-31 line-conditioning indicators, as OR-of-AND groups. Group
    // 0 is this record line's own 3 slots; each AND continuation line (cols
    // 14-16 == "AND") extends the *current* group's AND-list; each OR
    // continuation ("OR") starts a new group. The record fires iff any group
    // is fully satisfied (manual 88200-88217, 88491-88493). A plain record
    // with no continuation lines has exactly one group, matching the old
    // flat-AND-list behavior.
    std::vector<std::vector<CondInd>> conditions;
    std::vector<OField> fields;        // field lines that follow this record line
    std::string except_name;           // cols 32-37, EXCPT name (type-E only)
};

std::vector<ORecord> parse_ospecs(const std::vector<SourceLine> &src);

} // namespace rpgc

#endif // RPGC_OSPEC_H
