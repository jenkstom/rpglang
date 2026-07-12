/* docgen -- documentation generator (TOOLS_IDEAS.md §8.5).
 * Emits a Markdown reference page: comments, files, field table,
 * subroutines, indicator usage. */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>

namespace analyze {

namespace {

const char *doc_file_type(rpgc::FileType t) {
    switch (t) {
        case rpgc::FileType::Input:    return "Input";
        case rpgc::FileType::Output:   return "Output";
        case rpgc::FileType::Update:   return "Update";
        case rpgc::FileType::Combined: return "Combined";
    }
    return "?";
}

const char *doc_file_design(rpgc::FileDesign d) {
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

void emit_one(const ProgramIR &ir, std::ostream &out) {
    out << "# " << ir.program_id << "\n\n";
    out << "Source: `" << ir.path << "`\n\n";

    out << "## Files\n\n";
    out << "| Name | Type | Design | Device | Record length |\n";
    out << "|---|---|---|---|---|\n";
    for (auto &f : ir.prog.files) {
        out << "| " << f.name << " | " << doc_file_type(f.type) << " | " << doc_file_design(f.design)
            << " | " << f.device_text << " | " << f.reclen << " |\n";
    }
    out << "\n";

    out << "## Fields\n\n";
    out << "| Name | Kind | Length | Decimals | Format | Owner |\n";
    out << "|---|---|---|---|---|---|\n";
    for (auto &name : ir.symbol_order) {
        auto &s = ir.symbols.at(name);
        out << "| " << s.name << " | " << symbol_kind_text(s.kind) << " | " << s.length << " | "
            << s.decimals << " | " << data_format_text(s.data_format) << " | " << s.owner << " |\n";
    }
    out << "\n";

    out << "## Subroutines\n\n";
    if (ir.subroutines.empty()) out << "(none)\n\n";
    for (auto &sr : ir.subroutines) {
        out << "### " << sr.name << " (line " << sr.begin_line << "-" << sr.end_line << ")\n\n";
        if (!sr.calls.empty()) {
            out << "Calls: ";
            for (size_t i = 0; i < sr.calls.size(); ++i) out << (i ? ", " : "") << sr.calls[i];
            out << "\n\n";
        }
    }

    out << "## Indicators\n\n";
    out << "| Label | Class | Sets | Clears | Tests |\n";
    out << "|---|---|---|---|---|\n";
    for (auto &kv : ir.indicators) {
        auto &info = kv.second;
        out << "| " << info.label << " | " << indicator_class_text(info.klass) << " | "
            << info.sets.size() << " | " << info.clears.size() << " | " << info.tests.size() << " |\n";
    }
    out << "\n";

    out << "## Comments\n\n";
    for (auto &sl : ir.raw_lines) {
        if (sl.comment) out << "- line " << sl.lineno << ": `" << sl.text << "`\n";
    }
    out << "\n";
}

} // namespace

int cmd_docgen(const std::vector<std::string> &files, std::ostream &out) {
    int worst = 0;
    for (auto &path : files) {
        ProgramIR ir = ProgramIR::build(path);
        if (ir.load_failed) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        emit_one(ir, out);
    }
    return worst;
}

} // namespace analyze
