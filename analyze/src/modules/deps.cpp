// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* deps -- external dependency inventory (TOOLS_IDEAS.md §4.18).
 *
 * `CALL`/`CABL` inter-program calls aren't in this compiler's opcode set (an
 * unrecognized CALL would already surface as COMPAT-OP), so "called programs"
 * is always empty here; the inventory covers files and external indicators.
 */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "deps";
    r.title = "External dependency inventory";

    std::vector<std::string> input_files, output_files;
    for (auto &f : ir.prog.files) {
        if (f.type == rpgc::FileType::Input || f.type == rpgc::FileType::Combined || f.type == rpgc::FileType::Update)
            input_files.push_back(f.name);
        if (f.type == rpgc::FileType::Output || f.type == rpgc::FileType::Combined || f.type == rpgc::FileType::Update)
            output_files.push_back(f.name);
    }

    std::vector<std::string> ext_indicators;
    for (auto &kv : ir.indicators)
        if (kv.second.klass == IndicatorClass::External) ext_indicators.push_back(kv.first);

    if (!opts.quiet) {
        Section sec;
        sec.id = "deps";
        sec.title = "External dependency inventory";
        sec.text_lines.push_back("Input files:");
        for (auto &n : input_files) sec.text_lines.push_back("  " + n);
        sec.text_lines.push_back("Output files:");
        for (auto &n : output_files) sec.text_lines.push_back("  " + n);
        sec.text_lines.push_back("External indicators expected:");
        for (auto &n : ext_indicators) sec.text_lines.push_back("  " + n);
        sec.text_lines.push_back("Called programs: (none -- CALL/CABL is not part of this compiler's opcode set)");

        auto to_arr = [](const std::vector<std::string> &v) {
            Json a = Json::array();
            for (auto &s : v) a.push_back(Json(s));
            return a;
        };
        sec.data.set("input_files", to_arr(input_files));
        sec.data.set("output_files", to_arr(output_files));
        sec.data.set("external_indicators", to_arr(ext_indicators));
        sec.data.set("called_programs", Json::array());
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_deps_module() {
    return ModuleInfo{"deps", "External dependency inventory", "DEPENDENCIES", run};
}

} // namespace analyze
