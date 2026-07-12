/* dataflow -- output-field data-flow traces (TOOLS_IDEAS.md §4.17).
 *
 * v1 traces only the most recent straight-line result-write for each output
 * field's source symbol; anything conditioned on GOTO/indicators beyond that
 * is left as "unresolved" rather than attempting full path-sensitive flow
 * (see TOOLS_IDEAS.md §12).
 */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "dataflow";
    r.title = "Output-field data-flow traces";

    Section sec;
    sec.id = "dataflow";
    sec.title = "Output-field data-flow traces";
    Json arr = Json::array();

    for (auto &orec : ir.prog.outputs) {
        for (auto &fld : orec.fields) {
            if (fld.is_const || fld.name.empty()) continue;
            const SymbolInfo *sym = ir.find_symbol(fld.name);
            std::string trace;
            bool uninit = false;
            if (!sym) {
                trace = "unresolved (unknown field)";
            } else {
                const RefSite *last_write = nullptr;
                for (auto &ref : sym->refs)
                    if (ref.is_write && ref.role == "result") last_write = &ref;
                if (last_write) {
                    trace = fld.name + " <- result of C-spec line " + std::to_string(last_write->line);
                } else if (sym->kind == SymbolKind::Input) {
                    trace = fld.name + " <- input field (file " + sym->owner + ")";
                } else if (sym->kind == SymbolKind::DSSubfield) {
                    trace = fld.name + " <- DS subfield (" + sym->owner + ")";
                } else if (sym->kind == SymbolKind::Array || sym->kind == SymbolKind::Table) {
                    trace = fld.name + " <- array/table element";
                } else {
                    trace = "unresolved (no assignment found)";
                    uninit = true;
                }
            }

            if (!opts.quiet)
                sec.text_lines.push_back(std::to_string(fld.lineno) + ": " + trace);

            Json j = Json::object();
            j.set("output_line", fld.lineno);
            j.set("field", fld.name);
            j.set("trace", trace);
            arr.push_back(j);

            if (uninit) {
                Finding f;
                f.id = "DATA-UNINIT";
                f.severity = Severity::Warn;
                f.module = "dataflow";
                f.message = "output field '" + fld.name + "' has no traceable source assignment";
                f.file = ir.path;
                f.line = fld.lineno;
                f.spec = 'O';
                r.findings.push_back(f);
            }

            // DATA-TRUNC: source width vs. this field's implied output width.
            if (sym && sym->length > 0 && fld.end_pos > 0) {
                int prev_end = 0;
                for (auto &other : orec.fields) {
                    if (&other == &fld) break;
                    if (other.end_pos > prev_end) prev_end = other.end_pos;
                }
                int width = fld.end_pos - prev_end;
                if (width > 0 && sym->length > width) {
                    Finding f;
                    f.id = "DATA-TRUNC";
                    f.severity = Severity::Info;
                    f.module = "dataflow";
                    f.message = "output field '" + fld.name + "' source is " + std::to_string(sym->length) +
                                " byte(s) wide but the output slot is only " + std::to_string(width) +
                                " byte(s); the value will be truncated";
                    f.file = ir.path;
                    f.line = fld.lineno;
                    f.spec = 'O';
                    r.findings.push_back(f);
                }
            }
        }
    }

    sec.data.set("traces", arr);
    if (!opts.quiet) r.sections.push_back(std::move(sec));

    return r;
}

} // namespace

ModuleInfo make_dataflow_module() {
    return ModuleInfo{"dataflow", "Output-field data-flow traces", "DATA FLOW", run};
}

} // namespace analyze
