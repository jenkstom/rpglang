// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

#include "ir.h"

#include "cspec.h"
#include "fspec.h"
#include "hspec.h"
#include "ispec.h"
#include "ospec.h"
#include "espec.h"
#include "uspec.h"
#include "autoreport.h"

#include <algorithm>
#include <cctype>

namespace analyze {

using namespace rpgc;

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

bool is_literal_token(const std::string &tok) {
    if (tok.empty()) return true;
    if (tok[0] == '\'') return true;   // quoted character literal
    if (tok[0] == '*') return true;    // reserved word (*LIKE, *IN, ...)
    size_t i = 0;
    if (tok[i] == '-') ++i;
    bool any_digit = false;
    for (; i < tok.size(); ++i) {
        if (std::isdigit((unsigned char)tok[i])) { any_digit = true; continue; }
        if (tok[i] == '.') continue;
        return false;
    }
    return any_digit;
}

/* Split "ARR,IDX" into base/index; returns false if no comma. */
bool split_array_ref(const std::string &tok, std::string &base, std::string &idx) {
    auto pos = tok.find(',');
    if (pos == std::string::npos) return false;
    base = tok.substr(0, pos);
    idx = tok.substr(pos + 1);
    return true;
}

} // namespace

const char *symbol_kind_text(SymbolKind k) {
    switch (k) {
        case SymbolKind::Input:      return "input";
        case SymbolKind::LookAhead:  return "lookahead";
        case SymbolKind::DSSubfield: return "ds-subfield";
        case SymbolKind::Array:      return "array";
        case SymbolKind::Table:      return "table";
        case SymbolKind::Inline:     return "inline";
        case SymbolKind::Unknown:    return "unknown";
    }
    return "unknown";
}

const char *data_format_text(DataFormat d) {
    switch (d) {
        case DataFormat::Zoned:   return "zoned";
        case DataFormat::Packed:  return "packed";
        case DataFormat::Binary:  return "binary";
        case DataFormat::Alpha:   return "alpha";
        case DataFormat::Unknown: return "unknown";
    }
    return "unknown";
}

const char *indicator_class_text(IndicatorClass c) {
    switch (c) {
        case IndicatorClass::General:      return "general";
        case IndicatorClass::ControlLevel: return "control-level";
        case IndicatorClass::LastRecord:   return "last-record";
        case IndicatorClass::FirstPage:    return "first-page";
        case IndicatorClass::Matching:     return "matching";
        case IndicatorClass::Overflow:     return "overflow";
        case IndicatorClass::External:     return "external";
        case IndicatorClass::Halt:         return "halt";
        case IndicatorClass::FunctionKey:  return "function-key";
    }
    return "general";
}

std::string indicator_label(int encoded) {
    if (encoded == 0) return "";
    if (encoded >= 1 && encoded <= 99) {
        char buf[4];
        std::snprintf(buf, sizeof buf, "%02d", encoded);
        return buf;
    }
    if (encoded == -1) return "LR";
    if (encoded >= -10 && encoded <= -2) {
        int n = -(encoded + 1);
        return "L" + std::to_string(n);
    }
    if (encoded == -11) return "1P";
    if (encoded == -12) return "MR";
    if (encoded >= -19 && encoded <= -13) {
        int k = -13 - encoded;
        return std::string("O") + char('A' + k);
    }
    if (encoded == -20) return "OV";
    if (encoded >= -28 && encoded <= -21) {
        int k = -21 - encoded;
        return "U" + std::to_string(k + 1);
    }
    if (encoded >= -37 && encoded <= -29) {
        int k = -29 - encoded;
        return "H" + std::to_string(k + 1);
    }
    if (encoded >= -62 && encoded <= -38) {
        int k = -38 - encoded;
        return std::string("K") + char('A' + k);
    }
    return "";
}

IndicatorClass classify_indicator(const std::string &label) {
    if (label.size() == 2 && std::isdigit((unsigned char)label[0]) &&
        std::isdigit((unsigned char)label[1]))
        return IndicatorClass::General;
    if (label == "LR") return IndicatorClass::LastRecord;
    if (label == "1P") return IndicatorClass::FirstPage;
    if (label == "MR") return IndicatorClass::Matching;
    if (!label.empty() && label[0] == 'L') return IndicatorClass::ControlLevel;
    if (label == "OV") return IndicatorClass::Overflow;
    if (!label.empty() && label[0] == 'O') return IndicatorClass::Overflow;
    if (!label.empty() && label[0] == 'U') return IndicatorClass::External;
    if (!label.empty() && label[0] == 'H') return IndicatorClass::Halt;
    if (!label.empty() && label[0] == 'K') return IndicatorClass::FunctionKey;
    return IndicatorClass::General;
}

const SymbolInfo *ProgramIR::find_symbol(const std::string &name) const {
    auto it = symbols.find(upper(name));
    return it != symbols.end() ? &it->second : nullptr;
}

const IndicatorInfo *ProgramIR::find_indicator(const std::string &label) const {
    auto it = indicators.find(label);
    return it != indicators.end() ? &it->second : nullptr;
}

char ProgramIR::spec_type_at(int line) const {
    if (line <= 0 || (size_t)line > raw_lines.size()) return ' ';
    // raw_lines is not guaranteed indexed by lineno-1 after /COPY splicing, so
    // scan (source files are small; this is only used for display/lookup).
    for (auto &sl : raw_lines) {
        if (sl.lineno == line) return form_type(sl);
    }
    return ' ';
}

std::string ProgramIR::line_text(int line) const {
    for (auto &sl : raw_lines) {
        if (sl.lineno == line) return sl.text;
    }
    return "";
}

namespace {

/* ---- symbol table -------------------------------------------------------- */

SymbolInfo &touch(ProgramIR &ir, const std::string &rawName, SymbolKind kind_if_new) {
    std::string name = upper(rawName);
    auto it = ir.symbols.find(name);
    if (it == ir.symbols.end()) {
        SymbolInfo info;
        info.name = name;
        info.kind = kind_if_new;
        ir.symbols.emplace(name, info);
        ir.symbol_order.push_back(name);
        it = ir.symbols.find(name);
    }
    return it->second;
}

void note_ref(SymbolInfo &s, RefSite site) {
    if (s.def_line == 0) s.def_line = site.line;
    s.refs.push_back(std::move(site));
}

void add_ref(ProgramIR &ir, const std::string &rawName, RefSite site,
             SymbolKind kind_if_new = SymbolKind::Inline) {
    if (is_literal_token(rawName)) return;
    std::string base, idx;
    if (split_array_ref(rawName, base, idx)) {
        if (!base.empty()) note_ref(touch(ir, base, kind_if_new), site);
        if (!idx.empty() && !is_literal_token(idx)) note_ref(touch(ir, idx, kind_if_new), site);
        return;
    }
    note_ref(touch(ir, rawName, kind_if_new), site);
}

void build_symbol_table(ProgramIR &ir) {
    for (auto &f : ir.prog.in_fields) {
        if (f.name.empty()) continue;
        auto &s = touch(ir, f.name, SymbolKind::Input);
        s.kind = SymbolKind::Input;
        s.data_format = f.decimals < 0 && f.data_format == 0 ? DataFormat::Alpha
                       : f.data_format == 'P' ? DataFormat::Packed
                       : f.data_format == 'B' ? DataFormat::Binary
                       : DataFormat::Zoned;
        s.length = f.to - f.from + 1;
        s.decimals = f.decimals;
        s.owner = f.file;
        s.def_line = f.lineno;
        s.refs.push_back({'I', f.lineno, "field", true});
        s.defs.push_back({f.lineno, s.data_format, s.length, s.decimals, s.owner});
    }
    for (auto &f : ir.prog.lookahead_fields) {
        if (f.name.empty()) continue;
        auto &s = touch(ir, f.name, SymbolKind::LookAhead);
        s.kind = SymbolKind::LookAhead;
        s.length = f.to - f.from + 1;
        s.decimals = f.decimals;
        s.owner = f.file;
        s.def_line = f.lineno;
        s.refs.push_back({'I', f.lineno, "lookahead-field", true});
        s.defs.push_back({f.lineno, s.data_format, s.length, s.decimals, s.owner});
    }
    for (auto &sub : ir.prog.ds_subfields) {
        if (sub.name.empty()) continue;
        std::string owner;
        if (sub.ds_index >= 0 && (size_t)sub.ds_index < ir.prog.data_structures.size()) {
            owner = ir.prog.data_structures[sub.ds_index].name;
            if (owner.empty()) owner = "DS#" + std::to_string(sub.ds_index);
        }
        auto &s = touch(ir, sub.name, SymbolKind::DSSubfield);
        s.kind = SymbolKind::DSSubfield;
        s.data_format = sub.decimals < 0 ? DataFormat::Alpha : DataFormat::Zoned;
        s.length = sub.to - sub.from + 1;
        s.decimals = sub.decimals;
        s.owner = owner;
        s.def_line = sub.lineno;
        s.refs.push_back({'I', sub.lineno, "ds-subfield", true});
        s.defs.push_back({sub.lineno, s.data_format, s.length, s.decimals, s.owner});
    }
    for (auto &a : ir.prog.arrays) {
        if (a.name.empty()) continue;
        bool is_tab = is_table_name(a.name);
        auto &s = touch(ir, a.name, is_tab ? SymbolKind::Table : SymbolKind::Array);
        s.kind = is_tab ? SymbolKind::Table : SymbolKind::Array;
        s.data_format = a.decimals < 0 ? DataFormat::Alpha
                       : a.data_format == 'P' ? DataFormat::Packed
                       : a.data_format == 'B' ? DataFormat::Binary
                       : DataFormat::Zoned;
        s.length = a.entry_len;
        s.decimals = a.decimals;
        s.def_line = a.lineno;
        s.refs.push_back({'E', a.lineno, "array-def", true});
        s.defs.push_back({a.lineno, s.data_format, s.length, s.decimals, s.owner});
        if (!a.alt_name.empty()) {
            bool alt_tab = is_table_name(a.alt_name);
            auto &alt = touch(ir, a.alt_name, alt_tab ? SymbolKind::Table : SymbolKind::Array);
            alt.kind = alt_tab ? SymbolKind::Table : SymbolKind::Array;
            alt.data_format = a.alt_decimals < 0 ? DataFormat::Alpha
                             : a.alt_data_format == 'P' ? DataFormat::Packed
                             : a.alt_data_format == 'B' ? DataFormat::Binary
                             : DataFormat::Zoned;
            alt.length = a.alt_entry_len;
            alt.decimals = a.alt_decimals;
            alt.def_line = a.lineno;
            alt.refs.push_back({'E', a.lineno, "array-def(alt)", true});
            alt.defs.push_back({a.lineno, alt.data_format, alt.length, alt.decimals, alt.owner});
        }
    }

    // C-spec operand scan (see ir.h comment / TOOLS_IDEAS.md §4.4).
    for (auto &c : ir.prog.calcs) {
        if (c.op == Op::SETON || c.op == Op::SETOF) continue; // pure indicator ops

        bool f1_is_field = !(c.op == Op::GOTO || c.op == Op::TAG || c.op == Op::BEGSR ||
                              c.op == Op::DEFN);
        bool f2_is_field = !(c.op == Op::GOTO || c.op == Op::EXSR || c.op == Op::TAG ||
                              c.op == Op::EXCPT || c.op == Op::CHAIN || c.op == Op::SETLL ||
                              c.op == Op::READE || c.op == Op::READ || c.op == Op::READP);
        bool result_is_field = !(c.op == Op::GOTO || c.op == Op::TAG || c.op == Op::BEGSR ||
                                  c.op == Op::ENDSR || c.op == Op::EXSR || c.op == Op::EXCPT ||
                                  c.op == Op::SETLL || c.op == Op::READE || c.op == Op::READ ||
                                  c.op == Op::READP || c.op == Op::SORTA || c.op == Op::CAS);
        bool result_is_write = result_is_field && c.op != Op::TESTZ && c.op != Op::TESTB;

        if (f1_is_field && !c.factor1.empty())
            add_ref(ir, c.factor1, {'C', c.lineno, "factor1", false});
        if (f2_is_field && !c.factor2.empty())
            add_ref(ir, c.factor2, {'C', c.lineno, "factor2", false});
        if (result_is_field && !c.result.empty())
            add_ref(ir, c.result, {'C', c.lineno, "result", result_is_write});
    }

    // O-spec output fields (read-only references).
    for (auto &orec : ir.prog.outputs) {
        for (auto &fld : orec.fields) {
            if (fld.is_const || fld.name.empty()) continue;
            add_ref(ir, fld.name, {'O', fld.lineno, "output", false});
        }
    }
}

/* ---- indicator table ------------------------------------------------------ */

enum class IndAction { Set, Clear, Test };

void add_ind(ProgramIR &ir, int encoded, RefSite site, IndAction action) {
    std::string label = indicator_label(encoded);
    if (label.empty()) return;
    auto &info = ir.indicators[label];
    info.label = label;
    info.klass = classify_indicator(label);
    switch (action) {
        case IndAction::Set:   info.sets.push_back(site);   break;
        case IndAction::Clear: info.clears.push_back(site); break;
        case IndAction::Test:  info.tests.push_back(site);  break;
    }
}

void build_indicator_table(ProgramIR &ir) {
    for (auto &c : ir.prog.calcs) {
        for (auto &cond : c.conditions) {
            RefSite site{'C', c.lineno, "condition", false, cond.negate};
            add_ind(ir, cond.indicator, site, IndAction::Test);
        }
        if (!c.control_level.empty() && c.control_level != "AN" && c.control_level != "OR") {
            int idx = parse_indicator_token(c.control_level);
            if (idx != 0)
                add_ind(ir, idx, {'C', c.lineno, "control-level", false}, IndAction::Test);
        }
        IndAction resAction = (c.op == Op::SETOF) ? IndAction::Clear : IndAction::Set;
        if (c.hi.indicator) add_ind(ir, c.hi.indicator, {'C', c.lineno, "HI", true}, resAction);
        if (c.lo.indicator) add_ind(ir, c.lo.indicator, {'C', c.lineno, "LO", true}, resAction);
        if (c.eq.indicator) add_ind(ir, c.eq.indicator, {'C', c.lineno, "EQ", true}, resAction);
    }
    for (auto &f : ir.prog.files) {
        if (f.has_overflow)
            add_ind(ir, f.overflow_ind, {'F', f.lineno, "overflow", true}, IndAction::Set);
        if (f.has_cond)
            add_ind(ir, f.cond_ind, {'F', f.lineno, "file-condition", false}, IndAction::Test);
    }
    for (auto &rec : ir.prog.in_records) {
        if (rec.rec_indicator != 0)
            add_ind(ir, rec.rec_indicator, {'I', rec.lineno, "record-id", true}, IndAction::Set);
    }
    for (auto &fld : ir.prog.in_fields) {
        if (!fld.control_level.empty()) {
            int idx = parse_indicator_token(fld.control_level);
            if (idx != 0) add_ind(ir, idx, {'I', fld.lineno, "control-level-field", true}, IndAction::Set);
        }
        if (fld.plus_ind)  add_ind(ir, fld.plus_ind,  {'I', fld.lineno, "on-plus", true}, IndAction::Set);
        if (fld.minus_ind) add_ind(ir, fld.minus_ind, {'I', fld.lineno, "on-minus", true}, IndAction::Set);
        if (fld.zero_ind)  add_ind(ir, fld.zero_ind,  {'I', fld.lineno, "on-zero", true}, IndAction::Set);
        if (fld.record_id) add_ind(ir, fld.record_id, {'I', fld.lineno, "record-relation", false}, IndAction::Test);
    }
    for (auto &orec : ir.prog.outputs) {
        for (auto &group : orec.conditions)
            for (auto &cond : group)
                add_ind(ir, cond.indicator, {'O', orec.lineno, "line-condition", false, cond.negate}, IndAction::Test);
        for (auto &fld : orec.fields)
            for (auto &cond : fld.conditions)
                add_ind(ir, cond.indicator, {'O', fld.lineno, "field-condition", false, cond.negate}, IndAction::Test);
    }
}

/* ---- subroutine table ------------------------------------------------------ */

void build_subroutines(ProgramIR &ir) {
    auto &calcs = ir.prog.calcs;
    for (size_t i = 0; i < calcs.size(); ++i) {
        if (calcs[i].op != Op::BEGSR) continue;
        Subroutine sr;
        sr.name = upper(calcs[i].factor1);
        sr.begin_line = calcs[i].lineno;
        sr.begin_idx = (int)i;
        size_t j;
        for (j = i + 1; j < calcs.size(); ++j)
            if (calcs[j].op == Op::ENDSR) break;
        if (j < calcs.size()) { sr.end_line = calcs[j].lineno; sr.end_idx = (int)j; }
        else { sr.end_idx = (int)calcs.size(); }

        for (size_t k = i + 1; k < (size_t)sr.end_idx && k < calcs.size(); ++k) {
            auto &c = calcs[k];
            if (c.op == Op::EXSR && !c.factor2.empty()) sr.calls.push_back(upper(c.factor2));
            else if (c.op == Op::CAS && !c.result.empty()) sr.calls.push_back(upper(c.result));

            for (auto &cond : c.conditions) {
                std::string lbl = indicator_label(cond.indicator);
                if (!lbl.empty()) sr.indicators_tested.push_back(lbl);
            }
            auto note_set = [&](int idx) {
                if (!idx) return;
                std::string lbl = indicator_label(idx);
                if (!lbl.empty()) sr.indicators_set.push_back(lbl);
            };
            note_set(c.hi.indicator);
            note_set(c.lo.indicator);
            note_set(c.eq.indicator);

            bool result_is_field = !(c.op == Op::GOTO || c.op == Op::TAG || c.op == Op::BEGSR ||
                                      c.op == Op::ENDSR || c.op == Op::EXSR || c.op == Op::EXCPT ||
                                      c.op == Op::SETLL || c.op == Op::READE || c.op == Op::READ ||
                                      c.op == Op::READP || c.op == Op::SORTA || c.op == Op::CAS ||
                                      c.op == Op::TESTZ || c.op == Op::TESTB);
            if (result_is_field && !c.result.empty() && !is_literal_token(c.result))
                sr.fields_written.push_back(upper(c.result));
        }

        ir.subroutine_index[sr.name] = (int)ir.subroutines.size();
        ir.subroutines.push_back(std::move(sr));
        i = j;
    }
}

/* ---- control-flow graph ------------------------------------------------------ */

void build_cfg(ProgramIR &ir) {
    auto &calcs = ir.prog.calcs;
    size_t n = calcs.size();
    ControlFlowGraph g;
    g.succ.assign(n, {});
    g.reachable.assign(n, false);

    for (size_t i = 0; i < n; ++i)
        if (calcs[i].op == Op::TAG && !calcs[i].factor1.empty())
            g.tag_index[upper(calcs[i].factor1)] = (int)i;

    for (size_t i = 0; i < n; ++i) {
        auto &c = calcs[i];
        bool blocks_fallthrough = false;

        if (c.op == Op::GOTO) {
            if (!c.factor2.empty()) {
                auto it = g.tag_index.find(upper(c.factor2));
                if (it != g.tag_index.end()) g.succ[i].push_back({it->second, true});
            }
            if (c.conditions.empty()) blocks_fallthrough = true;
        }
        if (c.op == Op::ENDSR) blocks_fallthrough = true;

        bool next_is_begsr = (i + 1 < n) && calcs[i + 1].op == Op::BEGSR;
        if (!blocks_fallthrough && !next_is_begsr && i + 1 < n)
            g.succ[i].push_back({(int)(i + 1), false});

        std::string target;
        if (c.op == Op::EXSR && !c.factor2.empty()) target = upper(c.factor2);
        else if (c.op == Op::CAS && !c.result.empty()) target = upper(c.result);
        if (!target.empty()) {
            auto sit = ir.subroutine_index.find(target);
            if (sit != ir.subroutine_index.end()) {
                int begin_idx = ir.subroutines[sit->second].begin_idx;
                if (begin_idx >= 0 && (size_t)(begin_idx + 1) < n)
                    g.succ[i].push_back({begin_idx + 1, false});
                else if (begin_idx >= 0 && (size_t)begin_idx < n)
                    g.succ[i].push_back({begin_idx, false});
            }
        }
    }

    if (n > 0) {
        std::vector<int> stack{0};
        g.reachable[0] = true;
        while (!stack.empty()) {
            int u = stack.back();
            stack.pop_back();
            for (auto &e : g.succ[u]) {
                if (!g.reachable[e.to]) {
                    g.reachable[e.to] = true;
                    stack.push_back(e.to);
                }
            }
        }
    }

    ir.cfg = std::move(g);
}

} // namespace

ProgramIR ProgramIR::build(const std::string &path) {
    ProgramIR ir;
    ir.path = path;

    if (!load_source(path, ir.raw_lines)) {
        ir.load_failed = true;
        return ir;
    }

    auto slash = path.find_last_of('/');
    std::string base_dir = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);

    reset_diagnostics();
    set_diagnostic_sink([&ir](const std::string &file, int line, int col, DiagKind kind,
                              const std::string &message) {
        ir.diagnostics.push_back({file, line, col, kind, message});
    });

    expand_copy_statements(ir.raw_lines, base_dir);
    {
        // D3: Auto Report preprocessing (manual Ch. 26). Mirrors the compiler:
        // rewrites the source lines in place before any spec parser runs. The
        // boolean is discarded -- any hard error is captured via the diagnostic
        // sink into ir.diagnostics, exactly like expand_copy_statements above.
        AutoReportReport ar;
        expand_autoreport(ir.raw_lines, base_dir, ar);
    }

    ir.prog.hspec           = parse_hspec(ir.raw_lines);
    ir.prog.files            = parse_fspecs(ir.raw_lines);
    ir.prog.line_counters    = parse_lspecs(ir.raw_lines);
    ISpecs is                = parse_ispecs(ir.raw_lines);
    ir.prog.in_records       = std::move(is.records);
    ir.prog.in_fields        = std::move(is.fields);
    ir.prog.lookahead_fields = std::move(is.lookahead_fields);
    ir.prog.data_structures  = std::move(is.data_structures);
    ir.prog.ds_subfields     = std::move(is.ds_subfields);
    ir.prog.calcs            = parse_cspecs(ir.raw_lines);
    ir.prog.outputs          = parse_ospecs(ir.raw_lines);
    ir.prog.arrays           = parse_especs(ir.raw_lines);
    load_compile_time_data(ir.raw_lines, ir.prog.arrays);

    set_diagnostic_sink({});

    ir.program_id = ir.prog.hspec.program_id;
    if (ir.program_id.empty()) {
        std::string base = path;
        auto sl = base.find_last_of('/');
        if (sl != std::string::npos) base = base.substr(sl + 1);
        auto dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        ir.program_id = upper(base);
    }

    build_symbol_table(ir);
    build_indicator_table(ir);
    build_subroutines(ir);
    build_cfg(ir);

    return ir;
}

} // namespace analyze
