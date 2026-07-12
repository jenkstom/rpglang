/* format -- lint / canonicalize formatting, check-only (TOOLS_IDEAS.md §8.4).
 * Reports column-overflow, sequence errors, trailing whitespace, and
 * form-type misalignment. Never rewrites (see TOOLS_IDEAS.md §12: rewriting
 * risks corrupting column-sensitive fields). */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <cctype>

namespace analyze {

namespace {

bool is_known_form_type(char c) {
    static const std::string known = "HFICOELU";
    return known.find(c) != std::string::npos;
}

} // namespace

int cmd_format(const std::vector<std::string> &files, bool no_color, std::ostream &out) {
    const char *YELLOW = no_color ? "" : "\x1b[33m";
    const char *RESET = no_color ? "" : "\x1b[0m";
    int worst = 0;
    int total_issues = 0;

    for (auto &path : files) {
        std::vector<rpgc::SourceLine> src;
        if (!rpgc::load_source(path, src)) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }

        int prev_seq = -1;
        bool prev_seq_valid = false;
        int file_issues = 0;

        for (auto &sl : src) {
            std::vector<std::string> issues;

            if (sl.text.size() > 80)
                issues.push_back("column overflow (" + std::to_string(sl.text.size()) + " > 80)");

            if (!sl.text.empty()) {
                size_t last = sl.text.find_last_not_of(" \t");
                if (last != std::string::npos && last + 1 < sl.text.size())
                    issues.push_back("trailing whitespace");
            }

            if ((int)sl.text.size() >= 6 && !sl.comment) {
                char ft = sl.text[5];
                if (ft != ' ' && !is_known_form_type((char)std::toupper((unsigned char)ft)))
                    issues.push_back(std::string("unrecognized form type '") + ft + "' in column 6");
            }

            std::string seqtxt = rpgc::col_trim(sl.text, 1, 5);
            if (!seqtxt.empty()) {
                bool digits = true;
                for (char c : seqtxt) if (!std::isdigit((unsigned char)c)) { digits = false; break; }
                if (digits) {
                    int seq = std::stoi(seqtxt);
                    if (prev_seq_valid && seq < prev_seq)
                        issues.push_back("sequence number out of order (" + std::to_string(seq) +
                                         " after " + std::to_string(prev_seq) + ")");
                    prev_seq = seq;
                    prev_seq_valid = true;
                }
            }

            for (auto &issue : issues) {
                out << path << ":" << sl.lineno << ": " << YELLOW << issue << RESET << "\n";
                ++file_issues;
            }
        }
        total_issues += file_issues;
    }

    out << "\n" << total_issues << " formatting issue(s)\n";
    if (total_issues > 0) worst = std::max(worst, 1);
    return worst;
}

} // namespace analyze
