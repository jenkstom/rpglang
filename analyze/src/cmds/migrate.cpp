// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* migrate -- migration-difficulty scoring across many files (TOOLS_IDEAS.md §8.9).
 * Scores each program (unsupported opcodes, indicator logic, cycle
 * dependence, GOTO density) into tiers: trivial / moderate / rewrite. */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <set>

namespace analyze {

namespace {

bool is_known_opcode(const std::string &upperOp) {
    static const std::set<std::string> known = {
        "ADD", "Z-ADD", "ZADD", "Z-SUB", "ZSUB", "SETON", "SETOF", "COMP", "GOTO", "TAG",
        "MOVE", "MOVEL", "SUB", "MULT", "DIV", "MVR", "DO", "ELSE", "END", "EXSR", "BEGSR",
        "ENDSR", "EXCPT", "XFOOT", "SQRT", "LOKUP", "LOOKUP", "MOVEA", "TESTZ", "TESTB",
        "CHAIN", "SETLL", "READE", "READP", "READ", "BITON", "BITOF", "DEFN", "SORTA", "TIME",
        "MHHZO", "MHLZO", "MLHZO", "MLLZO",
    };
    if (known.count(upperOp)) return true;
    for (const char *prefix : {"IF", "DOW", "DOU", "CAS"}) {
        if (upperOp.rfind(prefix, 0) != 0) continue;
        std::string suffix = upperOp.substr(std::string(prefix).size());
        static const std::set<std::string> cmp = {"", "EQ", "NE", "GT", "LT", "GE", "LE"};
        if (cmp.count(suffix)) return true;
    }
    return false;
}

const char *tier_for(int score) {
    if (score < 5) return "trivial";
    if (score < 20) return "moderate";
    return "rewrite";
}

} // namespace

int cmd_migrate(const std::vector<std::string> &files, std::ostream &out) {
    struct Row { std::string id, path; int unsupported = 0, indicators = 0, gotos = 0; bool cycle_dep = false; int score = 0; };
    std::vector<Row> rows;
    int worst = 0;

    for (auto &path : files) {
        ProgramIR ir = ProgramIR::build(path);
        if (ir.load_failed) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        Row r;
        r.id = ir.program_id;
        r.path = path;
        for (auto &sl : ir.raw_lines) {
            if (sl.comment || form_type(sl) != 'C') continue;
            std::string op = upper_str(rpgc::col_trim(sl.text, 28, 32));
            if (!op.empty() && !is_known_opcode(op)) ++r.unsupported;
        }
        for (auto &f : ir.prog.files)
            if (f.device == rpgc::Device::Workstn || f.device == rpgc::Device::Special ||
                f.device == rpgc::Device::Console || f.design == rpgc::FileDesign::RecordAddr)
                ++r.unsupported;
        r.indicators = (int)ir.indicators.size();
        for (auto &c : ir.prog.calcs) if (c.op == rpgc::Op::GOTO) ++r.gotos;
        for (auto &f : ir.prog.files)
            if (f.design == rpgc::FileDesign::Primary || f.design == rpgc::FileDesign::Secondary)
                r.cycle_dep = true;

        r.score = r.unsupported * 10 + r.indicators / 2 + r.gotos * 2 + (r.cycle_dep ? 2 : 0);
        rows.push_back(r);
    }

    std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.score > b.score; });

    std::vector<std::vector<std::string>> table_rows;
    for (auto &r : rows)
        table_rows.push_back({r.id, std::to_string(r.unsupported), std::to_string(r.indicators),
                              std::to_string(r.gotos), r.cycle_dep ? "yes" : "no",
                              std::to_string(r.score), tier_for(r.score)});
    for (auto &line : render_table({"PROGRAM", "UNSUPP", "INDS", "GOTOS", "CYCLE", "SCORE", "TIER"}, table_rows))
        out << line << "\n";

    int trivial = 0, moderate = 0, rewrite = 0;
    for (auto &r : rows) {
        std::string t = tier_for(r.score);
        if (t == std::string("trivial")) ++trivial;
        else if (t == std::string("moderate")) ++moderate;
        else ++rewrite;
    }
    out << "\n" << trivial << " trivial, " << moderate << " moderate, " << rewrite << " rewrite\n";

    return worst;
}

} // namespace analyze
