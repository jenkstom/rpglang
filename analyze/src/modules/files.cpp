// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* files -- file description map (TOOLS_IDEAS.md §4.2). */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

const char *file_type_text(rpgc::FileType t) {
    switch (t) {
        case rpgc::FileType::Input:    return "Input";
        case rpgc::FileType::Output:   return "Output";
        case rpgc::FileType::Update:   return "Update";
        case rpgc::FileType::Combined: return "Combined";
    }
    return "?";
}

const char *file_design_text(rpgc::FileDesign d) {
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

const char *device_text(rpgc::Device d) {
    switch (d) {
        case rpgc::Device::Disk:    return "Disk";
        case rpgc::Device::Printer: return "Printer";
        case rpgc::Device::Workstn: return "WORKSTN";
        case rpgc::Device::Special: return "SPECIAL";
        case rpgc::Device::Console: return "CONSOLE";
        case rpgc::Device::Other:   return "Other";
    }
    return "?";
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "files";
    r.title = "File description map";

    std::vector<std::vector<std::string>> rows;
    Json arr = Json::array();

    for (auto &f : ir.prog.files) {
        std::string ovf = f.has_overflow ? indicator_label(f.overflow_ind) : "";
        std::string cond = f.has_cond ? indicator_label(f.cond_ind) : "";
        rows.push_back({f.name, file_type_text(f.type), file_design_text(f.design),
                        device_text(f.device), std::string(1, f.format ? f.format : 'F'),
                        std::to_string(f.reclen),
                        f.key_len ? std::to_string(f.key_len) : "",
                        f.key_start ? std::to_string(f.key_start) : "",
                        ovf, cond, f.end_required ? "yes" : "no"});

        Json j = Json::object();
        j.set("name", f.name);
        j.set("type", file_type_text(f.type));
        j.set("design", file_design_text(f.design));
        j.set("device", device_text(f.device));
        j.set("format", std::string(1, f.format ? f.format : 'F'));
        j.set("reclen", f.reclen);
        j.set("key_len", f.key_len);
        j.set("key_start", f.key_start);
        j.set("overflow_ind", ovf);
        j.set("cond_ind", cond);
        j.set("end_required", f.end_required);
        j.set("line", f.lineno);
        arr.push_back(j);

        if (f.device == rpgc::Device::Workstn || f.device == rpgc::Device::Special ||
            f.device == rpgc::Device::Console) {
            Finding fnd;
            fnd.id = "FILES-WORKSTN";
            fnd.severity = Severity::Error;
            fnd.module = "files";
            fnd.message = "file '" + f.name + "' uses unsupported device " + device_text(f.device);
            fnd.file = ir.path;
            fnd.line = f.lineno;
            fnd.spec = 'F';
            fnd.columns = "40-46";
            r.findings.push_back(fnd);
        }
        if (f.design == rpgc::FileDesign::RecordAddr) {
            Finding fnd;
            fnd.id = "FILES-RECADDR";
            fnd.severity = Severity::Error;
            fnd.module = "files";
            fnd.message = "file '" + f.name + "' uses unsupported record-address designation";
            fnd.file = ir.path;
            fnd.line = f.lineno;
            fnd.spec = 'F';
            fnd.columns = "16";
            r.findings.push_back(fnd);
        }
        if ((f.format == 'F' || f.format == 0) && f.reclen <= 0 &&
            f.device != rpgc::Device::Workstn && f.device != rpgc::Device::Special &&
            f.device != rpgc::Device::Console) {
            Finding fnd;
            fnd.id = "FILES-NOSIZE";
            fnd.severity = Severity::Warn;
            fnd.module = "files";
            fnd.message = "file '" + f.name + "' has no record length on a fixed-format file";
            fnd.file = ir.path;
            fnd.line = f.lineno;
            fnd.spec = 'F';
            fnd.columns = "24-27";
            r.findings.push_back(fnd);
        }
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "files";
        sec.title = "File description map";
        sec.text_lines = render_table(
            {"FILE", "TYPE", "DESIGN", "DEVICE", "FMT", "RECLEN", "KEYLEN", "KEYPOS", "OVFL", "COND", "EOFREQ"},
            rows);
        sec.data.set("files", arr);
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_files_module() {
    return ModuleInfo{"files", "File description map", "FILES", run};
}

} // namespace analyze
