/* ========================================================================== *
 * module.h -- the analysis module contract (TOOLS_IDEAS.md §4, "Module
 * contract") and the module registry (catalog order = registration order).
 * ========================================================================== */
#ifndef RPGANALYZE_MODULE_H
#define RPGANALYZE_MODULE_H

#include "finding.h"
#include "ir.h"
#include "json.h"

#include <functional>
#include <string>
#include <vector>

namespace analyze {

struct Section {
    std::string id;
    std::string title;
    std::vector<std::string> text_lines;  // pre-rendered for the text renderer
    Json data = Json::object();           // payload for --json
};

struct ModuleResult {
    std::string id;
    std::string title;
    std::vector<Section> sections;
    std::vector<Finding> findings;
};

struct ModuleOptions {
    bool quiet = false;   // suppress section bodies (findings still computed)
};

using ModuleRunFn = std::function<ModuleResult(const ProgramIR &, const ModuleOptions &)>;

struct ModuleInfo {
    std::string id;
    std::string title;
    std::string section_group;   // report §5 grouping, e.g. "FILES"
    ModuleRunFn run;
};

/* Catalog order matches TOOLS_IDEAS.md §4.1-4.20. */
const std::vector<ModuleInfo> &module_catalog();
const ModuleInfo *find_module(const std::string &id);

} // namespace analyze

#endif // RPGANALYZE_MODULE_H
