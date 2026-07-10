/* ========================================================================== *
 * ispec.h -- parse Input Specifications.
 *
 * An I-spec section for a file begins with a record-identification line and
 * is followed by zero or more field-description lines. We flatten these into:
 *   - one ISpecRec  per record-identification line, and
 *   - one ISpecField per field-description line, each tagged with its file.
 *
 * Phase 3 uses only: the record-identifying indicator, and for each field the
 * From/To record positions, the decimal-position count, and the field name.
 * ========================================================================== */
#ifndef RPGC_ISPEC_H
#define RPGC_ISPEC_H

#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

/* Record-identification line. */
struct ISpecRec {
    int         lineno = 0;
    std::string name;                 // cols 7-14 (the owning file)
    int         rec_indicator = 0;    // cols 19-20 (01-99); 0 if blank/special
};

/* Field-description line. */
struct ISpecField {
    int         lineno = 0;
    std::string file;                 // the file this field belongs to
    int         from = 0;             // cols 44-47 (1-based record position)
    int         to   = 0;             // cols 48-51
    int         decimals = -1;        // col 52 (-1 = alphameric)
    std::string name;                 // cols 53-58
    std::string control_level;        // cols 59-60 (L1-L9)
};

struct ISpecs {
    std::vector<ISpecRec>   records;
    std::vector<ISpecField> fields;
};

/* Parse all I-specs. Maintains the "current file" as record-identification
 * lines are seen, so field lines inherit the right owner. */
ISpecs parse_ispecs(const std::vector<SourceLine> &src);

} // namespace rpgc

#endif // RPGC_ISPEC_H
