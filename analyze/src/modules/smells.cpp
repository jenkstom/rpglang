/* smells -- anomaly / smell detection (TOOLS_IDEAS.md §4.20). */
#include "modules.h"
#include "../util.h"

#include <set>

namespace analyze {

namespace {

bool is_weird_op(rpgc::Op op) {
    return op == rpgc::Op::DEFN || op == rpgc::Op::BITON || op == rpgc::Op::BITOF ||
           op == rpgc::Op::MHHZO || op == rpgc::Op::MHLZO || op == rpgc::Op::MLHZO ||
           op == rpgc::Op::MLLZO || op == rpgc::Op::TESTB || op == rpgc::Op::TESTZ ||
           op == rpgc::Op::SORTA;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "smells";
    r.title = "Anomaly / smell detection";

    std::vector<std::string> notes;

    // SMELL-TEMPFLAG: a general indicator touched at 3+ sites reads like an
    // ad-hoc work flag rather than a documented condition.
    for (auto &kv : ir.indicators) {
        const IndicatorInfo &info = kv.second;
        if (info.klass != IndicatorClass::General) continue;
        size_t total = info.sets.size() + info.clears.size() + info.tests.size();
        if (total < 3) continue;
        Finding f;
        f.id = "SMELL-TEMPFLAG";
        f.severity = Severity::Info;
        f.module = "smells";
        f.message = "indicator " + info.label + " is touched at " + std::to_string(total) +
                    " sites; looks like an ad-hoc work flag rather than a documented condition";
        f.file = ir.path;
        f.line = info.sets.empty() ? (info.tests.empty() ? info.clears.front().line : info.tests.front().line)
                                    : info.sets.front().line;
        f.spec = 'C';
        r.findings.push_back(f);
        notes.push_back(f.message);
    }

    // SMELL-WEIRDOP
    for (auto &c : ir.prog.calcs) {
        if (!is_weird_op(c.op)) continue;
        Finding f;
        f.id = "SMELL-WEIRDOP";
        f.severity = Severity::Info;
        f.module = "smells";
        f.message = "unusual opcode " + c.op_text + " used";
        f.file = ir.path;
        f.line = c.lineno;
        f.spec = 'C';
        r.findings.push_back(f);
        notes.push_back(f.message);
    }

    // SMELL-HA-NODEC: half-adjust with no decimal positions to round.
    for (auto &c : ir.prog.calcs) {
        if (!c.half_adjust) continue;
        if (c.result_dec > 0) continue;
        Finding f;
        f.id = "SMELL-HA-NODEC";
        f.severity = Severity::Info;
        f.module = "smells";
        f.message = "half-adjust (H) set with no decimal positions on the result";
        f.file = ir.path;
        f.line = c.lineno;
        f.spec = 'C';
        f.columns = "53";
        r.findings.push_back(f);
        notes.push_back(f.message);
    }

    // SMELL-REDEFINE: the same subfield name defined under two different DSs.
    std::set<std::string> seen, dup;
    for (auto &sub : ir.prog.ds_subfields) {
        std::string name = upper_str(sub.name);
        if (name.empty()) continue;
        if (!seen.insert(name).second) dup.insert(name);
    }
    for (auto &name : dup) {
        Finding f;
        f.id = "SMELL-REDEFINE";
        f.severity = Severity::Info;
        f.module = "smells";
        f.message = "subfield name '" + name + "' is reused across multiple data structures";
        f.file = ir.path;
        f.line = 0;
        f.spec = 'I';
        r.findings.push_back(f);
        notes.push_back(f.message);
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "smells";
        sec.title = "Anomaly / smell detection";
        if (notes.empty()) sec.text_lines.push_back("(no smells detected)");
        else sec.text_lines = notes;
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_smells_module() {
    return ModuleInfo{"smells", "Anomaly / smell detection", "NOTES", run};
}

} // namespace analyze
