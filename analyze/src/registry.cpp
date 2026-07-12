#include "module.h"
#include "modules/modules.h"

namespace analyze {

const std::vector<ModuleInfo> &module_catalog() {
    static const std::vector<ModuleInfo> catalog = [] {
        std::vector<ModuleInfo> v;
        v.push_back(make_summary_module());
        v.push_back(make_files_module());
        v.push_back(make_recordmap_module());
        v.push_back(make_fields_module());
        v.push_back(make_xref_module());
        v.push_back(make_indicators_module());
        v.push_back(make_subroutines_module());
        v.push_back(make_controlflow_module());
        v.push_back(make_cycle_module());
        v.push_back(make_complexity_module());
        v.push_back(make_deadcode_module());
        v.push_back(make_security_module());
        v.push_back(make_compat_module());
        v.push_back(make_condlogic_module());
        v.push_back(make_buffer_module());
        v.push_back(make_termination_module());
        v.push_back(make_dataflow_module());
        v.push_back(make_deps_module());
        v.push_back(make_comments_module());
        v.push_back(make_smells_module());
        return v;
    }();
    return catalog;
}

const ModuleInfo *find_module(const std::string &id) {
    for (auto &m : module_catalog())
        if (m.id == id) return &m;
    return nullptr;
}

} // namespace analyze
