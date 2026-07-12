/* ========================================================================== *
 * espec.h -- parse Extension Specifications (E-specs) for arrays/tables.
 *
 * Section B extends Phase 9: tables (TAB-prefixed names), prerun-time arrays
 * loaded from a file at program start, and alternating arrays/tables (the
 * partner defined in columns 46-57). See docs/SPEC_MAP.md for columns.
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
    PreRunTime      // loaded from a file at program start (Section B)
};

struct ESpecArray {
    int         lineno = 0;
    std::string name;                 // cols 27-32
    int         entries = 0;          // cols 36-39 (total element count)
    int         entry_len = 0;        // cols 40-42 (digits per element)
    int         decimals = -1;        // col 44 (-1 = alphanumeric)
    bool        ascending = false;    // col 45 == 'A'
    char        data_format = 0;      // col 43: 0=zoned(blank), 'P'=packed, 'B'=binary
    ArrayLoad   load = ArrayLoad::RunTime;
    std::string from_file;            // cols 11-18 (prerun-time)
    std::vector<long> init_data;      // compile-time values (numeric)
    // Compile-time values for an alphanumeric array/table (A9): fixed-width
    // (entry_len-byte) strings, parallel to init_data. Populated instead of
    // init_data when decimals < 0.
    std::vector<std::string> init_str;
    // Alternating partner (cols 46-57): a second array/table whose elements
    // interleave with `name`'s on each input record. Blank when not used.
    std::string alt_name;             // cols 46-51
    int         alt_entry_len = 0;    // cols 52-54
    int         alt_decimals  = -1;   // col 56 (-1 = alphanumeric)
    bool        alt_ascending = false;// col 57 == 'A'
    char        alt_data_format = 0;  // col 55: 0=zoned(blank), 'P'=packed, 'B'=binary
    std::vector<long> alt_init_data;  // compile-time values for the partner
    std::vector<std::string> alt_init_str;  // alphanumeric partner values (A9)
};

/* A name beginning with TAB (case-insensitive) denotes a table rather than an
 * array, per the IBM manual. Tables have no explicit indexing; a hidden
 * "current element" is maintained and updated by LOKUP. */
bool is_table_name(const std::string &name);

std::vector<ESpecArray> parse_especs(const std::vector<SourceLine> &src);

/* Read compile-time array data: lines after the source, introduced by "** ".
 * Fills each ESpecArray.init_data in declaration order. */
void load_compile_time_data(const std::vector<SourceLine> &src,
                            std::vector<ESpecArray> &arrays);

} // namespace rpgc

#endif // RPGC_ESPEC_H
