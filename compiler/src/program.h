// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * program.h -- aggregate of all parsed specs (F, I, C) for one program.
 *
 * This is the input to code generation. Phase 3 adds the file/input layer on
 * top of the Phase 2 C-spec layer.
 * ========================================================================== */
#ifndef RPGC_PROGRAM_H
#define RPGC_PROGRAM_H

#include "cspec.h"
#include "fspec.h"
#include "hspec.h"
#include "ispec.h"
#include "ospec.h"
#include "espec.h"
#include <vector>

namespace rpgc {

struct Program {
    HSpec                    hspec;         // D1: control specification
    std::vector<FSpec>      files;
    std::unordered_map<std::string, LineCounter> line_counters;  // L-specs (F22)
    std::vector<ISpecRec>   in_records;
    std::vector<ISpecField> in_fields;
    std::vector<ISpecField> lookahead_fields;   // look-ahead fields (E19)
    std::vector<ISpecDS>       data_structures; // D2: DS statements
    std::vector<ISpecSubfield> ds_subfields;    // D2: DS subfield lines
    std::vector<CSpec>      calcs;
    std::vector<ORecord>    outputs;
    std::vector<ESpecArray> arrays;
    std::vector<ParamList>  param_lists;  // program linkage: PLIST/PARM
    std::vector<ExitDecl>   exit_decls;
};

} // namespace rpgc

#endif // RPGC_PROGRAM_H
