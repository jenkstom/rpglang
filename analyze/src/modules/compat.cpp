/* compat -- compiler-coverage checker (TOOLS_IDEAS.md §4.13).
 *
 * Runs against the same lenient IR the rest of the tool uses; unsupported
 * constructs never abort parsing (see ir.cpp / diagnostics.h's sink), so this
 * module can report "won't compile" on exactly the inputs that need it most.
 */
#include "modules.h"
#include "../util.h"

#include <functional>
#include <set>
#include <unordered_map>

namespace analyze {

namespace {

/* Mirrors cspec.cpp's parse_op() recognized mnemonics -- kept in sync by
 * hand since parse_op itself is private to that translation unit. */
bool is_known_opcode(const std::string &upperOp) {
    static const std::set<std::string> known = {
        "ADD", "Z-ADD", "ZADD", "Z-SUB", "ZSUB", "SETON", "SETOF", "COMP", "GOTO", "TAG",
        "MOVE", "MOVEL", "SUB", "MULT", "DIV", "MVR", "DO", "ELSE", "END", "EXSR", "BEGSR",
        "ENDSR", "EXCPT", "XFOOT", "SQRT", "LOKUP", "LOOKUP", "MOVEA", "TESTZ", "TESTB",
        "CHAIN", "SETLL", "READE", "READP", "READ", "BITON", "BITOF", "DEFN", "SORTA", "TIME",
        "MHHZO", "MHLZO", "MLHZO", "MLLZO",
    };
    if (known.count(upperOp)) return true;
    for (const char *prefix : {"IF", "DOW", "DOU", "CAS"}) {
        size_t plen = std::string(prefix).size();
        if (upperOp.rfind(prefix, 0) != 0) continue;
        std::string suffix = upperOp.substr(plen);
        static const std::set<std::string> cmp = {"", "EQ", "NE", "GT", "LT", "GE", "LE"};
        if (cmp.count(suffix)) return true;
    }
    return false;
}

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "compat";
    r.title = "Compiler-coverage checker";

    std::vector<std::string> notes;

    // COMPAT-OP: raw C-spec op text (cols 28-32) that doesn't parse.
    for (auto &sl : ir.raw_lines) {
        if (sl.comment || form_type(sl) != 'C') continue;
        std::string op = upper_str(rpgc::col_trim(sl.text, 28, 32));
        if (op.empty() || is_known_opcode(op)) continue;
        Finding f;
        f.id = "COMPAT-OP";
        f.severity = Severity::Error;
        f.module = "compat";
        f.message = "unsupported opcode '" + op + "'";
        f.file = ir.path;
        f.line = sl.lineno;
        f.spec = 'C';
        f.columns = "28-32";
        r.findings.push_back(f);
        notes.push_back(f.message + " (line " + std::to_string(sl.lineno) + ")");
    }

    // COMPAT-DEVICE / COMPAT-FEATURE (record-address files).
    for (auto &f : ir.prog.files) {
        if (f.device == rpgc::Device::Workstn || f.device == rpgc::Device::Special ||
            f.device == rpgc::Device::Console) {
            Finding fnd;
            fnd.id = "COMPAT-DEVICE";
            fnd.severity = Severity::Error;
            fnd.module = "compat";
            fnd.message = "file '" + f.name + "' uses unsupported device (WORKSTN/SPECIAL/CONSOLE)";
            fnd.file = ir.path;
            fnd.line = f.lineno;
            fnd.spec = 'F';
            r.findings.push_back(fnd);
            notes.push_back(fnd.message);
        }
        if (f.design == rpgc::FileDesign::RecordAddr) {
            Finding fnd;
            fnd.id = "COMPAT-FEATURE";
            fnd.severity = Severity::Error;
            fnd.module = "compat";
            fnd.message = "file '" + f.name + "' uses unsupported record-address designation";
            fnd.file = ir.path;
            fnd.line = f.lineno;
            fnd.spec = 'F';
            r.findings.push_back(fnd);
            notes.push_back(fnd.message);
        }
    }

    // COMPAT-FEATURE: Auto Report ('U' spec) source.
    for (auto &sl : ir.raw_lines) {
        if (form_type(sl) == 'U') {
            Finding fnd;
            fnd.id = "COMPAT-FEATURE";
            fnd.severity = Severity::Error;
            fnd.module = "compat";
            fnd.message = "Auto Report Option Specifications ('U' form type) are not supported";
            fnd.file = ir.path;
            fnd.line = sl.lineno;
            fnd.spec = 'U';
            r.findings.push_back(fnd);
            notes.push_back(fnd.message);
            break;
        }
    }

    // COMPAT-FEATURE: recursive subroutines (RPG II forbids recursion; this
    // compiler hard-errors at codegen time, mirrored here from the shared IR).
    {
        std::unordered_map<std::string, int> state; // 0 unseen, 1 on-stack, 2 done
        std::function<bool(const Subroutine &)> dfs = [&](const Subroutine &sr) -> bool {
            state[sr.name] = 1;
            for (auto &callee : sr.calls) {
                if (callee == sr.name) return true;
                auto it = ir.subroutine_index.find(callee);
                if (it == ir.subroutine_index.end()) continue;
                if (state[callee] == 1) return true;
                if (state[callee] == 0 && dfs(ir.subroutines[it->second])) return true;
            }
            state[sr.name] = 2;
            return false;
        };
        for (auto &sr : ir.subroutines) {
            if (state[sr.name] != 0) continue;
            if (dfs(sr)) {
                Finding fnd;
                fnd.id = "COMPAT-FEATURE";
                fnd.severity = Severity::Error;
                fnd.module = "compat";
                fnd.message = "subroutine '" + sr.name + "' participates in recursion (unsupported)";
                fnd.file = ir.path;
                fnd.line = sr.begin_line;
                fnd.spec = 'C';
                r.findings.push_back(fnd);
                notes.push_back(fnd.message);
            }
        }
    }

    // COMPAT-IND: raw indicator tokens (conditioning + resulting) that don't
    // resolve -- silently dropped by the strict parser (ir.h §7.1).
    auto check_token = [&](const std::string &raw, int lineno, const char *cols) {
        auto a = raw.find_first_not_of(" \t");
        std::string tok = a == std::string::npos ? std::string() : upper_str(raw.substr(a, raw.find_last_not_of(" \t") - a + 1));
        if (tok.empty() || tok == "L0") return;
        if (rpgc::parse_indicator_token(raw) != 0) return;
        Finding fnd;
        fnd.id = "COMPAT-IND";
        fnd.severity = Severity::Warn;
        fnd.module = "compat";
        fnd.message = "indicator token '" + tok + "' did not resolve and was silently dropped";
        fnd.file = ir.path;
        fnd.line = lineno;
        fnd.spec = 'C';
        fnd.columns = cols;
        r.findings.push_back(fnd);
        notes.push_back(fnd.message + " (line " + std::to_string(lineno) + ")");
    };
    for (auto &sl : ir.raw_lines) {
        if (sl.comment || form_type(sl) != 'C') continue;
        for (int start : {9, 12, 15}) {
            std::string grp = rpgc::col(sl.text, start, start + 2);
            if (grp.size() < 3) continue;
            bool neg = grp[0] == 'N' || grp[0] == 'n';
            check_token(grp.substr(neg ? 1 : 0, 2), sl.lineno, "9-17");
        }
        check_token(rpgc::col(sl.text, 54, 55), sl.lineno, "54-55");
        check_token(rpgc::col(sl.text, 56, 57), sl.lineno, "56-57");
        check_token(rpgc::col(sl.text, 58, 59), sl.lineno, "58-59");
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "compat";
        sec.title = "Compiler-coverage checker";
        if (notes.empty()) sec.text_lines.push_back("(no unsupported constructs detected)");
        else sec.text_lines = notes;
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_compat_module() {
    return ModuleInfo{"compat", "Compiler-coverage checker", "COMPILER COMPAT", run};
}

} // namespace analyze
