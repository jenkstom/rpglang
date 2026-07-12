/* xref -- field cross-reference (TOOLS_IDEAS.md §4.5). */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "xref";
    r.title = "Field cross-reference";

    Section sec;
    sec.id = "xref";
    sec.title = "Field cross-reference";
    Json arr = Json::array();

    for (auto &name : ir.symbol_order) {
        const SymbolInfo &s = ir.symbols.at(name);

        if (!opts.quiet) {
            sec.text_lines.push_back(s.name + " (" + symbol_kind_text(s.kind) + "):");
            for (auto &ref : s.refs) {
                sec.text_lines.push_back(std::string("  ") + ref.spec + " " + std::to_string(ref.line) +
                                         "  " + ref.role + (ref.is_write ? "  [write]" : "  [read]"));
            }
        }

        Json refs = Json::array();
        bool has_read = false, has_result_write = false;
        for (auto &ref : s.refs) {
            Json j = Json::object();
            j.set("spec", std::string(1, ref.spec));
            j.set("line", ref.line);
            j.set("role", ref.role);
            j.set("write", ref.is_write);
            refs.push_back(j);
            if (!ref.is_write) has_read = true;
            if (ref.is_write && ref.role == "result") has_result_write = true;
        }
        Json j = Json::object();
        j.set("name", s.name);
        j.set("kind", symbol_kind_text(s.kind));
        j.set("refs", refs);
        arr.push_back(j);

        bool declared_kind = s.kind == SymbolKind::Input || s.kind == SymbolKind::LookAhead ||
                             s.kind == SymbolKind::DSSubfield || s.kind == SymbolKind::Array ||
                             s.kind == SymbolKind::Table;
        if (declared_kind && s.refs.size() <= 1) {
            Finding f;
            f.id = "DEAD-FIELD";
            f.severity = Severity::Info;
            f.module = "xref";
            f.message = "field '" + s.name + "' is defined but never referenced";
            f.file = ir.path;
            f.line = s.def_line;
            r.findings.push_back(f);
        }
        if (has_result_write && !has_read) {
            Finding f;
            f.id = "XREF-WRITEONLY";
            f.severity = Severity::Warn;
            f.module = "xref";
            f.message = "field '" + s.name + "' is computed but never read";
            f.file = ir.path;
            f.line = s.def_line;
            r.findings.push_back(f);
        }
    }

    sec.data.set("symbols", arr);
    if (!opts.quiet) r.sections.push_back(std::move(sec));

    return r;
}

} // namespace

ModuleInfo make_xref_module() {
    return ModuleInfo{"xref", "Field cross-reference", "CROSS-REFERENCE", run};
}

} // namespace analyze
