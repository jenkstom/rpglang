// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* deadcode -- unused-definition & dead-path finder (TOOLS_IDEAS.md §4.11).
 *
 * Consolidates the dead-code findings that xref/indicators/subroutines/
 * controlflow each compute individually (DEAD-FIELD, IND-UNUSED, DEAD-SUBR,
 * FLOW-UNREACHABLE), recomputed here directly from the shared IR so this
 * module stays a self-contained function per the module contract, plus the
 * one check unique to this module: DEAD-FILE.
 */
#include "modules.h"
#include "../util.h"

#include <set>

namespace analyze {

namespace {

bool is_marker_op(rpgc::Op op) {
    return op == rpgc::Op::BEGSR || op == rpgc::Op::ENDSR || op == rpgc::Op::TAG;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "deadcode";
    r.title = "Dead code finder";

    Section sec;
    sec.id = "deadcode";
    sec.title = "Dead code finder";

    // DEAD-FIELD
    std::vector<std::string> dead_fields;
    for (auto &name : ir.symbol_order) {
        const SymbolInfo &s = ir.symbols.at(name);
        bool declared_kind = s.kind == SymbolKind::Input || s.kind == SymbolKind::LookAhead ||
                             s.kind == SymbolKind::DSSubfield || s.kind == SymbolKind::Array ||
                             s.kind == SymbolKind::Table;
        if (!declared_kind || s.refs.size() > 1) continue;
        dead_fields.push_back(s.name);
        Finding f;
        f.id = "DEAD-FIELD";
        f.severity = Severity::Info;
        f.module = "deadcode";
        f.message = "field '" + s.name + "' is defined but never referenced";
        f.file = ir.path;
        f.line = s.def_line;
        r.findings.push_back(f);
    }

    // IND-UNUSED
    std::vector<std::string> dead_indicators;
    for (auto &kv : ir.indicators) {
        const IndicatorInfo &info = kv.second;
        bool has_set = !info.sets.empty() || !info.clears.empty();
        if (!has_set || !info.tests.empty() || info.klass == IndicatorClass::LastRecord) continue;
        dead_indicators.push_back(info.label);
        Finding f;
        f.id = "IND-UNUSED";
        f.severity = Severity::Info;
        f.module = "deadcode";
        f.message = "indicator " + info.label + " is set but never tested";
        f.file = ir.path;
        f.line = info.sets.empty() ? info.clears.front().line : info.sets.front().line;
        f.spec = 'C';
        r.findings.push_back(f);
    }

    // DEAD-SUBR
    std::set<std::string> called;
    for (auto &sr : ir.subroutines)
        for (auto &c : sr.calls) called.insert(c);
    for (size_t i = 0; i < ir.prog.calcs.size(); ++i) {
        auto &c = ir.prog.calcs[i];
        bool in_any_sub = false;
        for (auto &sr : ir.subroutines)
            if ((int)i > sr.begin_idx && (int)i < sr.end_idx) { in_any_sub = true; break; }
        if (in_any_sub) continue;
        if (c.op == rpgc::Op::EXSR && !c.factor2.empty()) called.insert(upper_str(c.factor2));
        else if (c.op == rpgc::Op::CAS && !c.result.empty()) called.insert(upper_str(c.result));
    }
    std::vector<std::string> dead_subs;
    for (auto &sr : ir.subroutines) {
        if (called.count(sr.name)) continue;
        dead_subs.push_back(sr.name);
        Finding f;
        f.id = "DEAD-SUBR";
        f.severity = Severity::Info;
        f.module = "deadcode";
        f.message = "subroutine '" + sr.name + "' is never invoked by EXSR/CASxx";
        f.file = ir.path;
        f.line = sr.begin_line;
        f.spec = 'C';
        r.findings.push_back(f);
    }

    // FLOW-UNREACHABLE
    std::vector<int> unreachable_lines;
    for (size_t i = 0; i < ir.prog.calcs.size(); ++i) {
        if (is_marker_op(ir.prog.calcs[i].op)) continue;
        if (ir.cfg.reachable[i]) continue;
        unreachable_lines.push_back(ir.prog.calcs[i].lineno);
        Finding f;
        f.id = "FLOW-UNREACHABLE";
        f.severity = Severity::Warn;
        f.module = "deadcode";
        f.message = "line " + std::to_string(ir.prog.calcs[i].lineno) + " (" + ir.prog.calcs[i].op_text +
                    ") has no entry path";
        f.file = ir.path;
        f.line = ir.prog.calcs[i].lineno;
        f.spec = 'C';
        r.findings.push_back(f);
    }

    // DEAD-FILE: output files never written to, non-primary/secondary input
    // files never explicitly READ/CHAINed.
    std::set<std::string> touched;
    for (auto &c : ir.prog.calcs) {
        if ((c.op == rpgc::Op::CHAIN || c.op == rpgc::Op::SETLL || c.op == rpgc::Op::READE ||
             c.op == rpgc::Op::READ || c.op == rpgc::Op::READP) && !c.factor2.empty())
            touched.insert(upper_str(c.factor2));
    }
    for (auto &orec : ir.prog.outputs) touched.insert(upper_str(orec.file));

    std::vector<std::string> dead_files;
    for (auto &f : ir.prog.files) {
        bool needs_check = false;
        if (f.type == rpgc::FileType::Output || f.type == rpgc::FileType::Combined) needs_check = true;
        if ((f.type == rpgc::FileType::Input || f.type == rpgc::FileType::Update) &&
            (f.design == rpgc::FileDesign::Chained || f.design == rpgc::FileDesign::Demand ||
             f.design == rpgc::FileDesign::FullProc || f.design == rpgc::FileDesign::Table))
            needs_check = true;
        if (!needs_check) continue;
        if (touched.count(upper_str(f.name))) continue;
        dead_files.push_back(f.name);
        Finding fnd;
        fnd.id = "DEAD-FILE";
        fnd.severity = Severity::Info;
        fnd.module = "deadcode";
        fnd.message = "file '" + f.name + "' is declared but never read or written";
        fnd.file = ir.path;
        fnd.line = f.lineno;
        fnd.spec = 'F';
        r.findings.push_back(fnd);
    }

    if (!opts.quiet) {
        sec.text_lines.push_back("Dead fields (" + std::to_string(dead_fields.size()) + "): " +
                                 (dead_fields.empty() ? "none" : ""));
        for (auto &n : dead_fields) sec.text_lines.push_back("  " + n);
        sec.text_lines.push_back("Unused indicators (" + std::to_string(dead_indicators.size()) + "): " +
                                 (dead_indicators.empty() ? "none" : ""));
        for (auto &n : dead_indicators) sec.text_lines.push_back("  " + n);
        sec.text_lines.push_back("Dead subroutines (" + std::to_string(dead_subs.size()) + "): " +
                                 (dead_subs.empty() ? "none" : ""));
        for (auto &n : dead_subs) sec.text_lines.push_back("  " + n);
        sec.text_lines.push_back("Unreachable lines (" + std::to_string(unreachable_lines.size()) + "): " +
                                 (unreachable_lines.empty() ? "none" : ""));
        for (int l : unreachable_lines) sec.text_lines.push_back("  line " + std::to_string(l));
        sec.text_lines.push_back("Dead files (" + std::to_string(dead_files.size()) + "): " +
                                 (dead_files.empty() ? "none" : ""));
        for (auto &n : dead_files) sec.text_lines.push_back("  " + n);

        Json j = Json::object();
        auto to_arr = [](const std::vector<std::string> &v) {
            Json a = Json::array();
            for (auto &s : v) a.push_back(Json(s));
            return a;
        };
        j.set("dead_fields", to_arr(dead_fields));
        j.set("unused_indicators", to_arr(dead_indicators));
        j.set("dead_subroutines", to_arr(dead_subs));
        Json ul = Json::array();
        for (int l : unreachable_lines) ul.push_back(Json(l));
        j.set("unreachable_lines", ul);
        j.set("dead_files", to_arr(dead_files));
        sec.data = j;
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_deadcode_module() {
    return ModuleInfo{"deadcode", "Dead code finder", "DEAD CODE", run};
}

} // namespace analyze
