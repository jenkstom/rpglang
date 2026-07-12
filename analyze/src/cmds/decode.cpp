// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* decode -- column decoder (TOOLS_IDEAS.md §8.1). Pretty-prints each line
 * with every column field labeled per docs/SPEC_MAP.md. The first thing to
 * run on an unfamiliar line. */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <sstream>

namespace analyze {

namespace {

struct ColSpec { int first, last; const char *label; };

const std::vector<ColSpec> kHSpec = {
    {18, 18, "Currency symbol"}, {19, 19, "Date format"}, {20, 20, "Date edit"},
    {21, 21, "Inverted print"}, {26, 26, "Alt collating"}, {37, 37, "Inquiry allowed"},
    {41, 41, "1P forms position"}, {43, 43, "File translation"}, {45, 45, "Nonprint bypass"},
    {57, 57, "Transparent literal"}, {75, 80, "Program ID"},
};

const std::vector<ColSpec> kFSpec = {
    {7, 14, "File name"}, {15, 15, "Type (I/O/U/C)"}, {16, 16, "Designation"},
    {17, 17, "End required"}, {18, 18, "Sequence"}, {19, 19, "Format"},
    {24, 27, "Record length"}, {28, 28, "Mode"}, {29, 30, "Key length"},
    {31, 31, "Addr type"}, {32, 32, "Organization"}, {33, 34, "Overflow ind"},
    {35, 38, "Key start"}, {40, 46, "Device"}, {71, 72, "Cond indicator"},
};

const std::vector<ColSpec> kIRec = {
    {7, 14, "File name"}, {19, 20, "Record ind"},
    {21, 24, "Code1 pos"}, {25, 25, "Code1 not"}, {26, 26, "Code1 C/Z/D"}, {27, 27, "Code1 char"},
    {28, 31, "Code2 pos"}, {32, 32, "Code2 not"}, {33, 33, "Code2 C/Z/D"}, {34, 34, "Code2 char"},
    {35, 38, "Code3 pos"}, {39, 39, "Code3 not"}, {40, 40, "Code3 C/Z/D"}, {41, 41, "Code3 char"},
};

const std::vector<ColSpec> kIField = {
    {43, 43, "Data format (P/B)"}, {44, 47, "From"}, {48, 51, "To"}, {52, 52, "Decimals"},
    {53, 58, "Field name"}, {59, 60, "Control level"}, {61, 62, "Match field"},
    {63, 64, "Record-id rel"}, {65, 66, "Plus ind"}, {67, 68, "Minus ind"}, {69, 70, "Zero ind"},
};

const std::vector<ColSpec> kIDS = {
    {7, 12, "DS name"}, {18, 18, "LDA (U)"}, {19, 20, "'DS' marker"},
};

const std::vector<ColSpec> kCSpec = {
    {7, 8, "Control level"}, {9, 11, "Cond 1"}, {12, 14, "Cond 2"}, {15, 17, "Cond 3"},
    {18, 27, "Factor 1"}, {28, 32, "Operation"}, {33, 42, "Factor 2"}, {43, 48, "Result"},
    {49, 51, "Length"}, {52, 52, "Decimals"}, {53, 53, "Half-adjust"},
    {54, 55, "HI ind"}, {56, 57, "LO ind"}, {58, 59, "EQ ind"},
};

const std::vector<ColSpec> kORec = {
    {7, 14, "File name"}, {15, 15, "Type (H/D/T/E)"}, {16, 18, "Rec op / space"},
    {19, 20, "Skip before"}, {21, 22, "Skip after"},
    {23, 25, "Cond 1"}, {26, 28, "Cond 2"}, {29, 31, "Cond 3"}, {32, 37, "Except name"},
};

const std::vector<ColSpec> kOField = {
    {23, 25, "Cond 1"}, {26, 28, "Cond 2"}, {29, 31, "Cond 3"},
    {32, 37, "Field name"}, {38, 38, "Edit code"}, {39, 39, "Blank after"},
    {40, 43, "End position"}, {45, 70, "Constant / edit word"},
};

const std::vector<ColSpec> kESpec = {
    {11, 18, "From file"}, {27, 32, "Array/table name"}, {36, 39, "Entries"},
    {40, 42, "Entry length"}, {43, 43, "Data format"}, {44, 44, "Decimals"},
    {45, 45, "Ascending"}, {46, 51, "Alt name"}, {52, 54, "Alt entry length"},
    {55, 55, "Alt data format"}, {56, 56, "Alt decimals"}, {57, 57, "Alt ascending"},
};

void print_line(std::ostream &out, const rpgc::SourceLine &sl) {
    char ft = form_type(sl);
    out << "Line " << sl.lineno << " [" << ft << (sl.comment ? "*" : "") << "]: " << sl.text << "\n";
    if (sl.comment) { out << "  (comment)\n\n"; return; }

    const std::vector<ColSpec> *cols = nullptr;
    switch (ft) {
        case 'H': cols = &kHSpec; break;
        case 'F': cols = &kFSpec; break;
        case 'C': cols = &kCSpec; break;
        case 'O': cols = &kORec; break;   // record vs field disambiguated below
        case 'E': cols = &kESpec; break;
        case 'I': {
            std::string ri20 = upper_str(rpgc::col_trim(sl.text, 19, 20));
            std::string fromtxt = rpgc::col_trim(sl.text, 44, 47);
            std::string nametxt = rpgc::col_trim(sl.text, 53, 58);
            bool digits = !fromtxt.empty();
            for (char c : fromtxt) if (!std::isdigit((unsigned char)c)) { digits = false; break; }
            if (ri20 == "DS") cols = &kIDS;
            else if (digits && !nametxt.empty()) cols = &kIField;
            else cols = &kIRec;
            break;
        }
        default: cols = nullptr; break;
    }
    if (!cols) { out << "  (no column map for this form type)\n\n"; return; }

    // O-spec: a field line has cols 7-22 blank and 32-37 non-blank; use kOField then.
    if (ft == 'O') {
        std::string file = rpgc::col_trim(sl.text, 7, 14);
        std::string name = rpgc::col_trim(sl.text, 32, 37);
        if (file.empty() && !name.empty()) cols = &kOField;
    }

    for (auto &c : *cols) {
        std::string v = rpgc::col_trim(sl.text, c.first, c.last);
        if (v.empty()) continue;
        std::ostringstream range;
        range << c.first << "-" << c.last;
        out << "  " << pad_right(range.str(), 8) << pad_right(c.label, 22) << "= '" << v << "'\n";
    }
    out << "\n";
}

} // namespace

int cmd_decode(const std::vector<std::string> &files, const DecodeOptions &opts, std::ostream &out) {
    int worst = 0;
    for (auto &path : files) {
        std::vector<rpgc::SourceLine> src;
        if (!rpgc::load_source(path, src)) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        int lo = 1, hi = (int)src.size();
        if (opts.line > 0) { lo = hi = opts.line; }
        else if (!opts.range.empty()) {
            auto dash = opts.range.find('-');
            if (dash != std::string::npos) {
                try {
                    lo = std::stoi(opts.range.substr(0, dash));
                    hi = std::stoi(opts.range.substr(dash + 1));
                } catch (...) {}
            }
        }
        out << "=== " << path << " ===\n\n";
        for (auto &sl : src) {
            if (sl.lineno < lo || sl.lineno > hi) continue;
            print_line(out, sl);
        }
    }
    return worst;
}

} // namespace analyze
