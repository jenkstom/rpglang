/* ========================================================================== *
 * diagnostics.h -- small error-reporting helper used across the frontend.
 *
 * Centralising diagnostics here keeps file/line/column formatting consistent
 * and makes it trivial to later add colour output or a JSON mode.
 * ========================================================================== */
#ifndef RPGC_DIAGNOSTICS_H
#define RPGC_DIAGNOSTICS_H

#include <functional>
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

/* Reset the error counter to zero (tools that parse multiple files in one
 * process, like rpg-analyze, call this between files). */
void reset_diagnostics();

using DiagnosticSink = std::function<void(const std::string &file, int line,
                                          int col, DiagKind kind,
                                          const std::string &message)>;

/* Install a callback that receives every diagnostic instead of the default
 * stderr print (the error counter is still updated either way). Pass an empty
 * std::function to restore the default stderr behavior. `rpgc` never installs
 * a sink; `rpg-analyze` uses this to fold parse diagnostics into findings
 * instead of leaking raw compiler chatter onto stderr. */
void set_diagnostic_sink(DiagnosticSink sink);

} // namespace rpgc

#endif // RPGC_DIAGNOSTICS_H
