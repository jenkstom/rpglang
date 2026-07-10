/* ========================================================================== *
 * espec.h -- parse Extension Specifications (E-specs) for arrays/tables.
 *
 * Phase 9 implements a practical subset: compile-time and run-time numeric
 * arrays (no tables, no prerun-time). See docs/SPEC_MAP.md for columns.
 * ========================================================================== */
#ifndef RPGC_ESPEC_H
#define RPGC_ESPEC_H

#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

/* How the array's initial contents are supplied. */
enum class ArrayLoad {
    CompileTime,    // data follows the O-specs after a "** " record
    RunTime,        // loaded by C-specs at run time (zero-initialised)
    PreRunTime      // loaded from a file at program start (deferred)
};

struct ESpecArray {
    int         lineno = 0;
    std::string name;                 // cols 27-32
    int         entries = 0;          // cols 36-39 (total element count)
    int         entry_len = 0;        // cols 40-42 (digits per element)
    int         decimals = -1;        // col 44 (-1 = alphanumeric)
    bool        ascending = false;    // col 45 == 'A'
    ArrayLoad   load = ArrayLoad::RunTime;
    std::string from_file;            // cols 11-18 (prerun-time)
    std::vector<long> init_data;      // compile-time values (numeric)
};

std::vector<ESpecArray> parse_especs(const std::vector<SourceLine> &src);

/* Read compile-time array data: lines after the source, introduced by "** ".
 * Fills each ESpecArray.init_data in declaration order. */
void load_compile_time_data(const std::vector<SourceLine> &src,
                            std::vector<ESpecArray> &arrays);

} // namespace rpgc

#endif // RPGC_ESPEC_H
