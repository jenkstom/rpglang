#include "render_text.h"

#include <sstream>

namespace analyze {

namespace {

const char *RESET  = "\x1b[0m";
const char *BOLD    = "\x1b[1m";
const char *RED     = "\x1b[31m";
const char *YELLOW  = "\x1b[33m";
const char *CYAN    = "\x1b[36m";
const char *DIM     = "\x1b[2m";

std::string colorize(const char *code, const std::string &s, bool color) {
    if (!color) return s;
    return std::string(code) + s + RESET;
}

const char *sev_color(Severity s) {
    switch (s) {
        case Severity::Error: return RED;
        case Severity::Warn:  return YELLOW;
        case Severity::Info:  return CYAN;
    }
    return "";
}

} // namespace

std::string render_text(const Report &rep, const TextRenderOptions &opts) {
    std::ostringstream out;
    std::string rule(63, '=');

    out << colorize(BOLD, rule, opts.color) << "\n";
    out << " RPG ANALYSIS -- " << rep.program_id << "  (" << rep.file << ")\n";
    out << colorize(BOLD, rule, opts.color) << "\n\n";

    for (auto &res : rep.results) {
        if (res.sections.empty()) continue;
        for (auto &sec : res.sections) {
            if (sec.text_lines.empty()) continue;
            out << colorize(BOLD, "-- " + sec.title + " [" + res.id + "] --", opts.color) << "\n";
            for (auto &line : sec.text_lines) out << line << "\n";
            out << "\n";
        }
    }

    if (!opts.no_findings) {
        auto shown = filter_severity(rep.findings, opts.min_severity);
        out << colorize(BOLD, "=== FINDINGS (" + std::to_string(shown.size()) + ") ===", opts.color) << "\n";
        for (auto &f : shown) {
            std::string tag = "[" + std::string(severity_text(f.severity)) + "]";
            out << " " << colorize(sev_color(f.severity), tag, opts.color)
                << "  " << f.id << "  " << f.file << ":" << f.line
                << "  " << f.message << "\n";
            for (auto &e : f.evidence) {
                out << colorize(DIM, "         -> evidence: see \xc2\xa7" + e.section +
                                (e.line ? (":" + std::to_string(e.line)) : ""), opts.color) << "\n";
            }
        }
        out << "\n";
        FindingCounts c = count_by_severity(rep.findings);
        out << "Summary: " << c.errors << " error(s), " << c.warnings << " warning(s), "
            << c.infos << " info.\n";
    }

    return out.str();
}

} // namespace analyze
