/* ========================================================================== *
 * diagnostics.h -- small error-reporting helper used across the frontend.
 *
 * Centralising diagnostics here keeps file/line/column formatting consistent
 * and makes it trivial to later add colour output or a JSON mode.
 * ========================================================================== */
#ifndef RPGC_DIAGNOSTICS_H
#define RPGC_DIAGNOSTICS_H

#include <string>

namespace rpgc {

enum class DiagKind {
    Note,
    Warning,
    Error,
    Fatal,
};

/* Report a diagnostic about a position in the source file.
 *   file    - path of the source (may be "<stdin>")
 *   line    - 1-based line number (0 if unknown)
 *   col     - 1-based column number (0 if unknown)
 *   kind    - severity
 *   message - human-readable text */
void report(const std::string &file,
            int line,
            int col,
            DiagKind kind,
            const std::string &message);

/* Convenience shorthands for the common case (severity only). */
void error(const std::string &msg);
void fatal(const std::string &msg);

/* Number of errors reported so far in this compilation. */
int  error_count();

} // namespace rpgc

#endif // RPGC_DIAGNOSTICS_H
