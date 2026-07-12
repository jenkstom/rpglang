// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* portfolio -- codebase-wide metrics dashboard across many files (TOOLS_IDEAS.md §8.8). */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <map>

namespace analyze {

namespace {

struct FileStats {
    std::string path, id;
    int loc = 0, cspecs = 0, fields = 0, indicators = 0, subroutines = 0, complexity = 0;
};

int complexity_score(const ProgramIR &ir) {
    int score = 1;
    for (auto &c : ir.prog.calcs)
        if (c.op == rpgc::Op::IF || c.op == rpgc::Op::DOW || c.op == rpgc::Op::DOU || c.op == rpgc::Op::CAS)
            ++score;
    return score;
}

} // namespace

int cmd_portfolio(const std::vector<std::string> &files, bool html, std::ostream &out) {
    std::vector<FileStats> stats;
    std::map<std::string, int> opcode_totals;
    std::map<std::string, int> file_ref_counts;
    int worst = 0;

    for (auto &path : files) {
        ProgramIR ir = ProgramIR::build(path);
        if (ir.load_failed) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        FileStats s;
        s.path = path;
        s.id = ir.program_id;
        s.loc = (int)ir.raw_lines.size();
        s.cspecs = (int)ir.prog.calcs.size();
        s.fields = (int)ir.symbol_order.size();
        s.indicators = (int)ir.indicators.size();
        s.subroutines = (int)ir.subroutines.size();
        s.complexity = complexity_score(ir);
        stats.push_back(s);

        for (auto &c : ir.prog.calcs)
            if (!c.op_text.empty()) opcode_totals[upper_str(c.op_text)]++;
        for (auto &f : ir.prog.files) file_ref_counts[upper_str(f.name)]++;
    }

    std::sort(stats.begin(), stats.end(), [](auto &a, auto &b) { return a.complexity > b.complexity; });

    if (html) {
        out << "<!doctype html><html><head><meta charset=\"utf-8\"><title>rpg-analyze portfolio</title>"
            << "<style>table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:4px 8px}</style>"
            << "</head><body><h1>Portfolio</h1><table><tr><th>Program</th><th>File</th><th>LOC</th>"
            << "<th>C-specs</th><th>Fields</th><th>Indicators</th><th>Subroutines</th><th>Complexity</th></tr>\n";
        for (auto &s : stats) {
            out << "<tr><td>" << s.id << "</td><td>" << s.path << "</td><td>" << s.loc << "</td><td>"
                << s.cspecs << "</td><td>" << s.fields << "</td><td>" << s.indicators << "</td><td>"
                << s.subroutines << "</td><td>" << s.complexity << "</td></tr>\n";
        }
        out << "</table></body></html>\n";
        return worst;
    }

    for (auto &line : render_table({"PROGRAM", "LOC", "CSPECS", "FIELDS", "INDS", "SUBRS", "CPLX"}, [&] {
             std::vector<std::vector<std::string>> rows;
             for (auto &s : stats)
                 rows.push_back({s.id, std::to_string(s.loc), std::to_string(s.cspecs),
                                std::to_string(s.fields), std::to_string(s.indicators),
                                std::to_string(s.subroutines), std::to_string(s.complexity)});
             return rows;
         }()))
        out << line << "\n";

    out << "\nMost-referenced files:\n";
    std::vector<std::pair<std::string, int>> fr(file_ref_counts.begin(), file_ref_counts.end());
    std::sort(fr.begin(), fr.end(), [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &kv : fr) out << "  " << pad_right(kv.first, 12) << kv.second << "\n";

    out << "\nOpcode frequency across portfolio:\n";
    std::vector<std::pair<std::string, int>> ov(opcode_totals.begin(), opcode_totals.end());
    std::sort(ov.begin(), ov.end(), [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &kv : ov) out << "  " << pad_right(kv.first, 12) << kv.second << "\n";

    return worst;
}

} // namespace analyze
