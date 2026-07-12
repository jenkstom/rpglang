#include "report.h"

#include <algorithm>

namespace analyze {

std::vector<std::string> resolve_modules(const ReportOptions &opts) {
    std::vector<std::string> ids;
    if (opts.all || opts.modules.empty()) {
        for (auto &m : module_catalog()) ids.push_back(m.id);
    } else {
        for (auto &m : module_catalog())
            if (std::find(opts.modules.begin(), opts.modules.end(), m.id) != opts.modules.end())
                ids.push_back(m.id);
    }
    if (!opts.excluded.empty()) {
        ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string &id) {
            return std::find(opts.excluded.begin(), opts.excluded.end(), id) != opts.excluded.end();
        }), ids.end());
    }
    if (!opts.section_order.empty()) {
        std::vector<std::string> ordered;
        for (auto &want : opts.section_order)
            if (std::find(ids.begin(), ids.end(), want) != ids.end())
                ordered.push_back(want);
        for (auto &id : ids)
            if (std::find(ordered.begin(), ordered.end(), id) == ordered.end())
                ordered.push_back(id);
        ids = std::move(ordered);
    }
    return ids;
}

Report run_report(const ProgramIR &ir, const ReportOptions &opts) {
    Report rep;
    rep.file = ir.path;
    rep.program_id = ir.program_id;

    ModuleOptions mopts;
    mopts.quiet = opts.quiet;

    for (auto &id : resolve_modules(opts)) {
        const ModuleInfo *m = find_module(id);
        if (!m) continue;
        ModuleResult res = m->run(ir, mopts);
        rep.modules_run.push_back(id);
        for (auto &f : res.findings) rep.findings.push_back(f);
        rep.results.push_back(std::move(res));
    }

    dedup_and_sort(rep.findings);
    return rep;
}

} // namespace analyze
