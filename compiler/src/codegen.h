// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * codegen.h -- turn parsed C-specs into an llvm::Module.
 *
 * The generated module defines:
 *   @rpg_in      - [100 x i1]  global indicator array (slots 1..99)
 *   @rpg_lr      - i1          last-record indicator
 *   @main        - i32 ()      the program entry; runs all C-specs in order
 *
 * Each C-spec is emitted in a basic block. Conditioning indicators (cols 9-17)
 * gate the op via a conditional branch: if all AND-conditions hold, the op
 * block runs; otherwise it is skipped to the next spec.
 *
 * Phase 2 implements ADD / Z-ADD / SETON / SETOF. The indicator array and LR
 * latch are modelled exactly as described in docs/ARCHITECTURE.md.
 * ========================================================================== */
#ifndef RPGC_CODEGEN_H
#define RPGC_CODEGEN_H

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>
#include <vector>

#include "cspec.h"
#include "program.h"

namespace rpgc {

/* Build a module from a Program. If the program has a primary input file, the
 * implicit RPG cycle is generated (fetch → extract fields → detail calcs →
 * LR total → close). Otherwise the Phase 2 linear form is used (C-specs run
 * once, in order). The caller MUST own `ctx` and keep it alive for as long as
 * the returned module is used.
 *
 * `is_top_level` (program linkage) selects the shape of the
 * generated entry function: true (the default, and every existing caller's
 * behavior) produces today's `i32 @main()`; false produces a uniquely-named
 * `rpg_prog_<NAME>` function matching the rpg_entry_fn ABI (see
 * runtime/rpg_runtime.h) and self-registers it into the runtime program
 * registry, for a "library" program only reachable via CALL in a multi-file
 * build (see main.cpp). */
std::unique_ptr<llvm::Module> generate_module(
    const Program &prog,
    const std::string &module_name,
    llvm::LLVMContext &ctx,
    bool is_top_level = true);

/* Phase 2 entry retained for tests/back-compat: linear codegen from C-specs
 * only. */
std::unique_ptr<llvm::Module> generate_module_linear(
    const std::vector<CSpec> &specs,
    const std::string &module_name,
    llvm::LLVMContext &ctx);

} // namespace rpgc

#endif // RPGC_CODEGEN_H
