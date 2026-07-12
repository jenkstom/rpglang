// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* search -- structured column-aware query across files (TOOLS_IDEAS.md §8.2).
 * Query DSL: space-separated `key:value` terms, ANDed together.
 *   op:COMP         C-spec opcode equals COMP
 *   ind:20          indicator 20 (set/cleared/tested) touches this line
 *   field:AMT       field AMT is referenced on this line
 *   spec:C          the line's form type is C
 *   cond:N90        C-spec conditioning includes (negated) indicator 90
 *   cols:30-50      columns 30-50 are non-blank
 */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <sstream>

namespace analyze {

namespace {

struct Term { std::string key, value; };

std::vector<Term> parse_query(const std::string &query) {
    std::vector<Term> terms;
    std::istringstream ss(query);
    std::string tok;
    while (ss >> tok) {
        auto colon = tok.find(':');
        if (colon == std::string::npos) continue;
        terms.push_back({upper_str(tok.substr(0, colon)), tok.substr(colon + 1)});
    }
    return terms;
}

const rpgc::CSpec *cspec_at(const ProgramIR &ir, int line) {
    for (auto &c : ir.prog.calcs) if (c.lineno == line) return &c;
    return nullptr;
}

bool line_matches(const ProgramIR &ir, const rpgc::SourceLine &sl, const std::vector<Term> &terms) {
    for (auto &t : terms) {
        if (t.key == "SPEC") {
            if (t.value.empty() || std::toupper((unsigned char)t.value[0]) != form_type(sl)) return false;
        } else if (t.key == "OP") {
            const rpgc::CSpec *c = cspec_at(ir, sl.lineno);
            if (!c || upper_str(c->op_text) != upper_str(t.value)) return false;
        } else if (t.key == "IND") {
            std::string label = upper_str(t.value);
            const IndicatorInfo *info = ir.find_indicator(label);
            if (!info) return false;
            bool touches = false;
            for (auto *v : {&info->sets, &info->clears, &info->tests})
                for (auto &s : *v) if (s.line == sl.lineno) touches = true;
            if (!touches) return false;
        } else if (t.key == "FIELD") {
            const SymbolInfo *sym = ir.find_symbol(t.value);
            if (!sym) return false;
            bool touches = false;
            for (auto &r : sym->refs) if (r.line == sl.lineno) touches = true;
            if (!touches) return false;
        } else if (t.key == "COND") {
            const rpgc::CSpec *c = cspec_at(ir, sl.lineno);
            if (!c) return false;
            bool neg = !t.value.empty() && (t.value[0] == 'N' || t.value[0] == 'n');
            std::string tok = neg ? t.value.substr(1) : t.value;
            int idx = rpgc::parse_indicator_token(tok);
            bool found = false;
            for (auto &cond : c->conditions)
                if (cond.indicator == idx && cond.negate == neg) found = true;
            if (!found) return false;
        } else if (t.key == "COLS") {
            auto dash = t.value.find('-');
            if (dash == std::string::npos) return false;
            try {
                int a = std::stoi(t.value.substr(0, dash));
                int b = std::stoi(t.value.substr(dash + 1));
                if (rpgc::col_trim(sl.text, a, b).empty()) return false;
            } catch (...) { return false; }
        }
    }
    return true;
}

} // namespace

int cmd_search(const std::vector<std::string> &files, const std::string &query, std::ostream &out) {
    auto terms = parse_query(query);
    int worst = 0;
    int matches = 0;
    for (auto &path : files) {
        ProgramIR ir = ProgramIR::build(path);
        if (ir.load_failed) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        for (auto &sl : ir.raw_lines) {
            if (!line_matches(ir, sl, terms)) continue;
            out << path << ":" << sl.lineno << ": " << sl.text << "\n";
            ++matches;
        }
    }
    out << matches << " match(es)\n";
    return worst;
}

} // namespace analyze
