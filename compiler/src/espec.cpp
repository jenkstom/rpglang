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
    ++i;
    ESpecArray *cur = next_ct();
    for (; i < src.size(); ++i) {
        const auto &t = src[i].text;
        if (t.size() >= 3 && t[0] == '*' && t[1] == '*' && t[2] == ' ') {
            cur = next_ct();
            continue;
        }
        if (!cur) break;
        if (cur->decimals < 0) continue;   // alphanumeric arrays: deferred

        // Parse the data line as a sequence of fixed-width numeric fields.
        // Element width = entry_len. Entries are packed across lines.
        int w = cur->entry_len > 0 ? cur->entry_len : 1;
        for (int pos = 0; pos < (int)t.size() && (int)cur->init_data.size() < cur->entries; ) {
            // skip leading blanks
            while (pos < (int)t.size() && std::isspace((unsigned char)t[pos])) ++pos;
            if (pos + w > (int)t.size()) break;
            std::string field = t.substr(pos, w);
            // trim
            auto notspace = [](unsigned char ch){ return !std::isspace(ch); };
            auto a = std::find_if(field.begin(), field.end(), notspace);
            auto b = std::find_if(field.rbegin(), field.rend(), notspace).base();
            if (a < b) {
                std::string num(a, b);
                try { cur->init_data.push_back(std::stol(num)); }
                catch (...) {}
            } else {
                cur->init_data.push_back(0);
            }
            pos += w;
        }
    }

    // Pad each compile-time array to its declared size with zeros.
    for (auto &a : arrays) {
        if (a.load != ArrayLoad::CompileTime) continue;
        while ((int)a.init_data.size() < a.entries) a.init_data.push_back(0);
    }
}

} // namespace rpgc
