/* subroutines -- intra-program subroutine map (TOOLS_IDEAS.md §4.7). */
#include "modules.h"
#include "../util.h"

#include <algorithm>
#include <set>

namespace analyze {

namespace {

int subroutine_complexity(const ProgramIR &ir, const Subroutine &sr) {
    int score = 1;
    for (int i = sr.begin_idx + 1; i < sr.end_idx && i < (int)ir.prog.calcs.size(); ++i) {
        auto op = ir.prog.calcs[i].op;
        if (op == rpgc::Op::IF || op == rpgc::Op::DOW || op == rpgc::Op::DOU || op == rpgc::Op::CAS)
            ++score;
    }
    return score;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "subroutines";
    r.title = "Subroutine map";

    // Who calls whom, so DEAD-SUBR can check "never invoked".
    std::set<std::string> called;
    for (auto &sr : ir.subroutines)
        for (auto &c : sr.calls) called.insert(c);
    for (auto &c : ir.prog.calcs) {
        // Calls from the main body (outside any subroutine) also count.
        bool in_any_sub = false;
        for (auto &sr : ir.subroutines) {
            int idx = int(&c - &ir.prog.calcs[0]);
            if (idx > sr.begin_idx && idx < sr.end_idx) { in_any_sub = true; break; }
        }
        if (!in_any_sub) {
            if (c.op == rpgc::Op::EXSR && !c.factor2.empty()) called.insert(upper_str(c.factor2));
            else if (c.op == rpgc::Op::CAS && !c.result.empty()) called.insert(upper_str(c.result));
        }
    }

    std::vector<std::vector<std::string>> rows;
    Json arr = Json::array();

    for (auto &sr : ir.subroutines) {
        std::set<std::string> uniq_tested(sr.indicators_tested.begin(), sr.indicators_tested.end());
        std::set<std::string> uniq_set(sr.indicators_set.begin(), sr.indicators_set.end());
        std::set<std::string> uniq_written(sr.fields_written.begin(), sr.fields_written.end());
        int cplx = subroutine_complexity(ir, sr);

        rows.push_back({sr.name, std::to_string(sr.begin_line), std::to_string(sr.end_line),
                        std::to_string(sr.calls.size()), std::to_string(uniq_tested.size()),
                        std::to_string(uniq_set.size()), std::to_string(uniq_written.size()),
                        std::to_string(cplx)});

        Json j = Json::object();
        j.set("name", sr.name);
        j.set("begin_line", sr.begin_line);
        j.set("end_line", sr.end_line);
        Json calls = Json::array();
        for (auto &c : sr.calls) calls.push_back(Json(c));
        j.set("calls", calls);
        Json tested = Json::array();
        for (auto &t : uniq_tested) tested.push_back(Json(t));
        j.set("indicators_tested", tested);
        Json setj = Json::array();
        for (auto &s : uniq_set) setj.push_back(Json(s));
        j.set("indicators_set", setj);
        Json written = Json::array();
        for (auto &w : uniq_written) written.push_back(Json(w));
        j.set("fields_written", written);
        j.set("complexity", cplx);
        arr.push_back(j);

        if (sr.end_line == 0) {
            Finding f;
            f.id = "SUBR-NOENDSR";
            f.severity = Severity::Error;
            f.module = "subroutines";
            f.message = "subroutine '" + sr.name + "' has no ENDSR";
            f.file = ir.path;
            f.line = sr.begin_line;
            f.spec = 'C';
            r.findings.push_back(f);
        }
        if (called.find(sr.name) == called.end()) {
            Finding f;
            f.id = "DEAD-SUBR";
            f.severity = Severity::Info;
            f.module = "subroutines";
            f.message = "subroutine '" + sr.name + "' is never invoked by EXSR/CASxx";
            f.file = ir.path;
            f.line = sr.begin_line;
            f.spec = 'C';
            r.findings.push_back(f);
        }
    }

    // SUBR-COMMCOUPLING: subroutine A sets indicator X, subroutine B tests X,
    // with no direct call edge between A and B in either direction.
    std::set<std::pair<std::string, std::string>> reported;
    for (auto &a : ir.subroutines) {
        for (auto &lbl : a.indicators_set) {
            for (auto &b : ir.subroutines) {
                if (a.name == b.name) continue;
                bool tests = std::find(b.indicators_tested.begin(), b.indicators_tested.end(), lbl) !=
                             b.indicators_tested.end();
                if (!tests) continue;
                bool a_calls_b = std::find(a.calls.begin(), a.calls.end(), b.name) != a.calls.end();
                bool b_calls_a = std::find(b.calls.begin(), b.calls.end(), a.name) != b.calls.end();
                if (a_calls_b || b_calls_a) continue;
                auto key = a.name < b.name ? std::make_pair(a.name, b.name) : std::make_pair(b.name, a.name);
                if (!reported.insert(key).second) continue;
                Finding f;
                f.id = "SUBR-COMMCOUPLING";
                f.severity = Severity::Warn;
                f.module = "subroutines";
                f.message = "subroutines '" + a.name + "' and '" + b.name +
                            "' communicate only via indicator " + lbl;
                f.file = ir.path;
                f.line = a.begin_line;
                f.spec = 'C';
                r.findings.push_back(f);
            }
        }
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "subroutines";
        sec.title = "Subroutine map";
        sec.text_lines = render_table(
            {"NAME", "BEGIN", "END", "CALLS", "IND-TEST", "IND-SET", "FIELDS-W", "CPLX"}, rows);
        sec.data.set("subroutines", arr);
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_subroutines_module() {
    return ModuleInfo{"subroutines", "Subroutine map", "CONTROL FLOW", run};
}

} // namespace analyze
