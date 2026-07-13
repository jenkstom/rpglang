// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * main.cpp -- rpg-clean CLI: repair "mangled" RPG source files.
 *
 * RPG source pulled off midrange systems (S/34, S/36, AS/400, IBM i) often
 * arrives as valid RPG *content* in a damaged *file*: EBCDIC bytes, no line
 * terminators (fixed-80 card images), terminators replaced by stray 5250/C1
 * control sequences from a botched transcode, trailing NUL padding, etc. This
 * tool detects each issue independently, repairs it, and warns loudly about
 * anything it can't confidently classify.
 *
 * It is the on-disk counterpart of the cleanup that `rpgc` and `rpg-analyze`
 * run automatically inside `load_source` -- both link rpgc_parse and call the
 * same `clean_source_bytes` pipeline, so the tool and the compiler agree on
 * exactly what "clean" means.
 *
 * rpg-clean NEVER modifies its input files. It works as a Unix filter: by
 * default the cleaned output goes to stdout (so it composes with pipes and
 * redirection), and the only way to get a file written is to name it
 * explicitly with -o/--output. To replace a file in place, redirect yourself:
 *
 *     rpg-clean prog.rpg > prog.rpg.new && mv prog.rpg.new prog.rpg
 *
 * (you choose whether to make that atomic with `mv` or not). This is the
 * opposite of a mutating tool: there is no footgun where running the tool on a
 * file damages the original.
 * ========================================================================== */
#include "clean.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *kVersion = "rpg-clean 0.1.0";

// GPLv3-mandated copyright/warranty notice (§5(a)). Printed by --version
// and --help.
const std::string kLicenseNotice =
    "Copyright (C) 2026 Tom White\n"
    "License GPLv3+: GNU GPL version 3 or later <https://www.gnu.org/licenses/gpl.html>\n"
    "This is free software: you are free to change and redistribute it.\n"
    "There is NO WARRANTY, to the extent permitted by law.\n";

struct Options {
    bool help = false;
    bool version = false;
    bool check = false;           // --check (read-only report, no output)
    bool report = false;          // --report (print per-stage notes)
    std::string output_file;      // -o (single input only); empty = stdout
    int width = rpgc::kDefaultCleanWidth;
    std::vector<std::string> files;
};

void print_help() {
    std::cout <<
        "rpg-clean -- repair mangled RPG source files (EBCDIC, 5250/C1 separators,\n"
        "           NUL padding, fixed-80-no-newline, line endings)\n"
        "\n"
        "USAGE:\n"
        "    rpg-clean [options] <file...>\n"
        "\n"
        "The same staged cleanup that rpgc and rpg-analyze run automatically inside\n"
        "load_source, applied to files on disk. Each stage (NUL strip, EBCDIC decode,\n"
        "separator repair, fixed-width split, line-ending normalize) detects whether\n"
        "its issue is present and only fires if so -- already-clean files pass through\n"
        "byte-identical. Anything the tool cannot confidently classify is reported as\n"
        "'suspicious' so a human can look.\n"
        "\n"
        "rpg-clean NEVER modifies its input files. By default it is a Unix filter:\n"
        "cleaned output goes to stdout (inputs concatenated). The only way to get a\n"
        "file written is to name one explicitly with -o. To replace a file in place,\n"
        "redirect yourself, e.g.:\n"
        "    rpg-clean prog.rpg > prog.rpg.new && mv prog.rpg.new prog.rpg\n"
        "\n"
        "MODES:\n"
        "    (default)            write cleaned output to stdout (filter)\n"
        "    -o, --output FILE    write one cleaned input to FILE (single input only)\n"
        "    --check              read-only: report what would change; write no output.\n"
        "                         Exit 1 if any input needs cleaning, 2 if it looks\n"
        "                         suspicious even after cleaning.\n"
        "\n"
        "OPTIONS:\n"
        "    -w, --width N        fixed-record width for the no-newline split stage\n"
        "                         (default 80)\n"
        "    --report             print a per-stage note for each input to stderr\n"
        "                         (what fired)\n"
        "    -h, --help           show this help\n"
        "    -V, --version        show version\n"
        "\n"
        "EXIT CODES:\n"
        "    0  all inputs processed (or --check found nothing to clean)\n"
        "    1  --check found one or more inputs needing cleaning\n"
        "    2  I/O error, bad CLI usage, or an input looked suspicious after cleaning\n"
        "\n";
    std::cout << kLicenseNotice;
}

/* Returns false + sets *usage_error on a bad flag/missing argument. Mirrors
 * the parse_args style in analyze/src/main.cpp. */
bool parse_args(int argc, char **argv, Options &o, std::string &usage_error) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_arg = [&](const char *flag) -> bool {
            if (i + 1 >= argc) { usage_error = std::string("missing argument to ") + flag; return false; }
            return true;
        };
        if (a == "-h" || a == "--help") { o.help = true; }
        else if (a == "-V" || a == "--version") { o.version = true; }
        else if (a == "--check") { o.check = true; }
        else if (a == "--report") { o.report = true; }
        else if (a == "-w" || a == "--width") {
            if (!need_arg("--width")) return false;
            try { o.width = std::stoi(argv[++i]); }
            catch (...) { usage_error = "bad --width value: " + std::string(argv[i]); return false; }
            if (o.width <= 0) { usage_error = "--width must be positive"; return false; }
        } else if (a == "-o" || a == "--output") {
            if (!need_arg(a.c_str())) return false;
            o.output_file = argv[++i];
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            usage_error = "unknown option: " + a;
            return false;
        } else {
            o.files.push_back(a);
        }
    }
    return true;
}

/* Slurp a file into a string. Returns false on open failure. */
bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string &path, const std::string &data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return f.good();
}

/* Write cleaned bytes to the chosen sink. In --check mode nothing is written;
 * with -o, to the named file; otherwise to stdout. `path` is the input path,
 * used only for status messages. */
void emit(const Options &o, const std::string &path, const std::string &data) {
    if (!o.output_file.empty()) {
        if (!write_file(o.output_file, data)) {
            std::cerr << "rpg-clean: cannot write " << o.output_file << "\n";
        } else {
            std::cerr << "rpg-clean: wrote " << o.output_file
                      << " (" << data.size() << " bytes)";
            if (!o.files.empty() && o.files[0] != o.output_file)
                std::cerr << " from " << path;
            std::cerr << "\n";
        }
    } else {
        std::cout.write(data.data(), (std::streamsize)data.size());
    }
}

int run(const Options &o) {
    int rc = 0;

    for (const auto &path : o.files) {
        std::string data;
        if (!read_file(path, data)) {
            std::cerr << "rpg-clean: cannot open file: " << path << "\n";
            rc = std::max(rc, 2);
            continue;
        }

        // --check mode: read-only. Report status and what would fire; write
        // no output. Exit 1 if cleaning would change anything, 2 if suspicious.
        if (o.check) {
            std::string copy = data;
            rpgc::CleanReport r = rpgc::clean_source_bytes(copy, o.width);
            if (!r.notes.empty()) {
                std::cout << "NEEDS CLEAN  " << path << "  (" << data.size() << " bytes)\n";
                rc = std::max(rc, 1);
            } else {
                std::cout << "ok           " << path << "\n";
            }
            if (r.suspicious) {
                std::cout << "SUSPICIOUS   " << path << "\n";
                rc = std::max(rc, 2);
            }
            if (o.report) {
                for (const auto &note : r.notes)
                    std::cout << "             " << note << "\n";
            }
            continue;
        }

        // Clean (in memory only -- the input file is never written to).
        rpgc::CleanReport rep = rpgc::clean_source_bytes(data, o.width);

        if (o.report) {
            for (const auto &note : rep.notes)
                std::cerr << path << ": " << note << "\n";
            if (rep.suspicious)
                std::cerr << path << ": suspicious -- output may need manual review\n";
        }
        // Suspicious output bumps the exit code even when writing, so scripts
        // can detect files that need a human.
        if (rep.suspicious) rc = std::max(rc, 2);

        emit(o, path, data);
    }
    return rc;
}

} // namespace

int main(int argc, char **argv) {
    Options o;
    std::string usage_error;
    if (!parse_args(argc, argv, o, usage_error)) {
        std::cerr << "rpg-clean: " << usage_error << "\n";
        std::cerr << "Try 'rpg-clean --help'.\n";
        return 2;
    }
    if (o.help) { print_help(); return 0; }
    if (o.version) {
        std::cout << kVersion << "\n" << kLicenseNotice;
        return 0;
    }
    if (o.files.empty()) {
        std::cerr << "rpg-clean: no input files\n";
        std::cerr << "Try 'rpg-clean --help'.\n";
        return 2;
    }
    // -o takes exactly one input.
    if (!o.output_file.empty() && o.files.size() > 1) {
        std::cerr << "rpg-clean: -o/--output takes exactly one input file "
                     "(got " << o.files.size() << ")\n";
        return 2;
    }
    if (o.check && !o.output_file.empty()) {
        std::cerr << "rpg-clean: --check cannot combine with -o/--output\n";
        return 2;
    }
    return run(o);
}
