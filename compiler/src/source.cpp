/* ========================================================================== *
 * source.cpp -- implementation.
 * ========================================================================== */
#include "source.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

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

} // namespace rpgc
