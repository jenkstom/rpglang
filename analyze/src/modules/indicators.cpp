// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* indicators -- indicator lifecycle trace (TOOLS_IDEAS.md §4.6). */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

void emit_sites(std::vector<std::string> &lines, const char *label, const std::vector<RefSite> &sites) {
    if (sites.empty()) return;
    lines.push_back(std::string("  ") + label + ":");
    for (auto &s : sites) {
        std::string line = std::string("    ") + s.spec + " " + std::to_string(s.line) + "  " + s.role;
        if (s.negate) line += " (negated)";
        lines.push_back(line);
    }
}

Json sites_json(const std::vector<RefSite> &sites) {
    Json arr = Json::array();
    for (auto &s : sites) {
        Json j = Json::object();
        j.set("spec", std::string(1, s.spec));
        j.set("line", s.line);
        j.set("role", s.role);
        j.set("negate", s.negate);
        arr.push_back(j);
    }
    return arr;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "indicators";
    r.title = "Indicator lifecycle trace";

    Section sec;
    sec.id = "indicators";
    sec.title = "Indicator lifecycle trace";
    Json arr = Json::array();

    for (auto &kv : ir.indicators) {
        const IndicatorInfo &info = kv.second;
        bool has_set = !info.sets.empty();
        bool has_clear = !info.clears.empty();
        bool has_test = !info.tests.empty();

        if (!opts.quiet) {
            sec.text_lines.push_back(info.label + " (" + indicator_class_text(info.klass) + "):");
            emit_sites(sec.text_lines, "set", info.sets);
            emit_sites(sec.text_lines, "clear", info.clears);
            emit_sites(sec.text_lines, "test", info.tests);
        }

        Json j = Json::object();
        j.set("label", info.label);
        j.set("class", indicator_class_text(info.klass));
        j.set("sets", sites_json(info.sets));
        j.set("clears", sites_json(info.clears));
        j.set("tests", sites_json(info.tests));
        arr.push_back(j);

        bool controllable = info.klass == IndicatorClass::General || info.klass == IndicatorClass::ControlLevel;
        if (has_test && !has_set && !has_clear && controllable) {
            Finding f;
            f.id = "IND-UNSET";
            f.severity = Severity::Warn;
            f.module = "indicators";
            f.message = "indicator " + info.label + " is tested but never set";
            f.file = ir.path;
            f.line = info.tests.front().line;
            f.spec = 'C';
            for (auto &t : info.tests) f.evidence.push_back({"indicators", t.line});
            r.findings.push_back(f);
        }
        if ((has_set || has_clear) && !has_test && info.klass != IndicatorClass::LastRecord) {
            Finding f;
            f.id = "IND-UNUSED";
            f.severity = Severity::Info;
            f.module = "indicators";
            f.message = "indicator " + info.label + " is set but never tested";
            f.file = ir.path;
            f.line = has_set ? info.sets.front().line : info.clears.front().line;
            f.spec = 'C';
            r.findings.push_back(f);
        }
    }

    sec.data.set("indicators", arr);
    if (!opts.quiet) r.sections.push_back(std::move(sec));

    return r;
}

} // namespace

ModuleInfo make_indicators_module() {
    return ModuleInfo{"indicators", "Indicator lifecycle trace", "INDICATORS", run};
}

} // namespace analyze
