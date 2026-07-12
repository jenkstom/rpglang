// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * main.cpp -- entry point for the `rpgc` RPG II -> LLVM compiler.
 *
 * Phase 1 scope:
 *   - parse command-line flags (-o, --emit-ir, -S, -c, --llc, --clang, -v)
 *   - read the source file off disk
 *   - prove that we are linked against a usable LLVM by querying its version
 *   - print a friendly banner
 *   - exit 0 on a valid (even if empty) invocation, non-zero on misuse
 *
 * Real parsing / IR generation / backend driving arrive in Phases 2-5. The
 * flag surface here is deliberately the *final* surface so test scripts can
 * lock onto it now.
 * ========================================================================== */

#include "diagnostics.h"
#include "source.h"
#include "cspec.h"
#include "fspec.h"
#include "hspec.h"
#include "ispec.h"
#include "ospec.h"
#include "espec.h"
#include "uspec.h"
#include "program.h"
#include "codegen.h"
#include "driver.h"

// LLVM C++ API. Including these proves the library is linked and the headers
// are visible.
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
// In LLVM 19 the host-triple helpers moved from Support/Host.h into the new
// TargetParser subdirectory.
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Linker/Linker.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

// Defaults injected by CMake (see compiler/CMakeLists.txt).
#ifndef RPGC_DEFAULT_LLC
#define RPGC_DEFAULT_LLC "llc"
#endif
#ifndef RPGC_DEFAULT_CLANG
#define RPGC_DEFAULT_CLANG "clang"
#endif
#ifndef RPGC_RUNTIME_LIB
#define RPGC_RUNTIME_LIB "librpgruntime.a"
#endif

namespace {

// GPLv3-mandated copyright/warranty notice (§5(a)). Printed by --version,
// --help, and the interactive startup banner.
const std::string kLicenseNotice =
    "Copyright (C) 2026 Tom White\n"
    "License GPLv3+: GNU GPL version 3 or later <https://www.gnu.org/licenses/gpl.html>\n"
    "This is free software: you are free to change and redistribute it.\n"
    "There is NO WARRANTY, to the extent permitted by law.\n";

bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Where rpgc looks for librpgruntime.a when `--runtime` isn't given.
//
// RPGC_RUNTIME_LIB (baked in at configure time) is an absolute path into the
// *build tree*, which only exists on the machine that built rpgc. Once rpgc
// is installed elsewhere (e.g. from a .deb, at /usr/bin/rpgc) that path is
// gone. So first look for the lib relative to the running executable itself
// -- <prefix>/lib/rpgc/librpgruntime.a next to a <prefix>/bin/rpgc -- which is
// exactly the layout `make install` / the Debian package produce. Only fall
// back to the compiled-in build-tree path for uninstalled dev builds.
std::string default_runtime_lib() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string exe(buf);
        auto slash = exe.find_last_of('/');
        if (slash != std::string::npos) {
            std::string bindir = exe.substr(0, slash);
            std::string prefix = bindir;
            const std::string suffix = "/bin";
            if (prefix.size() >= suffix.size() &&
                prefix.compare(prefix.size() - suffix.size(), suffix.size(), suffix) == 0) {
                prefix = prefix.substr(0, prefix.size() - suffix.size());
            }
            std::string candidate = prefix + "/lib/rpgc/librpgruntime.a";
            if (file_exists(candidate)) return candidate;
        }
    }
    return RPGC_RUNTIME_LIB;
}

struct Options {
    // The .rpg source file(s). Program linkage's multi-file build:
    // input_files[0] is the top-level program (compiled to the process's
    // own `main`); every other file is a "library" program only reachable
    // via CALL, compiled into its own rpg_prog_<NAME> and linked into the
    // same executable. A single file is the (unchanged) common case.
    std::vector<std::string> input_files;
    std::string output_file;         // -o ; default derived from input
    std::string emit;                // --emit-ir (LLVM IR) | --emit-asm | --emit-obj | --emit-exe (default)
    std::string llc_path   = RPGC_DEFAULT_LLC;
    std::string clang_path = RPGC_DEFAULT_CLANG;
    std::string runtime_lib;         // resolved lazily: see default_runtime_lib()
    bool print_version = false;
    bool print_help    = false;
    bool verbose       = false;
    bool save_temps    = false;
    int  opt_level     = 0;
};

void print_help() {
    std::cout <<
        "rpgc -- RPG II to LLVM compiler\n"
        "\n"
        "USAGE:\n"
        "    rpgc [OPTIONS] <input.rpg>\n"
        "\n"
        "OPTIONS:\n"
        "    -o <file>          output file (default: input name with .x/.o/.s/.ll)\n"
        "    --emit-ir          emit textual LLVM IR (.ll)\n"
        "    --emit-asm         emit assembly (.s)\n"
        "    --emit-obj         emit relocatable object (.o)\n"
        "    --emit-exe         emit a final executable (default)\n"
        "    --llc <path>       path to llc (default: " << RPGC_DEFAULT_LLC << ")\n"
        "    --clang <path>     path to clang (default: " << RPGC_DEFAULT_CLANG << ")\n"
        "    --runtime <path>   path to librpgruntime.a\n"
        "    -O<level>          LLVM optimization level (0-3, default 0)\n"
        "    --save-temps       keep intermediate .ll/.o files\n"
        "    -v                 verbose (print each tool invocation)\n"
        "    --version          print version and exit\n"
        "    -h, --help         print this help and exit\n"
        "\n"
        + kLicenseNotice;
}

void print_version() {
    std::cout << "rpgc 0.1.0 (phase 1)\n";
    std::cout << "LLVM " << LLVM_VERSION_STRING
              << " (linked; target triple default = "
              << llvm::sys::getDefaultTargetTriple() << ")\n";
    std::cout << kLicenseNotice;
}

/* Read the whole source file into a string. Returns false on I/O error. */
bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

/* Derive the default output name from the input + emit mode. */
std::string default_output(const std::string &input, const std::string &emit) {
    auto dot = input.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? input : input.substr(0, dot);
    if (emit == "ir")  return stem + ".ll";
    if (emit == "asm") return stem + ".s";
    if (emit == "obj") return stem + ".o";
    return stem + ".x";
}

bool parse_args(int argc, char **argv, Options &opts) {
    // Defaults
    opts.emit = "exe";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            opts.print_help = true;
            return true;
        } else if (a == "--version") {
            opts.print_version = true;
            return true;
        } else if (a == "-v") {
            opts.verbose = true;
        } else if (a == "-o") {
            if (++i >= argc) { rpgc::error("missing argument to -o"); return false; }
            opts.output_file = argv[i];
        } else if (a == "--emit-ir") {
            opts.emit = "ir";
        } else if (a == "--emit-asm") {
            opts.emit = "asm";
        } else if (a == "--emit-obj") {
            opts.emit = "obj";
        } else if (a == "--emit-exe") {
            opts.emit = "exe";
        } else if (a == "--llc") {
            if (++i >= argc) { rpgc::error("missing argument to --llc"); return false; }
            opts.llc_path = argv[i];
        } else if (a == "--clang") {
            if (++i >= argc) { rpgc::error("missing argument to --clang"); return false; }
            opts.clang_path = argv[i];
        } else if (a == "--runtime") {
            if (++i >= argc) { rpgc::error("missing argument to --runtime"); return false; }
            opts.runtime_lib = argv[i];
        } else if (a == "--save-temps") {
            opts.save_temps = true;
        } else if (a.size() >= 2 && a[0] == '-' && a[1] == 'O') {
            // -O<level> : -O0..-O3
            std::string lvl = a.substr(2);
            try { opts.opt_level = std::stoi(lvl); }
            catch (...) { rpgc::error("invalid -O level: " + a); return false; }
            if (opts.opt_level < 0 || opts.opt_level > 3) {
                rpgc::error("-O level must be 0..3");
                return false;
            }
        } else if (a.rfind("--", 0) == 0 || a.rfind("-", 0) == 0) {
            rpgc::error("unknown option: " + a);
            return false;
        } else {
            // Multiple positional .rpg files: a multi-program build
            // (program linkage). The first is the top-level program.
            opts.input_files.push_back(a);
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    // Initialise only the *native* target machinery. We link against just the
    // `native` component (x86-64 on this host), so InitializeAll* variants --
    // which would pull in every backend's symbols -- would fail to link.
    // The native set is all we ever need: we emit code for the host triple.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmParser();
    llvm::InitializeNativeTargetAsmPrinter();

    Options opts;
    if (!parse_args(argc, argv, opts)) {
        return 1;
    }
    if (opts.print_help)    { print_help();    return 0; }
    if (opts.print_version) { print_version(); return 0; }

    if (opts.input_files.empty()) {
        rpgc::error("no input file");
        std::cerr << "run 'rpgc --help' for usage\n";
        return 1;
    }

    if (opts.output_file.empty()) {
        opts.output_file = default_output(opts.input_files[0], opts.emit);
    }
    if (opts.runtime_lib.empty()) {
        opts.runtime_lib = default_runtime_lib();
    }

    // The LLVMContext MUST outlive every module; keep it in main's scope and
    // pass it down to codegen. All per-file modules below share it so they
    // can be linked together into one final module.
    llvm::LLVMContext ctx;
    std::vector<std::unique_ptr<llvm::Module>> mods;

    // ---- Phase 3: parse F/I/C -> codegen, once per input file ---------------
    // Program linkage: input_files[0] is the top-level program
    // (compiled to `main`); every other file is a "library" program only
    // reachable via CALL from something compiled alongside it, compiled to
    // its own rpg_prog_<NAME> and self-registered into the runtime program
    // registry. All resulting modules are linked into one before handing off
    // to the backend driver.
    for (size_t fi = 0; fi < opts.input_files.size(); ++fi) {
        const std::string &input_file = opts.input_files[fi];
        bool is_top_level = (fi == 0);

        std::string source;
        if (!read_file(input_file, source)) {
            rpgc::fatal("cannot open input file: " + input_file);
            return 1;
        }

        std::vector<rpgc::SourceLine> src;
        if (!rpgc::load_source(input_file, src)) {
            rpgc::fatal("cannot read source: " + input_file);
            return 1;
        }
        // D3: expand /COPY directives before any spec parser sees the source
        // -- each parser just filters a flat line list by form_type, so
        // splicing in copied members here (rather than teaching every parser
        // about /COPY) keeps every downstream pass unchanged.
        {
            auto slash = input_file.find_last_of('/');
            std::string base_dir = (slash == std::string::npos)
                ? std::string(".") : input_file.substr(0, slash);
            if (!rpgc::expand_copy_statements(src, base_dir)) {
                return 1;
            }
        }
        // D3: Auto Report Option Specifications ('U' form type) aren't
        // expanded by this compiler; fail loudly now rather than silently
        // compiling a program with no real output specs (see uspec.h).
        rpgc::reject_uspecs(src);

        rpgc::Program prog;
        prog.hspec      = rpgc::parse_hspec(src);
        prog.files      = rpgc::parse_fspecs(src);
        prog.line_counters = rpgc::parse_lspecs(src);
        rpgc::ISpecs is = rpgc::parse_ispecs(src);
        prog.in_records = std::move(is.records);
        prog.in_fields  = std::move(is.fields);
        prog.lookahead_fields = std::move(is.lookahead_fields);
        prog.data_structures = std::move(is.data_structures);
        prog.ds_subfields    = std::move(is.ds_subfields);
        prog.calcs      = rpgc::parse_cspecs(src);
        prog.outputs    = rpgc::parse_ospecs(src);
        prog.arrays     = rpgc::parse_especs(src);
        rpgc::load_compile_time_data(src, prog.arrays);
        prog.param_lists = rpgc::group_param_lists(prog.calcs);
        prog.exit_decls  = rpgc::group_exit_decls(prog.calcs);

        if (opts.verbose) {
            std::cerr << "rpgc: parsed " << input_file << ": "
                      << prog.files.size()     << " F-spec(s), "
                      << prog.in_fields.size() << " I-field(s), "
                      << prog.calcs.size()     << " C-spec(s), "
                      << prog.outputs.size()   << " O-record(s), "
                      << prog.arrays.size()    << " E-array(s)\n";
        }

        if (rpgc::error_count() > 0) {
            std::cerr << rpgc::error_count() << " error(s)\n";
            return 1;
        }

        auto mod = rpgc::generate_module(prog, input_file, ctx, is_top_level);
        if (rpgc::error_count() > 0 || !mod) {
            std::cerr << rpgc::error_count() << " error(s)\n";
            return 1;
        }
        mods.push_back(std::move(mod));
    }

    // The overwhelmingly common case -- one input file -- uses that module
    // directly, so a single-file build is byte-for-byte identical to before
    // multi-file support existed. Only a multi-program build (2+ files) pays
    // for llvm::Linker merging every program into one module.
    std::unique_ptr<llvm::Module> linked_mod;
    if (mods.size() == 1) {
        linked_mod = std::move(mods[0]);
    } else {
        linked_mod = std::make_unique<llvm::Module>(opts.input_files[0], ctx);
        llvm::Linker linker(*linked_mod);
        for (size_t fi = 0; fi < mods.size(); ++fi) {
            if (linker.linkInModule(std::move(mods[fi]))) {
                rpgc::fatal("failed to link '" + opts.input_files[fi] +
                            "' with the other compiled program(s) (duplicate "
                            "program-id or symbol?)");
                return 1;
            }
        }
    }

    llvm::Module *mod = linked_mod.get();
    mod->setTargetTriple(llvm::sys::getDefaultTargetTriple());

    if (opts.verbose) {
        std::cout << "rpgc: " << opts.input_files[0]
                  << " -> " << opts.emit << " : " << opts.output_file << "\n";
    }

    rpgc::DriverOptions drv;
    drv.clang_path  = opts.clang_path;
    drv.llc_path    = opts.llc_path;
    drv.runtime_lib = opts.runtime_lib;
    drv.emit        = opts.emit;
    drv.output_file = opts.output_file;
    drv.verbose     = opts.verbose;
    drv.save_temps  = opts.save_temps;
    drv.opt_level   = opts.opt_level;

    if (!rpgc::drive(*mod, drv)) {
        rpgc::error("backend failed");
        return 1;
    }

    if (opts.verbose) {
        std::cerr << "rpgc: wrote " << opts.output_file << "\n";
    }
    return 0;
}
