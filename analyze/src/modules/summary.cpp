// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* summary -- program profile (TOOLS_IDEAS.md §4.1). */
#include "modules.h"
#include "../util.h"

#include <set>

namespace analyze {

namespace {

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "summary";
    r.title = "Overview";

    std::map<char, int> spec_counts;
    for (auto &sl : ir.raw_lines) {
        if (sl.comment) continue;
        spec_counts[form_type(sl)]++;
    }

    bool has_control_levels = false, has_matching = false, has_ext_ind = false, has_multifile = false;
    int input_file_count = 0;
    for (auto &f : ir.prog.files) if (f.type == rpgc::FileType::Input || f.type == rpgc::FileType::Combined) ++input_file_count;
    if (input_file_count > 1) has_multifile = true;
    for (auto &f : ir.prog.in_fields) {
        if (!f.control_level.empty()) has_control_levels = true;
        if (!f.match_field.empty()) has_matching = true;
    }
    for (auto &kv : ir.indicators)
        if (kv.second.klass == IndicatorClass::External) has_ext_ind = true;

    int array_count = 0, table_count = 0;
    for (auto &a : ir.prog.arrays) {
        if (rpgc::is_table_name(a.name)) ++table_count; else ++array_count;
    }

    std::set<std::string> indicator_labels;
    for (auto &kv : ir.indicators) indicator_labels.insert(kv.first);

    if (!opts.quiet) {
        Section sec;
        sec.id = "summary";
        sec.title = "Overview";
        sec.text_lines.push_back("Program ID:        " + ir.program_id);
        sec.text_lines.push_back("Source file:       " + ir.path);
        sec.text_lines.push_back("");
        sec.text_lines.push_back("Spec line counts:");
        for (auto &kv : spec_counts) {
            if (kv.first == ' ') continue;
            sec.text_lines.push_back(std::string("  ") + kv.first + "  " + std::to_string(kv.second));
        }
        sec.text_lines.push_back("");
        sec.text_lines.push_back("Files:              " + std::to_string(ir.prog.files.size()));
        sec.text_lines.push_back("Indicators used:    " + std::to_string(indicator_labels.size()));
        sec.text_lines.push_back("Arrays / tables:    " + std::to_string(array_count) + " / " + std::to_string(table_count));
        sec.text_lines.push_back("Subroutines:        " + std::to_string(ir.subroutines.size()));
        sec.text_lines.push_back("C-spec count:       " + std::to_string(ir.prog.calcs.size()));
        sec.text_lines.push_back("");
        sec.text_lines.push_back(std::string("Control levels:     ") + (has_control_levels ? "yes" : "no"));
        sec.text_lines.push_back(std::string("Matching fields:    ") + (has_matching ? "yes" : "no"));
        sec.text_lines.push_back(std::string("External indicators:") + (has_ext_ind ? " yes" : " no"));
        sec.text_lines.push_back(std::string("Multifile:          ") + (has_multifile ? "yes" : "no"));

        Json data = Json::object();
        Json specs = Json::object();
        for (auto &kv : spec_counts) if (kv.first != ' ') specs.set(std::string(1, kv.first), (int)kv.second);
        data.set("program_id", ir.program_id);
        data.set("spec_line_counts", specs);
        data.set("file_count", (int)ir.prog.files.size());
        data.set("indicator_count", (int)indicator_labels.size());
        data.set("array_count", array_count);
        data.set("table_count", table_count);
        data.set("subroutine_count", (int)ir.subroutines.size());
        data.set("cspec_count", (int)ir.prog.calcs.size());
        data.set("has_control_levels", has_control_levels);
        data.set("has_matching_fields", has_matching);
        data.set("has_external_indicators", has_ext_ind);
        data.set("multifile", has_multifile);
        sec.data = data;

        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_summary_module() {
    return ModuleInfo{"summary", "Overview", "OVERVIEW", run};
}

} // namespace analyze
