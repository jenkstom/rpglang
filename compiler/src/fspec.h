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
    char        format   = 'F';       // col 19  (F/blank = fixed)
    int         reclen   = 0;         // cols 24-27
    Device      device   = Device::Disk;           // cols 40-46
    std::string device_text;          // raw, for diagnostics
};

/* Parse all F-specs. Returns one FSpec per 'F' line. */
std::vector<FSpec> parse_fspecs(const std::vector<SourceLine> &src);

/* Convenience: find the primary input file, if any (designation == Primary
 * and type == Input). nullptr if none. */
const FSpec *find_primary_input(const std::vector<FSpec> &fs);

} // namespace rpgc

#endif // RPGC_FSPEC_H
