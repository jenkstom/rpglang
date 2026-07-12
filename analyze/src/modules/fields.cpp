/* fields -- field definition catalog (TOOLS_IDEAS.md §4.4). */
#include "modules.h"
#include "../util.h"

#include <cctype>
#include <set>
#include <tuple>

namespace analyze {

namespace {

bool looks_like_temp_name(const std::string &name) {
    // Heuristic: 2-char name, letter + digit (W1, T9, X3, ...).
    if (name.size() != 2) return false;
    return std::isalpha((unsigned char)name[0]) && std::isdigit((unsigned char)name[1]);
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "fields";
    r.title = "Field definition catalog";

    std::vector<std::vector<std::string>> rows;
    Json arr = Json::array();

    for (auto &name : ir.symbol_order) {
        const SymbolInfo &s = ir.symbols.at(name);

        std::string type = s.decimals < 0 ? "char" : ("num(" + std::to_string(s.decimals) + ")");
        rows.push_back({s.name, symbol_kind_text(s.kind), type, std::to_string(s.length),
                        data_format_text(s.data_format), s.owner, std::to_string(s.def_line)});

        Json j = Json::object();
        j.set("name", s.name);
        j.set("kind", symbol_kind_text(s.kind));
        j.set("length", s.length);
        j.set("decimals", s.decimals);
        j.set("data_format", data_format_text(s.data_format));
        j.set("owner", s.owner);
        j.set("def_line", s.def_line);
        j.set("ref_count", (int)s.refs.size());
        arr.push_back(j);

        // FIELD-DUPNAME: two+ definition sites with differing attributes.
        if (s.defs.size() > 1) {
            std::set<std::tuple<int, int, int>> fingerprints;
            for (auto &d : s.defs) fingerprints.insert({(int)d.data_format, d.length, d.decimals});
            if (fingerprints.size() > 1) {
                Finding f;
                f.id = "FIELD-DUPNAME";
                f.severity = Severity::Warn;
                f.module = "fields";
                f.message = "field '" + s.name + "' is defined " + std::to_string(s.defs.size()) +
                            " times with differing attributes";
                f.file = ir.path;
                f.line = s.defs.front().line;
                f.spec = 'I';
                for (auto &d : s.defs) f.evidence.push_back({"fields", d.line});
                r.findings.push_back(f);
            }
        }

        if (looks_like_temp_name(s.name)) {
            Finding f;
            f.id = "FIELD-TEMPNAME";
            f.severity = Severity::Info;
            f.module = "fields";
            f.message = "field '" + s.name + "' looks like a temporary/scratch name";
            f.file = ir.path;
            f.line = s.def_line;
            f.spec = 'I';
            r.findings.push_back(f);
        }
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "fields";
        sec.title = "Field definition catalog";
        sec.text_lines = render_table({"NAME", "KIND", "TYPE", "LEN", "FORMAT", "OWNER", "LINE"}, rows);
        sec.data.set("fields", arr);
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_fields_module() {
    return ModuleInfo{"fields", "Field definition catalog", "FIELDS", run};
}

} // namespace analyze
