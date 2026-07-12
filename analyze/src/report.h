/* ========================================================================== *
 * report.h -- runs the enabled modules, merges findings, produces a Report.
 * ========================================================================== */
#ifndef RPGANALYZE_REPORT_H
#define RPGANALYZE_REPORT_H

#include "finding.h"
#include "ir.h"
#include "module.h"

#include <string>
#include <vector>

namespace analyze {

struct ReportOptions {
    std::vector<std::string> modules;    // -m NAME (repeatable); empty + !all => all
    std::vector<std::string> excluded;   // --no-module NAME
    bool all = false;                    // -a / --all
    bool no_findings = false;
    std::vector<std::string> section_order;  // optional override of run order
    bool quiet = false;                  // --quiet: suppress section bodies
};

struct Report {
    std::string file;
    std::string program_id;
    std::vector<std::string> modules_run;
    std::vector<ModuleResult> results;   // one per enabled module, in run order
    std::vector<Finding> findings;       // merged, deduped, sorted (unfiltered)
};

/* Resolves which module ids are enabled, in run order, given ReportOptions. */
std::vector<std::string> resolve_modules(const ReportOptions &opts);

Report run_report(const ProgramIR &ir, const ReportOptions &opts);

} // namespace analyze

#endif // RPGANALYZE_REPORT_H
