/* comments -- comment / TODO mining (TOOLS_IDEAS.md §4.19). */
#include "modules.h"
#include "../util.h"

namespace analyze {

namespace {

const char *kMarkers[] = {"TODO", "FIXME", "HACK", "TEMP", "XXX", "NOTE"};

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "comments";
    r.title = "Comment / TODO mining";

    Section sec;
    sec.id = "comments";
    sec.title = "Comment / TODO mining";
    Json arr = Json::array();

    for (auto &sl : ir.raw_lines) {
        if (!sl.comment) continue;
        std::string upper = upper_str(sl.text);
        for (auto *marker : kMarkers) {
            if (upper.find(marker) == std::string::npos) continue;
            if (!opts.quiet)
                sec.text_lines.push_back(std::to_string(sl.lineno) + " [" + marker + "]: " + sl.text);
            Json j = Json::object();
            j.set("line", sl.lineno);
            j.set("marker", marker);
            j.set("text", sl.text);
            arr.push_back(j);

            Finding f;
            f.id = "COMM-MARKER";
            f.severity = Severity::Info;
            f.module = "comments";
            f.message = std::string(marker) + " comment found";
            f.file = ir.path;
            f.line = sl.lineno;
            r.findings.push_back(f);
            break; // one finding per line even if it matches multiple markers
        }
    }

    sec.data.set("markers", arr);
    if (!opts.quiet) r.sections.push_back(std::move(sec));

    return r;
}

} // namespace

ModuleInfo make_comments_module() {
    return ModuleInfo{"comments", "Comment / TODO mining", "NOTES", run};
}

} // namespace analyze
