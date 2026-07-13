// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * fspec.h -- parse File Description Specifications.
 *
 * Phase 3 only needs a subset: the filename, file type, designation, format,
 * record length, and device. See docs/SPEC_MAP.md for the full column map.
 * ========================================================================== */
#ifndef RPGC_FSPEC_H
#define RPGC_FSPEC_H

#include "source.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace rpgc {

enum class FileType     { Input, Output, Update, Combined };
enum class FileDesign   { Primary, Secondary, FullProc, Chained, RecordAddr,
                          Table, Demand, None };
enum class Device       { Disk, Printer, Workstn, Special, Console, Keybord,
                          Crt, Other };

struct FSpec {
    int         lineno = 0;
    std::string name;                 // cols 7-14
    FileType    type     = FileType::Input;        // col 15
    FileDesign  design   = FileDesign::Primary;    // col 16
    char        mode     = 0;         // col 28: blank / L (within limits) / R (random)
    int         key_len  = 0;         // cols 29-30: length of key/record-address field
    char        addr_type = 0;        // col 31: blank / A (zoned key) / I (RRN) / P (packed)
    char        organization = 0;     // col 32: blank / I (indexed) / T (address-output)
    char        format   = 'F';       // col 19  (F/blank = fixed)
    int         reclen   = 0;         // cols 24-27
    int         key_start = 0;        // cols 35-38: 1-based record position of the key
    int         overflow_ind = 0;     // cols 33-34: overflow indicator index (Section F)
    bool        has_overflow = false; // true if an overflow indicator is assigned
    Device      device   = Device::Disk;           // cols 40-46
    std::string device_text;          // raw, for diagnostics
    char        sequence = 0;         // col 18: blank / A (ascending) / D (descending)
    bool        end_required = false; // col 17: file must reach EOF before program can end
    int         cond_ind  = 0;        // cols 71-72: U1-U8 external file-condition indicator
    bool        has_cond  = false;    // true if a conditioning indicator is assigned

    // WORKSTN continuation-line options (manual "Continuation-Line Options
    // for WORKSTN File", cols 54-59 keyword / 60-65 or 60-67 value). Each is
    // its own physical F-spec line with a blank filename (cols 7-14) that
    // applies to the most recently named file -- see parse_fspecs.
    int         num = 1;              // NUM: max devices attachable at once (<=251)
    std::string savds;                // SAVDS: DS name swapped per attached device
    int         ind_count = 0;        // IND: indicator count (01-nn) swapped per device
    std::string sln;                  // SLN: 2-digit field naming the variable start line
    std::string fmts;                 // FMTS: display-format file name (W2); default set
                                       // at parse time to name + "FM" if blank
    std::string id_field;             // ID: 2-char field holding the responding device ID
    std::string infsr;                // INFSR: exception/error-processing subroutine name
    std::string infds;                // INFDS: file-information DS name (see ispec.h)
    std::string cfile;                // CFILE: ICF communications file (parsed, inert)

    // W2: resolved absolute path of `fmts`'s .dspf file, filled in by
    // main.cpp after parse_fspecs() once the program-id default (name +
    // "FM") can be applied and the file can be looked up on disk. Baked into
    // the generated IR as a literal path, same convention as an ordinary
    // DISK filename (codegen.cpp's open_input_files).
    std::string fmts_path;
};

/* Parse all F-specs. Returns one FSpec per 'F' line. */
std::vector<FSpec> parse_fspecs(const std::vector<SourceLine> &src);

/* Convenience: find the primary input file, if any (designation == Primary
 * and type == Input). nullptr if none. */
const FSpec *find_primary_input(const std::vector<FSpec> &fs);

/* Secondary input files (designation == Secondary and type == Input) in
 * F-spec order. Used by multifile/matching processing (Section F). */
std::vector<const FSpec *> secondary_inputs(const std::vector<FSpec> &fs);

/* Line-counter specifications (form type 'L'). Maps a PRINTER file name to
 * {lines_per_page, overflow_line}. Section F (F22). */
struct LineCounter { int lines_per_page = 66; int overflow_line = 60; };
std::unordered_map<std::string, LineCounter> parse_lspecs(const std::vector<SourceLine> &src);

} // namespace rpgc

#endif // RPGC_FSPEC_H
