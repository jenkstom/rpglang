/* duplicate -- clone / copy-paste detector across many files (TOOLS_IDEAS.md §8.7).
 * v1 similarity signal: cosine similarity of opcode-frequency vectors, plus
 * an always-reported exact signal (identically-named subroutines). */
#include "cmds.h"
#include "../ir.h"
#include "../util.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace analyze {

namespace {

double cosine_similarity(const std::map<std::string, int> &a, const std::map<std::string, int> &b) {
    double dot = 0, na = 0, nb = 0;
    for (auto &kv : a) {
        na += (double)kv.second * kv.second;
        auto it = b.find(kv.first);
        if (it != b.end()) dot += (double)kv.second * it->second;
    }
    for (auto &kv : b) nb += (double)kv.second * kv.second;
    if (na == 0 || nb == 0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

} // namespace

int cmd_duplicate(const std::vector<std::string> &files, double threshold, std::ostream &out) {
    struct Prog { std::string id, path; std::map<std::string, int> hist; std::vector<std::string> subs; };
    std::vector<Prog> progs;
    int worst = 0;

    for (auto &path : files) {
        ProgramIR ir = ProgramIR::build(path);
        if (ir.load_failed) {
            out << "rpg-analyze: cannot open file: " << path << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        Prog p;
        p.id = ir.program_id;
        p.path = path;
        for (auto &c : ir.prog.calcs)
            if (!c.op_text.empty()) p.hist[upper_str(c.op_text)]++;
        for (auto &sr : ir.subroutines) p.subs.push_back(sr.name);
        progs.push_back(std::move(p));
    }

    bool any = false;
    for (size_t i = 0; i < progs.size(); ++i) {
        for (size_t j = i + 1; j < progs.size(); ++j) {
            double sim = cosine_similarity(progs[i].hist, progs[j].hist);
            std::vector<std::string> shared_subs;
            for (auto &s : progs[i].subs)
                if (std::find(progs[j].subs.begin(), progs[j].subs.end(), s) != progs[j].subs.end())
                    shared_subs.push_back(s);

            if (sim < threshold && shared_subs.empty()) continue;
            any = true;
            out << progs[i].path << " <-> " << progs[j].path << "  similarity=" << sim << "\n";
            if (!shared_subs.empty()) {
                out << "  shared subroutine names:";
                for (auto &s : shared_subs) out << " " << s;
                out << "\n";
            }
        }
    }
    if (!any) out << "(no similar programs found at threshold " << threshold << ")\n";

    return worst;
}

} // namespace analyze
