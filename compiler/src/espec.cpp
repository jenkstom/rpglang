// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * espec.cpp -- parse Extension Specifications and compile-time array data.
 * ========================================================================== */
#include "espec.h"
#include "diagnostics.h"

#include <algorithm>
#include <cctype>

namespace rpgc {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}
} // namespace

bool is_table_name(const std::string &name) {
    return name.size() >= 3 &&
           std::toupper((unsigned char)name[0]) == 'T' &&
           std::toupper((unsigned char)name[1]) == 'A' &&
           std::toupper((unsigned char)name[2]) == 'B';
}

std::vector<ESpecArray> parse_especs(const std::vector<SourceLine> &src) {
    std::vector<ESpecArray> out;
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'E') continue;

        ESpecArray a;
        a.lineno = sl.lineno;
        a.name   = col_trim(sl.text, 27, 32);
        if (a.name.empty()) continue;

        std::string ne = col_trim(sl.text, 36, 39);
        if (!ne.empty()) { try { a.entries = std::stoi(ne); } catch (...) {} }
        std::string el = col_trim(sl.text, 40, 42);
        if (!el.empty()) { try { a.entry_len = std::stoi(el); } catch (...) {} }

        // E7: col 43, packed/binary prerun-time array/table data (manual
        // 80373-80378). Blank/other = zoned ASCII (the only format handled
        // before this).
        std::string fmt43 = upper(col_trim(sl.text, 43, 43));
        if (fmt43 == "P" || fmt43 == "B") a.data_format = fmt43[0];

        std::string dec = col_trim(sl.text, 44, 44);
        if (!dec.empty() && std::isdigit((unsigned char)dec[0]))
            a.decimals = dec[0] - '0';

        std::string seq = upper(col_trim(sl.text, 45, 45));
        a.ascending = (seq == "A");

        a.from_file = col_trim(sl.text, 11, 18);
        std::string perRec = col_trim(sl.text, 33, 35);
        if (a.from_file.empty() && perRec.empty())
            a.load = ArrayLoad::RunTime;
        else
            a.load = a.from_file.empty() ? ArrayLoad::CompileTime
                                         : ArrayLoad::PreRunTime;

        // Alternating partner (cols 46-57). Only meaningful when a name is
        // present in 46-51; the remaining sub-columns mirror 40/44/45.
        a.alt_name = col_trim(sl.text, 46, 51);
        if (!a.alt_name.empty()) {
            std::string ael = col_trim(sl.text, 52, 54);
            if (!ael.empty()) { try { a.alt_entry_len = std::stoi(ael); }
                                 catch (...) {} }
            // E7: col 55 mirrors col 43 for the alternating partner.
            std::string afmt = upper(col_trim(sl.text, 55, 55));
            if (afmt == "P" || afmt == "B") a.alt_data_format = afmt[0];
            std::string adec = col_trim(sl.text, 56, 56);
            if (!adec.empty() && std::isdigit((unsigned char)adec[0]))
                a.alt_decimals = adec[0] - '0';
            std::string aseq = upper(col_trim(sl.text, 57, 57));
            a.alt_ascending = (aseq == "A");
        }

        out.push_back(std::move(a));
    }
    return out;
}

void load_compile_time_data(const std::vector<SourceLine> &src,
                            std::vector<ESpecArray> &arrays) {
    // Find the first "** " line (cols 1-3): it marks the start of compile-time
    // data. Each subsequent "** " introduces the next compile-time array.
    size_t i = 0;
    for (; i < src.size(); ++i) {
        const auto &t = src[i].text;
        if (t.size() >= 3 && t[0] == '*' && t[1] == '*' && t[2] == ' ')
            break;
    }
    if (i >= src.size()) return;

    // Walk the compile-time arrays in declaration order (those marked
    // CompileTime). Each is introduced by a "** " line and followed by data
    // lines until the next "** " or end of source.
    size_t ai = 0;
    auto next_ct = [&]() -> ESpecArray * {
        while (ai < arrays.size()) {
            if (arrays[ai].load == ArrayLoad::CompileTime) return &arrays[ai++];
            ++ai;
        }
        return nullptr;
    };

    // The line at `i` is the first "** ". Skip it and assign data to the first
    // compile-time array.
    auto notspace = [](unsigned char ch){ return !std::isspace(ch); };
    // Pull one fixed-width numeric field out of `t` at byte `pos` (width w),
    // appending the decoded value to `out`. Returns the new position, or -1 if
    // the field no longer fits on the line.
    auto take_field = [&](const std::string &t, int pos, int w,
                          std::vector<long> &out) -> int {
        while (pos < (int)t.size() && std::isspace((unsigned char)t[pos])) ++pos;
        if (w < 1) w = 1;
        if (pos + w > (int)t.size()) return -1;
        std::string field = t.substr(pos, w);
        auto a = std::find_if(field.begin(), field.end(), notspace);
        auto b = std::find_if(field.rbegin(), field.rend(), notspace).base();
        if (a < b) {
            try { out.push_back(std::stol(std::string(a, b))); }
            catch (...) { out.push_back(0); }
        } else {
            out.push_back(0);
        }
        return pos + w;
    };
    // Pull one fixed-width alphanumeric field (A9): unlike take_field this
    // keeps the raw bytes verbatim (no numeric parse), matching the manual's
    // own headline array/table example, a month-name table (Ch. 20).
    auto take_str = [&](const std::string &t, int pos, int w,
                        std::vector<std::string> &out) -> int {
        while (pos < (int)t.size() && std::isspace((unsigned char)t[pos])) ++pos;
        if (w < 1) w = 1;
        if (pos + w > (int)t.size()) return -1;
        out.push_back(t.substr(pos, w));
        return pos + w;
    };

    ++i;
    ESpecArray *cur = next_ct();
    for (; i < src.size(); ++i) {
        const auto &t = src[i].text;
        if (t.size() >= 3 && t[0] == '*' && t[1] == '*' && t[2] == ' ') {
            cur = next_ct();
            continue;
        }
        if (!cur) break;

        if (cur->decimals < 0) {
            // Alphanumeric compile-time array/table (A9).
            int wA = cur->entry_len > 0 ? cur->entry_len : 1;
            bool altAlpha = !cur->alt_name.empty() && cur->alt_decimals < 0;
            int wB = altAlpha ? (cur->alt_entry_len > 0 ? cur->alt_entry_len : 1) : 0;
            int pos = 0;
            while (pos >= 0 &&
                   (int)cur->init_str.size() < cur->entries &&
                   (!altAlpha || (int)cur->alt_init_str.size() < cur->entries)) {
                pos = take_str(t, pos, wA, cur->init_str);
                if (altAlpha && pos >= 0)
                    pos = take_str(t, pos, wB, cur->alt_init_str);
            }
            continue;
        }

        int wA = cur->entry_len > 0 ? cur->entry_len : 1;
        bool alt = !cur->alt_name.empty() && cur->alt_decimals >= 0;
        int wB = alt ? (cur->alt_entry_len > 0 ? cur->alt_entry_len : 1) : 0;

        // Parse the data line as fixed-width numeric fields. For alternating
        // arrays/tables the partner's element follows each primary element on
        // the same record: A1 B1 A2 B2 ...
        int pos = 0;
        while (pos >= 0 &&
               (int)cur->init_data.size() < cur->entries &&
               (!alt || (int)cur->alt_init_data.size() < cur->entries)) {
            pos = take_field(t, pos, wA, cur->init_data);
            if (alt && (int)cur->init_data.size() <= cur->entries)
                pos = take_field(t, pos, wB, cur->alt_init_data);
        }
    }

    // Pad each compile-time array to its declared size (zeros for numeric,
    // blanks for alphanumeric -- A9).
    for (auto &a : arrays) {
        if (a.load != ArrayLoad::CompileTime) continue;
        if (a.decimals < 0) {
            int wA = a.entry_len > 0 ? a.entry_len : 1;
            while ((int)a.init_str.size() < a.entries)
                a.init_str.push_back(std::string((size_t)wA, ' '));
            if (!a.alt_name.empty() && a.alt_decimals < 0) {
                int wB = a.alt_entry_len > 0 ? a.alt_entry_len : 1;
                while ((int)a.alt_init_str.size() < a.entries)
                    a.alt_init_str.push_back(std::string((size_t)wB, ' '));
            }
            continue;
        }
        while ((int)a.init_data.size() < a.entries) a.init_data.push_back(0);
        if (!a.alt_name.empty())
            while ((int)a.alt_init_data.size() < a.entries)
                a.alt_init_data.push_back(0);
    }
}

} // namespace rpgc
