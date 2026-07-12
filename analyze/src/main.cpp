/* ========================================================================== *
 * main.cpp -- rpg-analyze CLI: arg parsing + dispatch (TOOLS_IDEAS.md §3).
 * ========================================================================== */
#include "finding.h"
#include "ir.h"
#include "module.h"
#include "report.h"
#include "render_text.h"
#include "render_json.h"
#include "cmds/cmds.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace analyze;

namespace {

const char *kVersion = "rpg-analyze 0.1.0";

void print_help() {
    std::cout <<
        "rpg-analyze -- static analysis for RPG II source\n"
        "\n"
        "USAGE:\n"
        "    rpg-analyze [global options] [command] [command options] <file...>\n"
        "\n"
        "COMMANDS:\n"
        "    report      Run analysis modules and emit a synthesized report (default).\n"
        "    decode      Decode raw spec lines into labeled columns.\n"
        "    search      Structured column-aware query across files.\n"
        "    diff        Semantic structural diff between two programs.\n"
        "    format      Lint / canonicalize formatting (check-only).\n"
        "    docgen      Generate a Markdown reference page for one program.\n"
        "    callgraph   Inter-program CALL/shared-file dependency graph.\n"
        "    duplicate   Clone / copy-paste detector across many files.\n"
        "    portfolio   Codebase-wide metrics dashboard across many files.\n"
        "    migrate     Migration-difficulty scoring across many files.\n"
        "\n"
        "    Any module id (see below) is also a valid command, equivalent to\n"
        "    `report -m <module>`.\n"
        "\n"
        "GLOBAL OPTIONS:\n"
        "    --json               Emit machine-readable JSON instead of text.\n"
        "    --no-color           Disable ANSI color.\n"
        "    -q, --quiet          Suppress section bodies; print only findings.\n"
        "    --severity LEVEL     Minimum severity to show: error, warn, info.\n"
        "    --compiler-feats F   Features file for the `compat` module.\n"
        "    -h, --help           Show this help.\n"
        "    -V, --version        Show version.\n"
        "\n"
        "REPORT OPTIONS:\n"
        "    -m, --module NAME    Enable one module (repeatable).\n"
        "    -a, --all            Enable every analysis module (the default).\n"
        "    --no-module NAME     Disable one module (use with --all to exclude).\n"
        "    --no-findings        Suppress the synthesized findings section.\n"
        "    --section-order a,b,c  Custom ordering of report sections.\n"
        "    -o, --output FILE    Write report to a file instead of stdout.\n"
        "\n"
        "UTILITY COMMAND OPTIONS:\n"
        "    --line N             decode: show only line N.\n"
        "    --range A-B          decode: show only lines A-B.\n"
        "    --query EXPR         search: query DSL, e.g. \"op:COMP ind:20\".\n"
        "    --dot                callgraph: emit Graphviz dot instead of a text tree.\n"
        "    --threshold F        duplicate: minimum similarity to report (default 0.9).\n"
        "    --html               portfolio: emit an HTML dashboard instead of text.\n"
        "\n"
        "MODULES:\n";
    for (auto &m : module_catalog())
        std::cout << "    " << m.id << "\n";
}

struct Options {
    bool json = false;
    bool no_color = false;
    bool quiet = false;
    Severity severity = Severity::Info;
    std::string compiler_feats;
    bool help = false;
    bool version = false;

    std::string command;   // empty => "report"
    std::vector<std::string> modules;
    bool all = false;
    std::vector<std::string> no_modules;
    bool no_findings = false;
    std::vector<std::string> section_order;
    std::string output_file;

    // Utility-subcommand options (§8).
    int line = 0;
    std::string range;
    std::string query;
    bool check = false;
    bool dot = false;
    double threshold = 0.9;
    bool html = false;

    std::vector<std::string> files;
};

std::vector<std::string> split_csv(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(tok);
    return out;
}

bool is_known_command(const std::string &s) {
    static const char *cmds[] = {"report", "decode", "search", "diff", "format",
                                  "docgen", "callgraph", "duplicate", "portfolio", "migrate"};
    for (auto c : cmds) if (s == c) return true;
    return false;
}

/* Returns false + sets *usage_error on a bad flag/missing argument. */
bool parse_args(int argc, char **argv, Options &o, std::string &usage_error) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_arg = [&](const char *flag) -> bool {
            if (i + 1 >= argc) { usage_error = std::string("missing argument to ") + flag; return false; }
            return true;
        };
        if (a == "-h" || a == "--help") { o.help = true; }
        else if (a == "-V" || a == "--version") { o.version = true; }
        else if (a == "--json") { o.json = true; }
        else if (a == "--no-color") { o.no_color = true; }
        else if (a == "-q" || a == "--quiet") { o.quiet = true; }
        else if (a == "--severity") {
            if (!need_arg("--severity")) return false;
            std::string v = argv[++i];
            if (!parse_severity(v, o.severity)) { usage_error = "bad --severity value: " + v; return false; }
        } else if (a == "--compiler-feats") {
            if (!need_arg("--compiler-feats")) return false;
            o.compiler_feats = argv[++i];
        } else if (a == "-m" || a == "--module") {
            if (!need_arg(a.c_str())) return false;
            o.modules.push_back(argv[++i]);
        } else if (a == "-a" || a == "--all") { o.all = true; }
        else if (a == "--no-module") {
            if (!need_arg("--no-module")) return false;
            o.no_modules.push_back(argv[++i]);
        } else if (a == "--no-findings") { o.no_findings = true; }
        else if (a == "--section-order") {
            if (!need_arg("--section-order")) return false;
            for (auto &s : split_csv(argv[++i])) o.section_order.push_back(s);
        } else if (a == "-o" || a == "--output") {
            if (!need_arg(a.c_str())) return false;
            o.output_file = argv[++i];
        } else if (a == "--line") {
            if (!need_arg("--line")) return false;
            try { o.line = std::stoi(argv[++i]); } catch (...) { usage_error = "bad --line value"; return false; }
        } else if (a == "--range") {
            if (!need_arg("--range")) return false;
            o.range = argv[++i];
        } else if (a == "--query") {
            if (!need_arg("--query")) return false;
            o.query = argv[++i];
        } else if (a == "--check") {
            o.check = true;
        } else if (a == "--dot") {
            o.dot = true;
        } else if (a == "--threshold") {
            if (!need_arg("--threshold")) return false;
            try { o.threshold = std::stod(argv[++i]); } catch (...) { usage_error = "bad --threshold value"; return false; }
        } else if (a == "--html") {
            o.html = true;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            usage_error = "unknown option: " + a;
            return false;
        } else {
            if (o.command.empty() && (is_known_command(a) || find_module(a) != nullptr)) {
                o.command = a;
            } else {
                o.files.push_back(a);
            }
        }
    }
    return true;
}

int run_report_command(const Options &o) {
    ReportOptions ropts;
    ropts.all = o.all;
    ropts.modules = o.modules;
    if (!o.command.empty() && o.command != "report" && find_module(o.command) != nullptr) {
        ropts.modules.push_back(o.command);
    }
    ropts.excluded = o.no_modules;
    ropts.no_findings = o.no_findings;
    ropts.section_order = o.section_order;
    ropts.quiet = o.quiet;

    int worst = 0;
    std::vector<Report> reports;

    for (auto &file : o.files) {
        ProgramIR ir = ProgramIR::build(file);
        if (ir.load_failed) {
            std::cerr << "rpg-analyze: cannot open file: " << file << "\n";
            worst = std::max(worst, 3);
            continue;
        }
        Report rep = run_report(ir, ropts);
        FindingCounts c = count_by_severity(rep.findings);
        if (c.errors > 0) worst = std::max(worst, 1);
        reports.push_back(std::move(rep));
    }

    std::ostream *out = &std::cout;
    std::ofstream ofs;
    if (!o.output_file.empty()) {
        ofs.open(o.output_file);
        if (!ofs) {
            std::cerr << "rpg-analyze: cannot open output file: " << o.output_file << "\n";
            return 3;
        }
        out = &ofs;
    }

    if (o.json) {
        JsonRenderOptions jopts;
        jopts.min_severity = o.severity;
        if (reports.size() == 1) {
            *out << render_json(reports[0], jopts).dump(2) << "\n";
        } else {
            Json arr = Json::array();
            for (auto &rep : reports) arr.push_back(render_json(rep, jopts));
            *out << arr.dump(2) << "\n";
        }
    } else {
        TextRenderOptions topts;
        topts.color = !o.no_color;
        topts.no_findings = o.no_findings;
        topts.min_severity = o.severity;
        for (auto &rep : reports) *out << render_text(rep, topts);
    }

    return worst;
}

} // namespace

int main(int argc, char **argv) {
    Options o;
    std::string usage_error;
    if (!parse_args(argc, argv, o, usage_error)) {
        std::cerr << "rpg-analyze: " << usage_error << "\n";
        std::cerr << "run 'rpg-analyze --help' for usage\n";
        return 3;
    }
    if (o.help) { print_help(); return 0; }
    if (o.version) { std::cout << kVersion << "\n"; return 0; }

    std::string cmd = o.command.empty() ? "report" : o.command;

    if (o.files.empty()) {
        std::cerr << "rpg-analyze: no input file\n";
        std::cerr << "run 'rpg-analyze --help' for usage\n";
        return 3;
    }

    std::ostream *out = &std::cout;
    std::ofstream ofs;
    if (!o.output_file.empty()) {
        ofs.open(o.output_file);
        if (!ofs) {
            std::cerr << "rpg-analyze: cannot open output file: " << o.output_file << "\n";
            return 3;
        }
        out = &ofs;
    }

    if (cmd == "decode") {
        DecodeOptions dopts;
        dopts.line = o.line;
        dopts.range = o.range;
        return cmd_decode(o.files, dopts, *out);
    }
    if (cmd == "search") return cmd_search(o.files, o.query, *out);
    if (cmd == "diff") return cmd_diff(o.files, *out);
    if (cmd == "format") return cmd_format(o.files, o.no_color, *out);
    if (cmd == "docgen") return cmd_docgen(o.files, *out);
    if (cmd == "callgraph") return cmd_callgraph(o.files, o.dot, *out);
    if (cmd == "duplicate") return cmd_duplicate(o.files, o.threshold, *out);
    if (cmd == "portfolio") return cmd_portfolio(o.files, o.html, *out);
    if (cmd == "migrate") return cmd_migrate(o.files, *out);

    if (cmd != "report" && find_module(cmd) == nullptr) {
        std::cerr << "rpg-analyze: unknown command '" << cmd << "'\n";
        return 3;
    }

    return run_report_command(o);
}
