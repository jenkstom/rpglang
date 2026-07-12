/* controlflow -- control-flow graph & reachability (TOOLS_IDEAS.md §4.8). */
#include "modules.h"
#include "../util.h"

#include <algorithm>

namespace analyze {

namespace {

/* Name of the subroutine containing calcs[idx], or "" for the main body. */
std::string owning_subroutine(const ProgramIR &ir, int idx) {
    for (auto &sr : ir.subroutines)
        if (idx >= sr.begin_idx && idx <= sr.end_idx) return sr.name;
    return "";
}

bool is_marker_op(rpgc::Op op) {
    return op == rpgc::Op::BEGSR || op == rpgc::Op::ENDSR || op == rpgc::Op::TAG;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "controlflow";
    r.title = "Control-flow graph";

    auto &calcs = ir.prog.calcs;

    int goto_count = 0, tag_count = 0, max_depth = 0;
    {
        int depth = 0;
        for (auto &c : calcs) {
            if (c.op == rpgc::Op::GOTO) ++goto_count;
            if (c.op == rpgc::Op::TAG) ++tag_count;
            if (c.op == rpgc::Op::IF || c.op == rpgc::Op::DOW || c.op == rpgc::Op::DOU || c.op == rpgc::Op::DO) {
                ++depth;
                max_depth = std::max(max_depth, depth);
            } else if (c.op == rpgc::Op::END) {
                if (depth > 0) --depth;
            }
        }
    }

    std::vector<int> unreachable;
    for (size_t i = 0; i < calcs.size(); ++i) {
        if (is_marker_op(calcs[i].op)) continue;
        if (!ir.cfg.reachable[i]) unreachable.push_back((int)i);
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "controlflow";
        sec.title = "Control-flow graph";

        sec.text_lines.push_back("GOTO count: " + std::to_string(goto_count) +
                                 "   TAG count: " + std::to_string(tag_count) +
                                 "   max structured nesting depth: " + std::to_string(max_depth));
        if (!ir.cfg.tag_index.empty()) {
            sec.text_lines.push_back("");
            sec.text_lines.push_back("Labels:");
            for (auto &kv : ir.cfg.tag_index)
                sec.text_lines.push_back("  " + kv.first + "  -> line " + std::to_string(calcs[kv.second].lineno));
        }
        if (!unreachable.empty()) {
            sec.text_lines.push_back("");
            sec.text_lines.push_back("Unreachable lines:");
            for (int idx : unreachable)
                sec.text_lines.push_back("  line " + std::to_string(calcs[idx].lineno) + "  " + calcs[idx].op_text);
        }

        Json labels = Json::array();
        for (auto &kv : ir.cfg.tag_index) {
            Json j = Json::object();
            j.set("name", kv.first);
            j.set("line", calcs[kv.second].lineno);
            labels.push_back(j);
        }
        Json unreach = Json::array();
        for (int idx : unreachable) unreach.push_back(Json(calcs[idx].lineno));
        sec.data.set("goto_count", goto_count);
        sec.data.set("tag_count", tag_count);
        sec.data.set("max_nesting_depth", max_depth);
        sec.data.set("labels", labels);
        sec.data.set("unreachable_lines", unreach);
        r.sections.push_back(std::move(sec));
    }

    for (int idx : unreachable) {
        Finding f;
        f.id = "FLOW-UNREACHABLE";
        f.severity = Severity::Warn;
        f.module = "controlflow";
        f.message = "line " + std::to_string(calcs[idx].lineno) + " (" + calcs[idx].op_text +
                    ") has no entry path";
        f.file = ir.path;
        f.line = calcs[idx].lineno;
        f.spec = 'C';
        r.findings.push_back(f);
    }

    if (tag_count > 0) {
        double ratio = (double)goto_count / (double)std::max(1, (int)calcs.size());
        if (goto_count >= 5 && ratio > 0.15) {
            Finding f;
            f.id = "FLOW-GOTODENSITY";
            f.severity = Severity::Warn;
            f.module = "controlflow";
            f.message = "high GOTO density (" + std::to_string(goto_count) + " GOTOs / " +
                        std::to_string(calcs.size()) + " C-specs)";
            f.file = ir.path;
            f.line = 0;
            f.spec = 'C';
            r.findings.push_back(f);
        }
    }

    for (size_t i = 0; i < calcs.size(); ++i) {
        auto &c = calcs[i];
        if (c.op != rpgc::Op::GOTO || c.factor2.empty()) continue;
        auto it = ir.cfg.tag_index.find(upper_str(c.factor2));
        if (it == ir.cfg.tag_index.end()) continue;
        std::string from_sub = owning_subroutine(ir, (int)i);
        std::string to_sub = owning_subroutine(ir, it->second);
        if (from_sub != to_sub) {
            Finding f;
            f.id = "FLOW-CROSSSUBR";
            f.severity = Severity::Error;
            f.module = "controlflow";
            f.message = "GOTO " + c.factor2 + " crosses a subroutine boundary (" +
                        (from_sub.empty() ? "main body" : from_sub) + " -> " +
                        (to_sub.empty() ? "main body" : to_sub) + ")";
            f.file = ir.path;
            f.line = c.lineno;
            f.spec = 'C';
            f.columns = "33-42";
            r.findings.push_back(f);
        }
    }

    return r;
}

} // namespace

ModuleInfo make_controlflow_module() {
    return ModuleInfo{"controlflow", "Control-flow graph", "CONTROL FLOW", run};
}

} // namespace analyze
