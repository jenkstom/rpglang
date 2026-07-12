/* ========================================================================== *
 * uspec.h -- Auto Report Option Specifications (form type 'U', D3).
 *
 * A 'U' line is the entry point of an Auto Report source program (manual
 * 89988-90360, Ch. 26): it and the D/O-specs it implies are expanded by the
 * IBM auto-report preprocessor into ordinary F/I/C/O-specs before the RPG
 * compiler proper ever runs. That expansion (deriving headings, spacing, and
 * field placement from just a field list) is a large, separate preprocessing
 * feature this compiler does not implement -- lowest priority among this
 * compiler's known gaps unless a specific legacy program library needs it.
 *
 * Silently ignoring 'U' lines would be worse than refusing them: an Auto
 * Report source program typically has no ordinary O-specs of its own (the
 * option specs stand in for them), so compiling it as-is would silently
 * produce a program with no meaningful output, not just a degraded one. This
 * parser only detects the form type's presence so main.cpp can fail loudly
 * with a clear diagnostic instead (matching the existing E8/B6 precedent).
 * ========================================================================== */
#ifndef RPGC_USPEC_H
#define RPGC_USPEC_H

#include "source.h"
#include <vector>

namespace rpgc {

/* Report a hard diagnostic error for every 'U' (Options) spec line found.
 * Returns true if any were found (and reported). */
bool reject_uspecs(const std::vector<SourceLine> &src);

} // namespace rpgc

#endif // RPGC_USPEC_H
