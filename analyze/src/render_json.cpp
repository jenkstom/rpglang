#include "render_json.h"

namespace analyze {

Json render_json(const Report &rep, const JsonRenderOptions &opts) {
    Json root = Json::object();
    root.set("file", rep.file);
    root.set("program_id", rep.program_id);

    Json modules_run = Json::array();
    for (auto &id : rep.modules_run) modules_run.push_back(Json(id));
    root.set("modules_run", modules_run);

    Json sections = Json::array();
    for (auto &res : rep.results) {
        for (auto &sec : res.sections) {
            Json s = Json::object();
            s.set("id", sec.id);
            s.set("title", sec.title);
            s.set("module", res.id);
            s.set("data", sec.data);
            sections.push_back(s);
        }
    }
    root.set("sections", sections);

    auto shown = filter_severity(rep.findings, opts.min_severity);
    Json findings = Json::array();
    for (auto &f : shown) {
        Json j = Json::object();
        j.set("id", f.id);
        j.set("severity", severity_text(f.severity));
        j.set("module", f.module);
        j.set("message", f.message);

        Json loc = Json::object();
        loc.set("file", f.file);
        loc.set("line", f.line);
        loc.set("spec", std::string(1, f.spec == ' ' ? '?' : f.spec));
        loc.set("columns", f.columns);
        j.set("location", loc);

        Json ev = Json::array();
        for (auto &e : f.evidence) {
            Json ej = Json::object();
            ej.set("section", e.section);
            ej.set("line", e.line);
            ev.push_back(ej);
        }
        j.set("evidence", ev);
        if (!f.rule.empty()) j.set("rule", f.rule);
        findings.push_back(j);
    }
    root.set("findings", findings);

    FindingCounts c = count_by_severity(rep.findings);
    Json summary = Json::object();
    summary.set("errors", c.errors);
    summary.set("warnings", c.warnings);
    summary.set("info", c.infos);
    root.set("summary", summary);

    return root;
}

} // namespace analyze
