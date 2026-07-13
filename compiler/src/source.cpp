// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * source.cpp -- implementation.
 * ========================================================================== */
#include "source.h"
#include "clean.h"
#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace rpgc {

/* Read an entire file as raw bytes. Returns false on open failure (matching
 * the previous std::ifstream behavior). Used so load_source can hand the whole
 * file to clean_source_bytes before line-splitting -- cleaning at the byte
 * level is what lets us repair fixed-80-no-newline files, NUL padding, EBCDIC,
 * and embedded separator sequences, none of which survive std::getline. */
static bool slurp_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool load_source(const std::string &path, std::vector<SourceLine> &out) {
    std::string bytes;
    if (!slurp_file(path, bytes)) return false;

    // Run the staged cleanup pipeline in place. Each stage detects whether its
    // issue is present and only applies if so, so already-clean files are left
    // byte-identical (no transformation fires). What each stage did is surfaced
    // as a diagnostic warning so users see when the compiler has repaired
    // their input -- and so anything suspicious is flagged.
    CleanReport crep = clean_source_bytes(bytes);
    for (const auto &note : crep.notes)
        report(path, 0, 0,
               crep.suspicious ? DiagKind::Error : DiagKind::Warning,
               "source cleanup: " + note);

    // `bytes` is now clean ASCII text, LF-terminated. Split into SourceLines,
    // keeping the previous CR-strip and col-7 comment semantics. We split on
    // the in-memory string (not std::getline on the stream) because cleaning
    // may have introduced/removed bytes; this keeps source positions exact.
    int lineno = 0;
    size_t pos = 0;
    while (pos < bytes.size()) {
        size_t nl = bytes.find('\n', pos);
        std::string raw = (nl == std::string::npos)
                              ? bytes.substr(pos)
                              : bytes.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? bytes.size() : nl + 1;
        ++lineno;
        // Drop a trailing CR (in case any CRLF survived; cleanup normally
        // normalizes these, but the strip is harmless and keeps semantics
        // identical to the old std::getline path).
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        SourceLine sl;
        sl.text   = std::move(raw);
        sl.lineno = lineno;
        // A comment line: column 7 is '*'. (Column 6 is the spec letter or the
        // sequence-number area; '*' in col 7 marks the line as a comment.)
        if ((int)sl.text.size() >= 7 && sl.text[6] == '*') {
            sl.comment = true;
        }
        out.push_back(std::move(sl));
    }
    return true;
}

std::string col(const std::string &line, int first, int last) {
    // Convert from 1-based inclusive to 0-based half-open.
    int begin = first - 1;
    int end   = last;            // std::string::substr takes (pos, count)
    if (begin < 0) begin = 0;
    if (end > (int)line.size()) end = (int)line.size();
    if (end < begin) return std::string();
    return line.substr(begin, (std::size_t)(end - begin));
}

std::string col_trim(const std::string &line, int first, int last) {
    std::string s = col(line, first, last);
    auto notspace = [](unsigned char c){ return !std::isspace(c); };
    auto a = std::find_if(s.begin(), s.end(), notspace);
    auto b = std::find_if(s.rbegin(), s.rend(), notspace).base();
    return (a < b) ? std::string(a, b) : std::string();
}

char form_type(const SourceLine &l) {
    if (l.text.size() < 6) return ' ';
    char c = l.text[5];          // column 6, 0-based index 5
    return (unsigned char)std::toupper((unsigned char)c);
}

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

/* Resolve `name` to a file under `base_dir`, trying each suffix in order.
 * Returns "" if none exist. Shared by /COPY member lookup and the FMTS
 * display-file lookup (source.h's resolve_display_file). */
std::string resolve_named_file(const std::string &base_dir, const std::string &name,
                               std::initializer_list<const char *> suffixes) {
    namespace fs = std::filesystem;
    for (const char *suffix : suffixes) {
        fs::path p = fs::path(base_dir) / (name + suffix);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return "";
}

/* Resolve a /COPY member name to a file under `base_dir`: the member itself,
 * then MEMBER.rpg, then MEMBER.cpy. Returns "" if none exist. */
std::string resolve_copy_member(const std::string &base_dir,
                                const std::string &member) {
    return resolve_named_file(base_dir, member, {"", ".rpg", ".cpy"});
}

/* Recursively expand /COPY lines into `out`. `chain` guards against a member
 * (directly or transitively) copying itself. Returns false on the first
 * unresolvable member or copy cycle (a diagnostic has already been reported). */
bool expand_into(const std::vector<SourceLine> &lines, const std::string &base_dir,
                 std::unordered_set<std::string> &chain,
                 std::vector<SourceLine> &out) {
    for (const auto &sl : lines) {
        if (!sl.comment && upper(col(sl.text, 7, 11)) == "/COPY") {
            std::string args = col_trim(sl.text, 13, 29);
            std::string lib, member;
            auto comma = args.find(',');
            if (comma == std::string::npos) {
                member = args;
            } else {
                lib = args.substr(0, comma);
                member = args.substr(comma + 1);
            }
            // trim
            auto trim = [](std::string s) {
                auto notspace = [](unsigned char c){ return !std::isspace(c); };
                auto a = std::find_if(s.begin(), s.end(), notspace);
                auto b = std::find_if(s.rbegin(), s.rend(), notspace).base();
                return (a < b) ? std::string(a, b) : std::string();
            };
            member = trim(member);
            if (member.empty()) {
                report("input", sl.lineno, 13, DiagKind::Error,
                       "/COPY: no member name in cols 13-29");
                return false;
            }
            std::string path = resolve_copy_member(base_dir, member);
            if (path.empty()) {
                report("input", sl.lineno, 13, DiagKind::Error,
                       "/COPY " + (lib.empty() ? member : lib + "," + member) +
                       ": cannot find a copy member named '" + member +
                       "' (looked for " + member + ", " + member + ".rpg, " +
                       member + ".cpy in " +
                       (base_dir.empty() ? std::string(".") : base_dir) + ")");
                return false;
            }
            std::string canon = std::filesystem::weakly_canonical(path).string();
            if (!chain.insert(canon).second) {
                report("input", sl.lineno, 13, DiagKind::Error,
                       "/COPY " + member + ": copy cycle detected (member "
                       "copies itself, directly or transitively)");
                return false;
            }
            std::vector<SourceLine> member_src;
            if (!load_source(path, member_src)) {
                report("input", sl.lineno, 13, DiagKind::Error,
                       "/COPY " + member + ": cannot read '" + path + "'");
                chain.erase(canon);
                return false;
            }
            bool ok = expand_into(member_src, base_dir, chain, out);
            chain.erase(canon);
            if (!ok) return false;
            continue;   // the /COPY directive line itself is not a spec line
        }
        out.push_back(sl);
    }
    return true;
}

} // namespace

bool expand_copy_statements(std::vector<SourceLine> &src,
                            const std::string &base_dir) {
    // Fast path: skip the copy entirely if nothing looks like a /COPY line.
    bool any = false;
    for (const auto &sl : src)
        if (!sl.comment && upper(col(sl.text, 7, 11)) == "/COPY") { any = true; break; }
    if (!any) return true;

    std::vector<SourceLine> out;
    std::unordered_set<std::string> chain;
    if (!expand_into(src, base_dir, chain, out)) return false;
    src = std::move(out);
    return true;
}

std::string resolve_display_file(const std::string &base_dir, const std::string &name) {
    return resolve_named_file(base_dir, name, {"", ".dspf"});
}

} // namespace rpgc
