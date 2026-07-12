// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * driver.cpp -- backend driver implementation.
 *
 * Pipeline order:
 *   1. (optional) run the LLVM new-pass-manager optimization pipeline
 *   2. write the module as textual IR to a temp .ll
 *   3. invoke llc for asm/obj (or hand the .ll to clang for the final ELF)
 *   4. for --emit-exe, invoke clang to link the object with the runtime
 *
 * Intermediate files live under a unique temp dir so concurrent rpgc runs do
 * not collide; they are removed unless --save-temps or -v.
 * ========================================================================== */
#include "driver.h"
#include "diagnostics.h"

#include <llvm/IR/Module.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace rpgc {

namespace {

/* Run the LLVM optimization pipeline in-place on the module. level 0 = none. */
void optimize_module(llvm::Module &mod, int level) {
    if (level <= 0) return;

    using namespace llvm;
    PassBuilder pb;
    ModuleAnalysisManager mam;
    // Register the analysis managers with each other + target-only analyses.
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    LoopAnalysisManager lam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    OptimizationLevel ol;
    switch (level) {
        case 1:  ol = OptimizationLevel::O1;     break;
        case 2:  ol = OptimizationLevel::O2;     break;
        case 3:  ol = OptimizationLevel::O3;     break;
        default: ol = OptimizationLevel::O2;     break;
    }
    ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(ol);
    mpm.run(mod, mam);

    // Explicitly clear the analysis managers. The -O2/-O3 pipelines run passes
    // (GlobalDCE, function deletion) that can remove IR elements whose
    // pointers are still cached in the managers; dropping the caches here
    // avoids use-after-free when the locals go out of scope.
    mam.clear();
    fam.clear();
    cgam.clear();
    lam.clear();
}

/* Write the module as textual IR to `path`. */
bool write_ir(llvm::Module &mod, const std::string &path) {
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
    if (ec) {
        error("cannot open '" + path + "' for writing: " + ec.message());
        return false;
    }
    mod.print(out, nullptr);
    out.flush();
    return true;
}

/* Run a shell command via system(); returns true if it exited 0. Paths are
 * single-quoted to survive spaces. Output is captured unless verbose. */
bool run(const std::string &cmd, bool verbose, const std::string &errctx) {
    if (verbose) {
        std::fprintf(stderr, "rpgc: run: %s\n", cmd.c_str());
        int st = std::system(cmd.c_str());
        if (st == -1) { error(errctx + ": failed to invoke shell"); return false; }
        return WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    // Quiet: redirect stdout+stderr to /dev/null.
    std::string q = cmd + " >/dev/null 2>&1";
    int st = std::system(q.c_str());
    if (st == -1) { error(errctx + ": failed to invoke shell"); return false; }
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        error(errctx + " failed (rerun with -v for tool output)");
        return false;
    }
    return true;
}

/* Derive the -l name and -L dir from a librpgruntime.a path. */
void split_lib(const std::string &lib, std::string &dir, std::string &lname) {
    auto slash = lib.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : lib.substr(0, slash);
    std::string base = (slash == std::string::npos) ? lib : lib.substr(slash + 1);
    if (base.rfind("lib", 0) == 0) base = base.substr(3);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    lname = base;
}

/* Make a unique temp directory; returns "" on failure. */
std::string make_tmpdir() {
    char tmpl[] = "/tmp/rpgcXXXXXX";
    if (char *d = mkdtemp(tmpl)) return std::string(d);
    return "";
}

/* Remove a temp dir and its .ll. */
void rmdir_tmp(const std::string &dir, const std::string &ll) {
    std::remove(ll.c_str());
    rmdir(dir.c_str());
}

} // namespace

bool drive(llvm::Module &mod, const DriverOptions &opts) {
    // 1. Optimize (no-op at -O0).
    optimize_module(mod, opts.opt_level);

    // Scratch dir for intermediates. In --emit-ir mode the output IS the .ll,
    // so no temp dir is needed.
    std::string tmpdir;
    if (opts.emit != "ir") {
        tmpdir = make_tmpdir();
        if (tmpdir.empty()) {
            error("cannot create temp directory");
            return false;
        }
    }

    // 2. Write IR. For --emit-ir this is the final output.
    std::string ll = (opts.emit == "ir")
                         ? opts.output_file
                         : tmpdir + "/out.ll";
    if (!write_ir(mod, ll)) return false;
    if (opts.emit == "ir") return true;

    // 3. Run llc to produce asm or obj.
    //     -filetype=asm -> .s ; -filetype=obj -> .o
    std::string ext = (opts.emit == "asm") ? ".s" : ".o";
    std::string tmp_out = tmpdir + "/out" + ext;

    if (opts.emit == "asm" || opts.emit == "obj") {
        std::string ft = (opts.emit == "asm") ? "asm" : "obj";
        // -relocation-model=pic so the object links under a PIE-default linker
        // (Ubuntu's default). Harmless for -O0 too.
        std::string cmd = "'" + opts.llc_path + "' -filetype=" + ft +
                          " -relocation-model=pic '" + ll + "' -o '" + tmp_out + "'";
        if (!run(cmd, opts.verbose,
                 opts.emit == "asm" ? "llc (assembly)" : "llc (object)")) {
            return false;
        }
        // Move the result to the requested output path.
        std::string mv = "mv -f '" + tmp_out + "' '" + opts.output_file + "'";
        if (!run(mv, opts.verbose, "install output")) return false;
        if (!opts.save_temps) rmdir_tmp(tmpdir, ll);
        return true;
    }

    // 4. --emit-exe: llc -> object, then clang -> ELF linked with runtime.
    std::string obj = tmpdir + "/out.o";
    std::string llc_cmd = "'" + opts.llc_path + "' -filetype=obj"
                          " -relocation-model=pic '" + ll +
                          "' -o '" + obj + "'";
    if (!run(llc_cmd, opts.verbose, "llc (object)")) return false;

    std::string libdir, lname;
    split_lib(opts.runtime_lib, libdir, lname);
    // -no-pie: RPG programs are standalone executables; the system linker
    // defaults to PIE, which needs PIC objects. -no-pie is the simplest robust
    // choice and matches a classic batch-program model.
    std::string link_cmd = "'" + opts.clang_path + "' -no-pie '" + obj +
                           "' -o '" + opts.output_file + "' '-L" + libdir +
                           "' -l" + lname;
    if (!run(link_cmd, opts.verbose, "clang (link)")) return false;

    if (!opts.save_temps && !opts.verbose) {
        std::remove(obj.c_str());
        std::remove(ll.c_str());
        rmdir(tmpdir.c_str());
    } else if (opts.save_temps) {
        // Leave intermediates; tell the user where they are.
        std::fprintf(stderr, "rpgc: intermediates kept in %s\n", tmpdir.c_str());
    }
    return true;
}

} // namespace rpgc
