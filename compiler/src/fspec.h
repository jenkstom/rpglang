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
enum class Device       { Disk, Printer, Workstn, Special, Console, Other };

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
