/* condlogic -- conditioning-indicator reconstruction (TOOLS_IDEAS.md §4.14). */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

std::string cond_expr(const std::vector<rpgc::CondInd> &conds) {
    std::vector<std::string> parts;
    for (auto &c : conds) {
        std::string lbl = indicator_label(c.indicator);
        if (lbl.empty()) continue;
        parts.push_back((c.negate ? "N" : "") + lbl);
    }
    if (parts.empty()) return "";
    std::string out = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) out += " and " + parts[i];
    return out;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "condlogic";
    r.title = "Conditioning-indicator reconstruction";

    Section sec;
    sec.id = "condlogic";
    sec.title = "Conditioning-indicator reconstruction";
    Json arr = Json::array();

    for (auto &c : ir.prog.calcs) {
        if (c.conditions.empty()) continue;
        std::string expr = cond_expr(c.conditions);
        if (expr.empty()) continue;
        std::string stmt = c.op_text;
        if (!c.factor1.empty()) stmt += " " + c.factor1;
        if (!c.factor2.empty()) stmt += " " + c.factor2;
        if (!c.result.empty()) stmt += " " + c.result;

        if (!opts.quiet) {
            sec.text_lines.push_back(std::to_string(c.lineno) + ": if (" + expr + ") " + stmt);
        }
        Json j = Json::object();
        j.set("line", c.lineno);
        j.set("spec", "C");
        j.set("condition", expr);
        j.set("statement", stmt);
        arr.push_back(j);
    }

    for (auto &orec : ir.prog.outputs) {
        std::vector<std::string> groups;
        for (auto &grp : orec.conditions) {
            std::string e = cond_expr(grp);
            if (!e.empty()) groups.push_back(e);
        }
        if (!groups.empty()) {
            std::string joined = groups[0];
            for (size_t i = 1; i < groups.size(); ++i) joined += ") or (" + groups[i];
            std::string line = std::to_string(orec.lineno) + ": if ((" + joined + ")) write " + orec.file;
            if (!opts.quiet) sec.text_lines.push_back(line);
            Json j = Json::object();
            j.set("line", orec.lineno);
            j.set("spec", "O");
            j.set("condition", joined);
            j.set("statement", "write " + orec.file);
            arr.push_back(j);
        }
        for (auto &fld : orec.fields) {
            std::string e = cond_expr(fld.conditions);
            if (e.empty()) continue;
            std::string name = fld.is_const ? ("'" + fld.text + "'") : fld.name;
            if (!opts.quiet)
                sec.text_lines.push_back("  " + std::to_string(fld.lineno) + ": if (" + e + ") output " + name);
            Json j = Json::object();
            j.set("line", fld.lineno);
            j.set("spec", "O");
            j.set("condition", e);
            j.set("statement", "output " + name);
            arr.push_back(j);
        }
    }

    // COND-DEEPCOND: structured-block nesting depth beyond a threshold.
    const int kThreshold = 4;
    int depth = 0;
    for (auto &c : ir.prog.calcs) {
        if (c.op == rpgc::Op::IF || c.op == rpgc::Op::DOW || c.op == rpgc::Op::DOU || c.op == rpgc::Op::DO) {
            ++depth;
            if (depth > kThreshold) {
                Finding f;
                f.id = "COND-DEEPCOND";
                f.severity = Severity::Info;
                f.module = "condlogic";
                f.message = "conditioning/structured-block nesting reaches depth " + std::to_string(depth);
                f.file = ir.path;
                f.line = c.lineno;
                f.spec = 'C';
                r.findings.push_back(f);
            }
        } else if (c.op == rpgc::Op::END) {
            if (depth > 0) --depth;
        }
    }

    sec.data.set("entries", arr);
    if (!opts.quiet) r.sections.push_back(std::move(sec));

    return r;
}

} // namespace

ModuleInfo make_condlogic_module() {
    return ModuleInfo{"condlogic", "Conditioning-indicator reconstruction", "CONTROL FLOW", run};
}

} // namespace analyze
