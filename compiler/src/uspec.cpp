/* ========================================================================== *
 * uspec.cpp -- reject Auto Report Option Specifications (D3).
 * ========================================================================== */
#include "uspec.h"
#include "diagnostics.h"

namespace rpgc {

bool reject_uspecs(const std::vector<SourceLine> &src) {
    bool found = false;
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'U') continue;
        found = true;
        report("input", sl.lineno, 6, DiagKind::Error,
               "form type 'U' (Auto Report Option Specification, manual "
               "Ch. 26) is not implemented -- this compiler does not expand "
               "Auto Report source programs into ordinary F/I/C/O-specs; "
               "rewrite this program using explicit specifications instead");
    }
    return found;
}

} // namespace rpgc
