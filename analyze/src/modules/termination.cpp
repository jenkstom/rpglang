// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* termination -- LR / exit analysis (TOOLS_IDEAS.md §4.16). */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "termination";
    r.title = "LR / exit analysis";

    std::vector<std::string> notes;
    const IndicatorInfo *lr = ir.find_indicator("LR");
    bool has_unconditional_lr = false;
    if (lr) {
        for (auto &s : lr->sets) {
            // Find the owning C-spec to check its own conditioning columns.
            for (auto &c : ir.prog.calcs) {
                if (c.lineno != s.line) continue;
                if (c.conditions.empty()) has_unconditional_lr = true;
                break;
            }
        }
    }

    // A Primary/Secondary input file turns LR on automatically at end-of-file
    // (the normal RPG II cycle-driven termination pattern), so "no explicit
    // SETON LR" is only suspect when nothing in the program can reach EOF and
    // there's no other way to end the program cycle.
    bool has_cycle_termination = false;
    for (auto &f : ir.prog.files) {
        if (f.design == rpgc::FileDesign::Primary || f.design == rpgc::FileDesign::Secondary) {
            has_cycle_termination = true;
            break;
        }
    }

    if ((!lr || lr->sets.empty()) && !has_cycle_termination) {
        Finding f;
        f.id = "TERM-NOLR";
        f.severity = Severity::Warn;
        f.module = "termination";
        f.message = "no SETON LR anywhere, and no Primary/Secondary input file to set it via "
                    "end-of-file; the program may never terminate on its own";
        f.file = ir.path;
        f.line = 0;
        f.spec = 'C';
        r.findings.push_back(f);
        notes.push_back(f.message);
    } else if (lr && !lr->sets.empty() && !has_unconditional_lr) {
        Finding f;
        f.id = "TERM-PATHNOLR";
        f.severity = Severity::Warn;
        f.module = "termination";
        f.message = "every SETON LR is conditioned; some reachable exit path may never set LR";
        f.file = ir.path;
        f.line = lr->sets.front().line;
        f.spec = 'C';
        for (auto &s : lr->sets) f.evidence.push_back({"termination", s.line});
        r.findings.push_back(f);
        notes.push_back(f.message);
    }

    for (auto &f : ir.prog.files) {
        if (f.design != rpgc::FileDesign::Primary && f.design != rpgc::FileDesign::Secondary) continue;
        if (f.type != rpgc::FileType::Input && f.type != rpgc::FileType::Combined) continue;
        if (f.end_required) continue;
        Finding fnd;
        fnd.id = "TERM-EOFNOTREQ";
        fnd.severity = Severity::Info;
        fnd.module = "termination";
        fnd.message = "primary/secondary input file '" + f.name +
                      "' is read every cycle but lacks the end-required flag (col 17)";
        fnd.file = ir.path;
        fnd.line = f.lineno;
        fnd.spec = 'F';
        fnd.columns = "17";
        r.findings.push_back(fnd);
        notes.push_back(fnd.message);
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "termination";
        sec.title = "LR / exit analysis";
        if (lr) {
            sec.text_lines.push_back("LR set at: ");
            for (auto &s : lr->sets) sec.text_lines.push_back("  line " + std::to_string(s.line));
        }
        for (auto &n : notes) sec.text_lines.push_back(n);
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_termination_module() {
    return ModuleInfo{"termination", "LR / exit analysis", "SECURITY", run};
}

} // namespace analyze
