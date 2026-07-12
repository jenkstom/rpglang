// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* callgraph -- inter-program CALL/shared-file dependency graph (TOOLS_IDEAS.md §8.6).
 * This compiler has no CALL/CABL opcode (it's not in the Op enum -- an
 * attempt would surface as COMPAT-OP), so the only cross-program edges it
 * can detect are files two programs both declare in their F-specs. */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <map>
#include <set>

namespace analyze {

int cmd_callgraph(const std::vector<std::string> &files, bool dot, std::ostream &out) {
    std::map<std::string, std::set<std::string>> program_files; // program -> file names it uses
    std::map<std::string, std::set<std::string>> file_programs; // file name -> programs using it
    int worst = 0;

    for (auto &path : files) {
        ProgramIR ir = ProgramIR::build(path);
        if (ir.load_failed) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        for (auto &f : ir.prog.files) {
            program_files[ir.program_id].insert(upper_str(f.name));
            file_programs[upper_str(f.name)].insert(ir.program_id);
        }
    }

    if (dot) {
        out << "graph callgraph {\n";
        std::set<std::pair<std::string, std::string>> edges;
        for (auto &kv : file_programs) {
            auto &progs = kv.second;
            for (auto a = progs.begin(); a != progs.end(); ++a) {
                auto b = a;
                for (++b; b != progs.end(); ++b) {
                    auto key = *a < *b ? std::make_pair(*a, *b) : std::make_pair(*b, *a);
                    if (edges.insert(key).second)
                        out << "  \"" << key.first << "\" -- \"" << key.second << "\" [label=\"" << kv.first << "\"];\n";
                }
            }
        }
        out << "}\n";
        return worst;
    }

    for (auto &kv : program_files) {
        out << kv.first << ":\n";
        for (auto &fname : kv.second) {
            out << "  " << fname << "  (shared with:";
            bool any = false;
            for (auto &p : file_programs[fname]) {
                if (p == kv.first) continue;
                out << " " << p;
                any = true;
            }
            if (!any) out << " none";
            out << ")\n";
        }
    }

    return worst;
}

} // namespace analyze
