/* ========================================================================== *
 * diagnostics.cpp -- implementation of the diagnostic emitter.
 * ========================================================================== */
#include "diagnostics.h"

#include <cstdio>

namespace rpgc {

namespace {
const char *kind_str(DiagKind k) {
    switch (k) {
        case DiagKind::Note:    return "note";
        case DiagKind::Warning: return "warning";
        case DiagKind::Error:   return "error";
        case DiagKind::Fatal:   return "fatal error";
    }
    return "error";
}

int g_error_count = 0;
} // namespace

void report(const std::string &file,
            int line,
            int col,
            DiagKind kind,
            const std::string &message) {
    std::fprintf(stderr, "%s:", file.c_str());
    if (line > 0) {
        std::fprintf(stderr, "%d:", line);
        if (col > 0) std::fprintf(stderr, "%d:", col);
    }
    std::fprintf(stderr, " %s: %s\n", kind_str(kind), message.c_str());

    if (kind == DiagKind::Error || kind == DiagKind::Fatal) {
        ++g_error_count;
    }
}

void error(const std::string &msg) {
    report("<rpgc>", 0, 0, DiagKind::Error, msg);
}

void fatal(const std::string &msg) {
    report("<rpgc>", 0, 0, DiagKind::Fatal, msg);
}

int error_count() {
    return g_error_count;
}

} // namespace rpgc
