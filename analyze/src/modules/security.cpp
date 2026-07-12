/* security -- risky-pattern scanner (TOOLS_IDEAS.md §4.12). */
#include "modules.h"
#include "../util.h"

#include <set>

namespace analyze {

namespace {

bool is_read_op(rpgc::Op op) {
    return op == rpgc::Op::CHAIN || op == rpgc::Op::READ || op == rpgc::Op::READE || op == rpgc::Op::READP;
}

bool is_numeric_literal(const std::string &tok) {
    if (tok.empty()) return false;
    size_t i = 0;
    if (tok[i] == '-') ++i;
    bool any = false;
    for (; i < tok.size(); ++i) {
        if (std::isdigit((unsigned char)tok[i])) { any = true; continue; }
        if (tok[i] == '.') continue;
        return false;
    }
    return any;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "security";
    r.title = "Risky-pattern scanner";

    std::vector<std::string> notes;
    auto &calcs = ir.prog.calcs;

    // SEC-UPDATE-NOREAD: an O-spec UPDATE/DELETE with no CHAIN/READ* anywhere
    // in the program against the same file.
    std::set<std::string> read_files;
    for (auto &c : calcs)
        if (is_read_op(c.op) && !c.factor2.empty()) read_files.insert(upper_str(c.factor2));
    for (auto &orec : ir.prog.outputs) {
        if (orec.rec_op != rpgc::ORecOp::Update && orec.rec_op != rpgc::ORecOp::Delete) continue;
        if (read_files.count(upper_str(orec.file))) continue;
        Finding f;
        f.id = "SEC-UPDATE-NOREAD";
        f.severity = Severity::Warn;
        f.module = "security";
        f.message = std::string(orec.rec_op == rpgc::ORecOp::Update ? "UPDATE" : "DELETE") +
                    " on file '" + orec.file + "' has no preceding READ/CHAIN in the program";
        f.file = ir.path;
        f.line = orec.lineno;
        f.spec = 'O';
        r.findings.push_back(f);
        notes.push_back(f.message);
    }

    // SEC-NOEOFIND: CHAIN's "no-record" indicator is HI (cols 54-55); READ/
    // READE/READP's EOF/unequal/BOF indicator is EQ (cols 58-59, see cspec.h).
    for (auto &c : calcs) {
        if (c.op == rpgc::Op::CHAIN && c.hi.indicator == 0) {
            Finding f;
            f.id = "SEC-NOEOFIND";
            f.severity = Severity::Warn;
            f.module = "security";
            f.message = "CHAIN on '" + c.factor2 + "' has no no-record indicator (cols 54-55)";
            f.file = ir.path;
            f.line = c.lineno;
            f.spec = 'C';
            f.columns = "54-55";
            r.findings.push_back(f);
            notes.push_back(f.message);
        }
        if ((c.op == rpgc::Op::READ || c.op == rpgc::Op::READE || c.op == rpgc::Op::READP) &&
            c.eq.indicator == 0) {
            Finding f;
            f.id = "SEC-NOEOFIND";
            f.severity = Severity::Warn;
            f.module = "security";
            f.message = c.op_text + " on '" + c.factor2 + "' has no EOF/unequal indicator (cols 58-59)";
            f.file = ir.path;
            f.line = c.lineno;
            f.spec = 'C';
            f.columns = "58-59";
            r.findings.push_back(f);
            notes.push_back(f.message);
        }
    }

    // SEC-UNCHECKED-U: an external U1-U8 indicator is tested somewhere.
    for (auto &kv : ir.indicators) {
        if (kv.second.klass != IndicatorClass::External || kv.second.tests.empty()) continue;
        Finding f;
        f.id = "SEC-UNCHECKED-U";
        f.severity = Severity::Info;
        f.module = "security";
        f.message = "external indicator " + kv.second.label + " is tested; its source is not " +
                    "controlled by this program";
        f.file = ir.path;
        f.line = kv.second.tests.front().line;
        f.spec = 'C';
        r.findings.push_back(f);
        notes.push_back(f.message);
    }

    // SEC-UNVALIDATED: an input field flows into arithmetic without ever
    // being tested (COMP/IF/DOW/DOU/CAS/conditioning) anywhere first.
    std::set<std::string> validated;
    for (auto &c : calcs) {
        if (c.op == rpgc::Op::COMP || c.op == rpgc::Op::IF || c.op == rpgc::Op::DOW ||
            c.op == rpgc::Op::DOU || c.op == rpgc::Op::CAS) {
            if (!c.factor1.empty()) validated.insert(upper_str(c.factor1));
            if (!c.factor2.empty()) validated.insert(upper_str(c.factor2));
        }
    }
    std::set<std::string> flagged_unvalidated;
    for (auto &c : calcs) {
        if (c.op != rpgc::Op::ADD && c.op != rpgc::Op::SUB && c.op != rpgc::Op::MULT && c.op != rpgc::Op::DIV)
            continue;
        for (const std::string &tok : {c.factor1, c.factor2}) {
            if (tok.empty() || is_numeric_literal(tok) || tok[0] == '\'') continue;
            std::string name = upper_str(tok);
            const SymbolInfo *sym = ir.find_symbol(name);
            if (!sym || sym->kind != SymbolKind::Input) continue;
            if (validated.count(name)) continue;
            if (!flagged_unvalidated.insert(name).second) continue;
            Finding f;
            f.id = "SEC-UNVALIDATED";
            f.severity = Severity::Info;
            f.module = "security";
            f.message = "input field '" + name + "' flows into arithmetic without ever being range-checked";
            f.file = ir.path;
            f.line = c.lineno;
            f.spec = 'C';
            r.findings.push_back(f);
            notes.push_back(f.message);
        }
    }

    // SEC-DIVBYZERO: DIV with an unguarded variable (or literal-zero) divisor.
    int depth = 0;
    for (auto &c : calcs) {
        if (c.op == rpgc::Op::IF || c.op == rpgc::Op::DOW || c.op == rpgc::Op::DOU || c.op == rpgc::Op::DO)
            ++depth;
        else if (c.op == rpgc::Op::END) { if (depth > 0) --depth; }

        if (c.op != rpgc::Op::DIV) continue;
        std::string divisor = c.factor2.empty() ? c.factor1 : c.factor2;
        bool zero_literal = is_numeric_literal(divisor) && divisor.find_first_not_of("0.-") == std::string::npos;
        bool unguarded = c.conditions.empty() && depth == 0;
        if (zero_literal || (unguarded && !is_numeric_literal(divisor))) {
            Finding f;
            f.id = "SEC-DIVBYZERO";
            f.severity = Severity::Warn;
            f.module = "security";
            f.message = zero_literal ? "DIV by literal zero" : "DIV by '" + divisor + "' with no visible guard";
            f.file = ir.path;
            f.line = c.lineno;
            f.spec = 'C';
            r.findings.push_back(f);
            notes.push_back(f.message);
        }
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "security";
        sec.title = "Risky-pattern scanner";
        if (notes.empty()) sec.text_lines.push_back("(no risky patterns detected)");
        else sec.text_lines = notes;
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_security_module() {
    return ModuleInfo{"security", "Risky-pattern scanner", "SECURITY", run};
}

} // namespace analyze
