// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * modules/modules.h -- factory declarations for every analysis module.
 * Catalog order (registry.cpp) matches TOOLS_IDEAS.md §4.1-4.20.
 * ========================================================================== */
#ifndef RPGANALYZE_MODULES_H
#define RPGANALYZE_MODULES_H

#include "../module.h"

namespace analyze {

ModuleInfo make_summary_module();
ModuleInfo make_files_module();
ModuleInfo make_recordmap_module();
ModuleInfo make_fields_module();
ModuleInfo make_xref_module();
ModuleInfo make_indicators_module();
ModuleInfo make_subroutines_module();
ModuleInfo make_controlflow_module();
ModuleInfo make_cycle_module();
ModuleInfo make_complexity_module();
ModuleInfo make_deadcode_module();
ModuleInfo make_security_module();
ModuleInfo make_compat_module();
ModuleInfo make_condlogic_module();
ModuleInfo make_buffer_module();
ModuleInfo make_termination_module();
ModuleInfo make_dataflow_module();
ModuleInfo make_deps_module();
ModuleInfo make_comments_module();
ModuleInfo make_smells_module();

} // namespace analyze

#endif // RPGANALYZE_MODULES_H
