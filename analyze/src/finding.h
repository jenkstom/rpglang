/* ========================================================================== *
 * finding.h -- the Finding record and the findings list (dedup/sort/filter).
 *
 * Every analysis module emits Findings; report.cpp merges them from every
 * enabled module into one sorted, de-duplicated list. See TOOLS_IDEAS.md §5.1.
 * ========================================================================== */
#ifndef RPGANALYZE_FINDING_H
#define RPGANALYZE_FINDING_H

#include <string>
#include <vector>

namespace analyze {

enum class Severity { Error, Warn, Info };

const char *severity_text(Severity s);
/* Parses "error"/"warn"/"info" (case-insensitive). Returns false if unknown. */
bool parse_severity(const std::string &s, Severity &out);

struct EvidenceRef {
    std::string section;   // module/section id this evidence points at
    int         line = 0;
};

struct Finding {
    std::string id;         // stable check id, e.g. "BUF-OVERLAP"
    Severity    severity = Severity::Info;
    std::string module;     // producing module id
    std::string message;    // one-line human description

    // location
    std::string file;
    int         line = 0;
    char        spec = ' ';     // form-type letter, ' ' if not applicable
    std::string columns;        // e.g. "9-17"

    std::vector<EvidenceRef> evidence;
    std::string rule;       // optional doc anchor, e.g. "SPEC_MAP.md#..."
};

/* Sorts by severity (Error, Warn, Info) then file/line/id; merges findings
 * that share the same id+file+line (unioning their evidence lists). */
void dedup_and_sort(std::vector<Finding> &findings);

/* Keeps only findings at or above `min` severity (Error is the highest). */
std::vector<Finding> filter_severity(const std::vector<Finding> &findings, Severity min);

struct FindingCounts { int errors = 0, warnings = 0, infos = 0; };
FindingCounts count_by_severity(const std::vector<Finding> &findings);

} // namespace analyze

#endif // RPGANALYZE_FINDING_H
