/* complexity -- complexity metrics (TOOLS_IDEAS.md §4.10). */
#include "modules.h"
#include "../util.h"

#include <algorithm>
#include <map>

namespace analyze {

namespace {

bool is_decision(rpgc::Op op) {
    return op == rpgc::Op::IF || op == rpgc::Op::DOW || op == rpgc::Op::DOU || op == rpgc::Op::CAS;
}

/* Cyclomatic-style score over a half-open [begin,end) range of calcs. */
int score_range(const ProgramIR &ir, int begin, int end) {
    int score = 1;
    for (int i = begin; i < end && i < (int)ir.prog.calcs.size(); ++i)
        if (is_decision(ir.prog.calcs[i].op)) ++score;
    return score;
}

const char *tier_text(int score) {
    if (score < 10) return "low";
    if (score < 25) return "medium";
    if (score < 50) return "high";
    return "extreme";
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "complexity";
    r.title = "Complexity metrics";

    auto &calcs = ir.prog.calcs;
    std::map<std::string, int> histogram;
    int goto_count = 0, tag_count = 0;
    int depth = 0, max_depth = 0;
    long depth_sum = 0;

    for (auto &c : calcs) {
        if (!c.op_text.empty()) histogram[upper_str(c.op_text)]++;
        if (c.op == rpgc::Op::GOTO) ++goto_count;
        if (c.op == rpgc::Op::TAG) ++tag_count;
        if (c.op == rpgc::Op::IF || c.op == rpgc::Op::DOW || c.op == rpgc::Op::DOU || c.op == rpgc::Op::DO) {
            ++depth;
            max_depth = std::max(max_depth, depth);
        } else if (c.op == rpgc::Op::END) {
            if (depth > 0) --depth;
        }
        depth_sum += depth;
    }
    double mean_depth = calcs.empty() ? 0.0 : (double)depth_sum / (double)calcs.size();
    double goto_density = calcs.empty() ? 0.0 : (double)goto_count / (double)calcs.size();

    // Per-section (main body + each subroutine) cyclomatic score.
    struct SectionScore { std::string name; int line; int score; };
    std::vector<SectionScore> sections;
    {
        // Main body: every calc index not owned by any subroutine.
        std::vector<bool> in_sub(calcs.size(), false);
        for (auto &sr : ir.subroutines)
            for (int i = sr.begin_idx; i <= sr.end_idx && i < (int)calcs.size(); ++i) in_sub[i] = true;
        int main_score = 1;
        int main_line = calcs.empty() ? 0 : calcs.front().lineno;
        for (size_t i = 0; i < calcs.size(); ++i)
            if (!in_sub[i] && is_decision(calcs[i].op)) ++main_score;
        sections.push_back({"(main)", main_line, main_score});
    }
    for (auto &sr : ir.subroutines)
        sections.push_back({sr.name, sr.begin_line, score_range(ir, sr.begin_idx + 1, sr.end_idx)});

    int overall = (int)calcs.size() / 10 + max_depth * 2 + goto_count;

    if (!opts.quiet) {
        Section sec;
        sec.id = "complexity";
        sec.title = "Complexity metrics";
        sec.text_lines.push_back("Total C-spec count:       " + std::to_string(calcs.size()));
        sec.text_lines.push_back("Subroutine count:         " + std::to_string(ir.subroutines.size()));
        sec.text_lines.push_back("GOTO/TAG:                 " + std::to_string(goto_count) + " / " +
                                 std::to_string(tag_count) + "  (density " +
                                 std::to_string((int)(goto_density * 100)) + "%)");
        sec.text_lines.push_back("Structured nesting depth: max " + std::to_string(max_depth) +
                                 ", mean " + std::to_string(mean_depth));
        sec.text_lines.push_back("Overall complexity tier:  " + std::string(tier_text(overall)) +
                                 " (score " + std::to_string(overall) + ")");
        sec.text_lines.push_back("");
        sec.text_lines.push_back("Opcode frequency:");
        std::vector<std::pair<std::string, int>> hv(histogram.begin(), histogram.end());
        std::sort(hv.begin(), hv.end(), [](auto &a, auto &b) { return a.second > b.second; });
        for (auto &kv : hv) sec.text_lines.push_back("  " + pad_right(kv.first, 8) + std::to_string(kv.second));
        sec.text_lines.push_back("");
        sec.text_lines.push_back("Per-section cyclomatic score:");
        for (auto &s : sections)
            sec.text_lines.push_back("  " + pad_right(s.name, 12) + std::to_string(s.score));

        Json hist = Json::object();
        for (auto &kv : histogram) hist.set(kv.first, kv.second);
        Json secj = Json::array();
        for (auto &s : sections) {
            Json j = Json::object();
            j.set("name", s.name);
            j.set("line", s.line);
            j.set("score", s.score);
            secj.push_back(j);
        }
        sec.data.set("total_opcodes", (int)calcs.size());
        sec.data.set("subroutine_count", (int)ir.subroutines.size());
        sec.data.set("goto_count", goto_count);
        sec.data.set("tag_count", tag_count);
        sec.data.set("goto_density", goto_density);
        sec.data.set("max_nesting_depth", max_depth);
        sec.data.set("mean_nesting_depth", mean_depth);
        sec.data.set("overall_score", overall);
        sec.data.set("overall_tier", tier_text(overall));
        sec.data.set("opcode_histogram", hist);
        sec.data.set("sections", secj);
        r.sections.push_back(std::move(sec));
    }

    const int kSectionThreshold = 10;
    for (auto &s : sections) {
        if (s.score > kSectionThreshold) {
            Finding f;
            f.id = "CPLX-HIGH";
            f.severity = Severity::Warn;
            f.module = "complexity";
            f.message = "section '" + s.name + "' has a high cyclomatic score (" + std::to_string(s.score) + ")";
            f.file = ir.path;
            f.line = s.line;
            f.spec = 'C';
            r.findings.push_back(f);
        }
    }

    return r;
}

} // namespace

ModuleInfo make_complexity_module() {
    return ModuleInfo{"complexity", "Complexity metrics", "COMPLEXITY", run};
}

} // namespace analyze
