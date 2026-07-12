// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

#include "finding.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace analyze {

const char *severity_text(Severity s) {
    switch (s) {
        case Severity::Error: return "ERROR";
        case Severity::Warn:  return "WARN";
        case Severity::Info:  return "INFO";
    }
    return "INFO";
}

bool parse_severity(const std::string &s, Severity &out) {
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (u == "error") { out = Severity::Error; return true; }
    if (u == "warn" || u == "warning") { out = Severity::Warn; return true; }
    if (u == "info") { out = Severity::Info; return true; }
    return false;
}

namespace {
int sev_rank(Severity s) {
    switch (s) {
        case Severity::Error: return 0;
        case Severity::Warn:  return 1;
        case Severity::Info:  return 2;
    }
    return 3;
}
std::string key_of(const Finding &f) {
    return f.id + "\x1f" + f.file + "\x1f" + std::to_string(f.line);
}
} // namespace

void dedup_and_sort(std::vector<Finding> &findings) {
    std::unordered_map<std::string, size_t> first_index;
    std::vector<Finding> merged;
    merged.reserve(findings.size());
    for (auto &f : findings) {
        std::string k = key_of(f);
        auto it = first_index.find(k);
        if (it == first_index.end()) {
            first_index[k] = merged.size();
            merged.push_back(f);
        } else {
            auto &dst = merged[it->second];
            for (auto &e : f.evidence) dst.evidence.push_back(e);
        }
    }
    std::stable_sort(merged.begin(), merged.end(), [](const Finding &a, const Finding &b) {
        if (sev_rank(a.severity) != sev_rank(b.severity)) return sev_rank(a.severity) < sev_rank(b.severity);
        if (a.file != b.file) return a.file < b.file;
        if (a.line != b.line) return a.line < b.line;
        return a.id < b.id;
    });
    findings = std::move(merged);
}

std::vector<Finding> filter_severity(const std::vector<Finding> &findings, Severity min) {
    std::vector<Finding> out;
    for (auto &f : findings) {
        if (sev_rank(f.severity) <= sev_rank(min)) out.push_back(f);
    }
    return out;
}

FindingCounts count_by_severity(const std::vector<Finding> &findings) {
    FindingCounts c;
    for (auto &f : findings) {
        switch (f.severity) {
            case Severity::Error: ++c.errors; break;
            case Severity::Warn:  ++c.warnings; break;
            case Severity::Info:  ++c.infos; break;
        }
    }
    return c;
}

} // namespace analyze
