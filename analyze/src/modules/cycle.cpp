// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* cycle -- program-cycle / control-level decoder (TOOLS_IDEAS.md §4.9). */
#include "modules.h"
#include "../util.h"

#include <map>
#include <set>

namespace analyze {

namespace {

const char *design_text(rpgc::FileDesign d) {
    switch (d) {
        case rpgc::FileDesign::Primary:    return "Primary";
        case rpgc::FileDesign::Secondary:  return "Secondary";
        case rpgc::FileDesign::FullProc:   return "Full Procedural";
        case rpgc::FileDesign::Chained:    return "Chained";
        case rpgc::FileDesign::RecordAddr: return "Record Address";
        case rpgc::FileDesign::Table:      return "Table";
        case rpgc::FileDesign::Demand:     return "Demand";
        case rpgc::FileDesign::None:       return "None";
    }
    return "?";
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "cycle";
    r.title = "Program-cycle decoder";

    Section sec;
    sec.id = "cycle";
    sec.title = "Program-cycle decoder";
    Json files_json = Json::array();

    for (auto &f : ir.prog.files) {
        if (f.type != rpgc::FileType::Input && f.type != rpgc::FileType::Combined) continue;

        std::vector<std::string> levels, matches;
        for (auto &fld : ir.prog.in_fields) {
            if (fld.file != f.name) continue;
            if (!fld.control_level.empty()) levels.push_back(fld.control_level + ":" + fld.name);
            if (!fld.match_field.empty()) matches.push_back(fld.match_field + ":" + fld.name);
        }

        if (!opts.quiet) {
            sec.text_lines.push_back(f.name + "  [" + design_text(f.design) + "]" +
                                     (f.sequence == 'D' ? "  (descending)" : "") +
                                     (f.end_required ? "  (EOF required before LR)" : ""));
            for (auto &l : levels) sec.text_lines.push_back("  control level " + l);
            for (auto &m : matches) sec.text_lines.push_back("  matching field " + m);
        }

        Json j = Json::object();
        j.set("file", f.name);
        j.set("design", design_text(f.design));
        j.set("sequence", std::string(1, f.sequence ? f.sequence : 'A'));
        j.set("end_required", f.end_required);
        Json lv = Json::array();
        for (auto &l : levels) lv.push_back(Json(l));
        j.set("control_levels", lv);
        Json mf = Json::array();
        for (auto &m : matches) mf.push_back(Json(m));
        j.set("matching_fields", mf);
        files_json.push_back(j);
    }
    sec.data.set("files", files_json);

    // CYC-MULTIMATCH: any M-field beyond a single M1, or combined match keys
    // (more than one matching field within the same file).
    std::map<std::string, int> match_per_file;
    std::set<std::string> match_labels;
    for (auto &fld : ir.prog.in_fields) {
        if (fld.match_field.empty()) continue;
        match_labels.insert(fld.match_field);
        match_per_file[fld.file]++;
        if (fld.match_field != "M1") {
            Finding f;
            f.id = "CYC-MULTIMATCH";
            f.severity = Severity::Warn;
            f.module = "cycle";
            f.message = "field '" + fld.name + "' uses matching field " + fld.match_field +
                        "; only a single M1 is supported";
            f.file = ir.path;
            f.line = fld.lineno;
            f.spec = 'I';
            f.columns = "61-62";
            r.findings.push_back(f);
        }
    }
    for (auto &kv : match_per_file) {
        if (kv.second > 1) {
            Finding f;
            f.id = "CYC-MULTIMATCH";
            f.severity = Severity::Warn;
            f.module = "cycle";
            f.message = "file '" + kv.first + "' declares " + std::to_string(kv.second) +
                        " matching fields (combined-key matching is not supported)";
            f.file = ir.path;
            f.line = 0;
            f.spec = 'I';
            r.findings.push_back(f);
        }
    }

    // CYC-ORPHANCL: a control-level field with no calc/output conditioned on it.
    for (auto &kv : ir.indicators) {
        const IndicatorInfo &info = kv.second;
        if (info.klass != IndicatorClass::ControlLevel) continue;
        if (!info.sets.empty() && info.tests.empty()) {
            Finding f;
            f.id = "CYC-ORPHANCL";
            f.severity = Severity::Info;
            f.module = "cycle";
            f.message = "control level " + info.label + " is set on a field but no detail/total " +
                        "calculation is conditioned on it";
            f.file = ir.path;
            f.line = info.sets.front().line;
            f.spec = 'I';
            r.findings.push_back(f);
        }
    }

    if (!opts.quiet) r.sections.push_back(std::move(sec));
    return r;
}

} // namespace

ModuleInfo make_cycle_module() {
    return ModuleInfo{"cycle", "Program-cycle decoder", "PROGRAM CYCLE", run};
}

} // namespace analyze
