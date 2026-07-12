// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* recordmap -- record layout reconstruction (TOOLS_IDEAS.md §4.3).
 *
 * In this compiler's I-spec encoding, From/To (cols 44-51) are byte offsets
 * within the physical record for every data format -- zoned, packed, and
 * binary alike (see runtime/rpg_runtime.c: rpg_rt_get_packed/get_binary index
 * `from..to` directly as record bytes, not digit positions). So byte width is
 * simply `to - from + 1` regardless of format; no extra decoding is needed.
 */
#include "modules.h"
#include "../util.h"

#include <algorithm>
#include <unordered_map>

namespace analyze {

namespace {

std::string fmt_text(const rpgc::ISpecField &f) {
    if (f.decimals < 0) return "alpha";
    if (f.data_format == 'P') return "packed";
    if (f.data_format == 'B') return "binary";
    return "zoned";
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "recordmap";
    r.title = "Record layout reconstruction";

    std::vector<std::string> file_order;
    std::unordered_map<std::string, std::vector<const rpgc::ISpecField *>> by_file;
    for (auto &f : ir.prog.in_fields) {
        if (by_file.find(f.file) == by_file.end()) file_order.push_back(f.file);
        by_file[f.file].push_back(&f);
    }

    Section sec;
    sec.id = "recordmap";
    sec.title = "Record layout reconstruction";
    Json files_json = Json::array();

    for (auto &fname : file_order) {
        auto sorted = by_file[fname];
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const rpgc::ISpecField *a, const rpgc::ISpecField *b) { return a->from < b->from; });

        int reclen = 0;
        for (auto &fs : ir.prog.files) {
            if (fs.name == fname) { reclen = fs.reclen; break; }
        }

        if (!opts.quiet) {
            sec.text_lines.push_back("File " + fname + " (record length " + std::to_string(reclen) + "):");
            std::vector<std::vector<std::string>> rows;
            for (auto *f : sorted) {
                rows.push_back({f->name, std::to_string(f->from), std::to_string(f->to),
                                std::to_string(f->to - f->from + 1), fmt_text(*f),
                                f->decimals < 0 ? "" : std::to_string(f->decimals)});
            }
            for (auto &line : render_table({"FIELD", "FROM", "TO", "LEN", "FORMAT", "DEC"}, rows))
                sec.text_lines.push_back("  " + line);
            sec.text_lines.push_back("");
        }

        Json rows_json = Json::array();
        for (auto *f : sorted) {
            Json j = Json::object();
            j.set("name", f->name);
            j.set("from", f->from);
            j.set("to", f->to);
            j.set("length", f->to - f->from + 1);
            j.set("decimals", f->decimals);
            j.set("format", fmt_text(*f));
            rows_json.push_back(j);
        }
        Json fj = Json::object();
        fj.set("file", fname);
        fj.set("reclen", reclen);
        fj.set("fields", rows_json);
        files_json.push_back(fj);

        for (size_t i = 0; i < sorted.size(); ++i) {
            auto *f = sorted[i];
            if (reclen > 0 && f->to > reclen) {
                Finding fnd;
                fnd.id = "BUF-OVERRUN";
                fnd.severity = Severity::Error;
                fnd.module = "recordmap";
                fnd.message = "field '" + f->name + "' (bytes " + std::to_string(f->from) + "-" +
                              std::to_string(f->to) + ") extends past " + fname + "'s record length (" +
                              std::to_string(reclen) + ")";
                fnd.file = ir.path;
                fnd.line = f->lineno;
                fnd.spec = 'I';
                fnd.columns = "44-51";
                r.findings.push_back(fnd);
            }
            if (i > 0) {
                auto *prev = sorted[i - 1];
                if (f->from <= prev->to) {
                    Finding fnd;
                    fnd.id = "BUF-OVERLAP";
                    fnd.severity = Severity::Error;
                    fnd.module = "recordmap";
                    fnd.message = "field '" + f->name + "' (bytes " + std::to_string(f->from) + "-" +
                                  std::to_string(f->to) + ") overlaps '" + prev->name + "' (bytes " +
                                  std::to_string(prev->from) + "-" + std::to_string(prev->to) + ") in file " + fname;
                    fnd.file = ir.path;
                    fnd.line = f->lineno;
                    fnd.spec = 'I';
                    fnd.columns = "44-51";
                    fnd.evidence.push_back({"recordmap", prev->lineno});
                    r.findings.push_back(fnd);
                } else if (f->from > prev->to + 1) {
                    Finding fnd;
                    fnd.id = "BUF-GAP";
                    fnd.severity = Severity::Info;
                    fnd.module = "recordmap";
                    fnd.message = "gap of " + std::to_string(f->from - prev->to - 1) + " byte(s) between '" +
                                  prev->name + "' and '" + f->name + "' in file " + fname;
                    fnd.file = ir.path;
                    fnd.line = f->lineno;
                    fnd.spec = 'I';
                    fnd.columns = "44-51";
                    r.findings.push_back(fnd);
                }
            }
        }
    }

    sec.data.set("files", files_json);
    if (!opts.quiet) r.sections.push_back(std::move(sec));

    return r;
}

} // namespace

ModuleInfo make_recordmap_module() {
    return ModuleInfo{"recordmap", "Record layout reconstruction", "FILES", run};
}

} // namespace analyze
