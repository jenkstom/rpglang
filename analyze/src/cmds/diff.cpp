/* diff -- semantic structural diff between two programs (TOOLS_IDEAS.md §8.3).
 * Compares field/opcode/indicator-level structure, ignoring whitespace and
 * sequence-number renumbering (it never looks at raw text, only the parsed
 * IR). */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>

namespace analyze {

namespace {

template <typename T>
void diff_sets(std::ostream &out, const char *label, const std::set<T> &a, const std::set<T> &b) {
    std::vector<T> removed, added;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(removed));
    std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(added));
    if (removed.empty() && added.empty()) return;
    out << label << ":\n";
    for (auto &r : removed) out << "  - " << r << "\n";
    for (auto &a2 : added) out << "  + " << a2 << "\n";
}

} // namespace

int cmd_diff(const std::vector<std::string> &files, std::ostream &out) {
    if (files.size() != 2) {
        out << "rpg-analyze: diff requires exactly two files\n";
        return 3;
    }
    ProgramIR a = ProgramIR::build(files[0]);
    ProgramIR b = ProgramIR::build(files[1]);
    if (a.load_failed || b.load_failed) {
        out << "rpg-analyze: cannot open one or both files\n";
        return 3;
    }

    out << "--- " << files[0] << "\n+++ " << files[1] << "\n\n";

    std::set<std::string> fields_a(a.symbol_order.begin(), a.symbol_order.end());
    std::set<std::string> fields_b(b.symbol_order.begin(), b.symbol_order.end());
    diff_sets(out, "Fields", fields_a, fields_b);

    for (auto &name : fields_a) {
        if (!fields_b.count(name)) continue;
        auto &sa = a.symbols.at(name);
        auto &sb = b.symbols.at(name);
        if (sa.kind != sb.kind || sa.length != sb.length || sa.decimals != sb.decimals ||
            sa.data_format != sb.data_format) {
            out << "Field " << name << " changed: "
                << symbol_kind_text(sa.kind) << "/" << sa.length << "/" << sa.decimals << "/"
                << data_format_text(sa.data_format) << "  ->  "
                << symbol_kind_text(sb.kind) << "/" << sb.length << "/" << sb.decimals << "/"
                << data_format_text(sb.data_format) << "\n";
        }
    }

    std::set<std::string> inds_a, inds_b;
    for (auto &kv : a.indicators) inds_a.insert(kv.first);
    for (auto &kv : b.indicators) inds_b.insert(kv.first);
    diff_sets(out, "Indicators used", inds_a, inds_b);

    std::set<std::string> subs_a, subs_b;
    for (auto &s : a.subroutines) subs_a.insert(s.name);
    for (auto &s : b.subroutines) subs_b.insert(s.name);
    diff_sets(out, "Subroutines", subs_a, subs_b);

    std::set<std::string> files_a, files_b;
    for (auto &f : a.prog.files) files_a.insert(f.name);
    for (auto &f : b.prog.files) files_b.insert(f.name);
    diff_sets(out, "Files", files_a, files_b);

    std::map<std::string, int> ops_a, ops_b;
    for (auto &c : a.prog.calcs) if (!c.op_text.empty()) ops_a[upper_str(c.op_text)]++;
    for (auto &c : b.prog.calcs) if (!c.op_text.empty()) ops_b[upper_str(c.op_text)]++;
    bool op_header = false;
    std::set<std::string> all_ops;
    for (auto &kv : ops_a) all_ops.insert(kv.first);
    for (auto &kv : ops_b) all_ops.insert(kv.first);
    for (auto &op : all_ops) {
        int ca = ops_a.count(op) ? ops_a[op] : 0;
        int cb = ops_b.count(op) ? ops_b[op] : 0;
        if (ca == cb) continue;
        if (!op_header) { out << "Opcode counts:\n"; op_header = true; }
        out << "  " << pad_right(op, 8) << ca << " -> " << cb << "\n";
    }

    out << "\nC-spec count: " << a.prog.calcs.size() << " -> " << b.prog.calcs.size() << "\n";

    return 0;
}

} // namespace analyze
