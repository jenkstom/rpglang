/* ========================================================================== *
 * source.cpp -- implementation.
 * ========================================================================== */
#include "source.h"
#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace rpgc {

bool load_source(const std::string &path, std::vector<SourceLine> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    std::string raw;
    int lineno = 0;
    while (std::getline(f, raw)) {
        ++lineno;
        // Drop a trailing CR (CRLF files).
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        SourceLine sl;
        sl.text   = raw;
        sl.lineno = lineno;

        // A comment line: column 6 is a spec letter OR blank/sequence area, and
        // column 7 is '*'. We only treat lines that have an actual spec letter
        // in col 6 with a '*' in col 7 as comments, plus the common "* in col 7
        // of any line" convention.
        if ((int)raw.size() >= 7 && raw[6] == '*') {
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

/* Resolve a /COPY member name to a file under `base_dir`: the member itself,
 * then MEMBER.rpg, then MEMBER.cpy. Returns "" if none exist. */
std::string resolve_copy_member(const std::string &base_dir,
                                const std::string &member) {
    namespace fs = std::filesystem;
    for (const char *suffix : {"", ".rpg", ".cpy"}) {
        fs::path p = fs::path(base_dir) / (member + suffix);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return "";
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

} // namespace rpgc
