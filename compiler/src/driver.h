/* ========================================================================== *
 * driver.h -- backend driver: turn an llvm::Module into IR text / assembly /
 *             object / executable by invoking llc and clang.
 *
 * The pipeline (for --emit-exe, the default):
 *
 *   module --(opt passes)--> optimized module
 *          --(print)------>  tmp.ll
 *          --(llc)-------->  tmp.o      (relocatable object)
 *          --(clang)------>  out        (ELF, linked with librpgruntime.a)
 *
 * `--emit-ir`  stops after printing the (optionally optimized) module.
 * `--emit-asm` stops after llc -filetype=asm.
 * `--emit-obj` stops after llc -filetype=obj.
 * `--emit-exe` runs the whole chain.
 *
 * `-O<level>` (0/1/2/z) controls the LLVM optimization passes run on the
 * module before emission. Default -O0 (none) -- RPG field globals and the
 * cycle are kept intact, which makes generated IR readable; -O2 collapses the
 * trivial indicator blocks and produces tight native code.
 * ========================================================================== */
#ifndef RPGC_DRIVER_H
#define RPGC_DRIVER_H

#include <llvm/IR/Module.h>
#include <string>

namespace rpgc {

struct DriverOptions {
    std::string clang_path;     // from CLI / cmake default
    std::string llc_path;       // from CLI / cmake default
    std::string runtime_lib;    // path to librpgruntime.a
    std::string runtime_inc;    // include dir for rpg_runtime.h
    std::string emit;           // "ir" | "asm" | "obj" | "exe"
    std::string output_file;
    int  opt_level = 0;         // 0,1,2 (z mapped to 2 + size opts later)
    bool verbose      = false;
    bool save_temps   = false;  // keep intermediate .ll/.o
};

/* Run the requested pipeline on `mod`. Returns true on success.
 * Reports failures via rpgc::error/fatal. */
bool drive(llvm::Module &mod, const DriverOptions &opts);

} // namespace rpgc

#endif // RPGC_DRIVER_H
