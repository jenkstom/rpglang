// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * fspec.cpp -- parse File Description Specifications.
 * ========================================================================== */
#include "fspec.h"
#include "cspec.h"      // for parse_indicator_token
#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace rpgc {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}
} // namespace

std::vector<FSpec> parse_fspecs(const std::vector<SourceLine> &src) {
    std::vector<FSpec> out;
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'F') continue;

        FSpec f;
        f.lineno = sl.lineno;
        f.name   = col_trim(sl.text, 7, 14);
        if (f.name.empty()) {
            report("input", sl.lineno, 7, DiagKind::Warning,
                   "F-spec with no filename (ignored)");
            continue;
        }

        // File type (col 15)
        switch (upper(col_trim(sl.text, 15, 15))[0]) {
            case 'O': f.type = FileType::Output;    break;
            case 'U': f.type = FileType::Update;    break;
            case 'C': f.type = FileType::Combined;  break;
            default:  f.type = FileType::Input;     break; // 'I' or blank
        }

        // Designation (col 16)
        switch (upper(col_trim(sl.text, 16, 16))[0]) {
            case 'P': f.design = FileDesign::Primary;   break;
            case 'S': f.design = FileDesign::Secondary; break;
            case 'F': f.design = FileDesign::FullProc;  break;
            case 'C': f.design = FileDesign::Chained;   break;
            case 'R': f.design = FileDesign::RecordAddr;break;
            case 'T': f.design = FileDesign::Table;     break;
            case 'D': f.design = FileDesign::Demand;    break;
            default:
                // Output files have blank designation; primary is the cycle
                // default for input files.
                f.design = (f.type == FileType::Output) ? FileDesign::None
                                                        : FileDesign::Primary;
                break;
        }

        // E5: record-address files (designation R) drive a companion "to
        // filename" (E-spec cols 19-26) through the implicit cycle by
        // chaining that file's records off addresses read from this one --
        // a distinct data-flow mode this compiler's cycle codegen never
        // implemented (codegen.cpp never consumes FileDesign::RecordAddr).
        // Rather than silently compile a program whose record-address file
        // is parsed but never actually drives anything (manual 79661), this
        // is a hard error, matching the EXTK/composite-key precedent (B6).
        if (f.design == FileDesign::RecordAddr) {
            report("input", sl.lineno, 16, DiagKind::Error,
                   "F-spec file '" + f.name + "': record-address files "
                   "(designation R) are not implemented");
        }

        // E3: col 17 (end-of-file requirement): E means the file must reach
        // EOF before the program can end; blank means it need not (Section
        // F cycle end-of-job logic; see codegen.cpp's required-files check).
        // Manual 76968-76989.
        std::string eof = upper(col_trim(sl.text, 17, 17));
        f.end_required = (!eof.empty() && eof[0] == 'E');

        // E2: col 18 (sequence): A/D/blank. Consumed by multifile matching
        // (codegen.cpp's decode_m1/selection loop) to detect a
        // descending-sequence file (manual 77002-77030).
        std::string seq18 = upper(col_trim(sl.text, 18, 18));
        if (!seq18.empty() && (seq18[0] == 'A' || seq18[0] == 'D'))
            f.sequence = seq18[0];

        // Format (col 19): F or blank => fixed.
        std::string fmt = upper(col_trim(sl.text, 19, 19));
        f.format = fmt.empty() ? 'F' : fmt[0];

        // Record length (cols 24-27)
        std::string rl = col_trim(sl.text, 24, 27);
        if (!rl.empty()) {
            try { f.reclen = std::stoi(rl); }
            catch (...) { f.reclen = 0; }
        }

        // Device (cols 40-46)
        f.device_text = upper(col_trim(sl.text, 40, 46));
        if (f.device_text == "DISK")          f.device = Device::Disk;
        else if (f.device_text == "PRINTER")  f.device = Device::Printer;
        else if (f.device_text == "WORKSTN")  f.device = Device::Workstn;
        else if (f.device_text == "SPECIAL")  f.device = Device::Special;
        else if (f.device_text == "CONSOLE")  f.device = Device::Console;
        else                                  f.device = Device::Other;

        // E8: WORKSTN/SPECIAL/CONSOLE devices have no codegen support --
        // every file open in this compiler (open_input/open_output/
        // open_update, codegen.cpp) treats the F-spec filename as an
        // ordinary flat DISK/PRINTER file on Linux, regardless of the
        // declared device. A WORKSTN/SPECIAL/CONSOLE file compiled without
        // error would silently try to read/write a plain file named after
        // the device instead of driving an interactive terminal or a
        // special device handler. Hard error instead (manual scope cut,
        // same precedent as EXTK/B6 and record-address files/E5).
        if (f.device == Device::Workstn || f.device == Device::Special ||
            f.device == Device::Console) {
            report("input", sl.lineno, 40, DiagKind::Error,
                   "F-spec file '" + f.name + "': device '" + f.device_text +
                   "' is not implemented (only DISK and PRINTER are "
                   "supported)");
        }

        // Keyed/random access fields (Section G, G24):
        //   col 28 = mode (blank / L / R), cols 29-30 = key length,
        //   col 31  = record-address type (blank / A / I / P),
        //   col 32  = organization (blank / I / T),
        //   cols 35-38 = key-field starting position.
        std::string m = upper(col_trim(sl.text, 28, 28));
        if (!m.empty()) f.mode = m[0];
        std::string kl = col_trim(sl.text, 29, 30);
        if (!kl.empty()) {
            try { f.key_len = std::stoi(kl); } catch (...) {}
        }
        std::string at = upper(col_trim(sl.text, 31, 31));
        if (!at.empty()) f.addr_type = at[0];
        std::string org = upper(col_trim(sl.text, 32, 32));
        if (!org.empty()) f.organization = org[0];
        // B6: cols 35-38 hold a numeric key-start column, except for the
        // composite/noncontiguous-key marker "EXTK" (manual 77556, 77569),
        // which this compiler does not implement. Silently defaulting
        // key_start to 0 on a parse failure produced a wrong-but-compiling
        // program with no diagnostic; a composite key must be a hard error
        // until composite-key support exists.
        std::string ks = col_trim(sl.text, 35, 38);
        if (!ks.empty()) {
            try {
                size_t consumed = 0;
                f.key_start = std::stoi(ks, &consumed);
                if (consumed != ks.size()) throw std::invalid_argument(ks);
            } catch (...) {
                report("input", sl.lineno, 35, DiagKind::Error,
                       "F-spec cols 35-38 for file '" + f.name + "': '" + ks +
                       "' is not a supported key-start value (composite keys "
                       "via EXTK are not implemented)");
            }
        }

        // Overflow indicator (cols 33-34): OA-OG or OV (Section F, F22).
        int ovind = parse_indicator_token(col_trim(sl.text, 33, 34));
        if (ovind <= -13 && ovind >= -20) {   // an overflow indicator
            f.overflow_ind = ovind;
            f.has_overflow = true;
        }

        // E1: cols 71-72, external file conditioning (U1-U8). When the
        // indicator is off, the file is treated as though it has reached
        // end of file: no records are read from or written to it (manual
        // 78727-78739).
        int condind = parse_indicator_token(col_trim(sl.text, 71, 72));
        if (condind <= -21 && condind >= -28) {   // U1..U8
            f.cond_ind = condind;
            f.has_cond = true;
        }

        out.push_back(std::move(f));
    }
    return out;
}

const FSpec *find_primary_input(const std::vector<FSpec> &fs) {
    for (const auto &f : fs) {
        // An update file (type U) feeds the cycle like an input primary (G25).
        if ((f.type == FileType::Input || f.type == FileType::Update)
            && f.design == FileDesign::Primary) {
            return &f;
        }
    }
    return nullptr;
}

std::vector<const FSpec *> secondary_inputs(const std::vector<FSpec> &fs) {
    std::vector<const FSpec *> out;
    for (const auto &f : fs) {
        if (f.type == FileType::Input && f.design == FileDesign::Secondary) {
            out.push_back(&f);
        }
    }
    return out;
}

/* Line-counter specifications (form type 'L'): cols 7-14 filename, 15-17 lines
 * per page, 18-19 "FL" marker, 20-22 overflow line, 23-24 "OL" marker. We read
 * only the two numbers; markers are accepted and ignored (Section F, F22). */
std::unordered_map<std::string, LineCounter> parse_lspecs(const std::vector<SourceLine> &src) {
    std::unordered_map<std::string, LineCounter> out;
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'L') continue;
        std::string name = col_trim(sl.text, 7, 14);
        if (name.empty()) continue;
        LineCounter lc;
        std::string lpp = col_trim(sl.text, 15, 17);
        if (!lpp.empty()) {
            try { lc.lines_per_page = std::stoi(lpp); } catch (...) {}
            if (lc.lines_per_page < 1) lc.lines_per_page = 66;
        }
        std::string ofl = col_trim(sl.text, 20, 22);
        if (!ofl.empty()) {
            try { lc.overflow_line = std::stoi(ofl); } catch (...) {}
            if (lc.overflow_line < 2) lc.overflow_line = lc.lines_per_page;
        } else {
            // Manual default: six lines from the bottom (line 60 of 66).
            lc.overflow_line = lc.lines_per_page - 6;
        }
        out[name] = lc;
    }
    return out;
}

} // namespace rpgc
