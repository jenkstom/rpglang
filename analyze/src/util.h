// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * util.h -- small text-formatting helpers shared by the analysis modules.
 * ========================================================================== */
#ifndef RPGANALYZE_UTIL_H
#define RPGANALYZE_UTIL_H

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace analyze {

inline std::string upper_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

inline std::string pad_right(const std::string &s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

inline std::string pad_left(const std::string &s, size_t width) {
    if (s.size() >= width) return s;
    return std::string(width - s.size(), ' ') + s;
}

/* Renders a simple left-aligned column table: one row per entry, columns
 * separated by two spaces, each column padded to the widest cell in it. */
inline std::vector<std::string> render_table(const std::vector<std::string> &headers,
                                              const std::vector<std::vector<std::string>> &rows) {
    size_t ncols = headers.size();
    std::vector<size_t> width(ncols);
    for (size_t c = 0; c < ncols; ++c) width[c] = headers[c].size();
    for (auto &row : rows)
        for (size_t c = 0; c < ncols && c < row.size(); ++c)
            width[c] = std::max(width[c], row[c].size());

    std::vector<std::string> out;
    {
        std::string line;
        for (size_t c = 0; c < ncols; ++c) {
            line += pad_right(headers[c], width[c]);
            if (c + 1 < ncols) line += "  ";
        }
        out.push_back(line);
        std::string rule;
        for (size_t c = 0; c < ncols; ++c) {
            rule += std::string(width[c], '-');
            if (c + 1 < ncols) rule += "  ";
        }
        out.push_back(rule);
    }
    for (auto &row : rows) {
        std::string line;
        for (size_t c = 0; c < ncols; ++c) {
            std::string cell = c < row.size() ? row[c] : std::string();
            line += pad_right(cell, width[c]);
            if (c + 1 < ncols) line += "  ";
        }
        out.push_back(line);
    }
    return out;
}

} // namespace analyze

#endif // RPGANALYZE_UTIL_H
