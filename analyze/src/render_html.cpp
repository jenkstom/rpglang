// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

#include "render_html.h"

#include "module.h"
#include "util.h"

#include <map>
#include <sstream>

namespace analyze {

namespace {

/* ---- Json -> HTML -------------------------------------------------------- */

std::string json_to_html(const Json &v);

/* Renders a Json object of scalar fields as a two-column key/value table. */
std::string scalar_object_table(const Json &obj) {
    std::ostringstream h;
    h << "<table class=\"kv\"><tbody>";
    for (auto &kv : obj.fields()) {
        const Json &val = kv.second;
        // Skip nested compound values here; they are rendered as nested
        // blocks below the table.
        if (val.type() == Json::Type::Array || val.type() == Json::Type::Object) continue;
        h << "<tr><th>" << html_escape(kv.first) << "</th><td>";
        switch (val.type()) {
            case Json::Type::Bool:
                h << (val.as_bool() ? "yes" : "no");
                break;
            case Json::Type::Int:
                h << val.as_int();
                break;
            case Json::Type::Double:
                h << val.as_double();
                break;
            case Json::Type::String:
                h << html_escape(val.as_string());
                break;
            default:
                h << html_escape(val.dump());
                break;
        }
        h << "</td></tr>";
    }
    h << "</tbody></table>";
    return h.str();
}

bool is_compound(const Json &v) {
    return v.type() == Json::Type::Array || v.type() == Json::Type::Object;
}

/* True if `data` carries structured content worth rendering as HTML tables.
 * Section::data defaults to an empty Json::object (module.h), so a mere
 * non-null check is not enough -- empty objects/arrays must fall back to the
 * pre-rendered text_lines. */
bool has_structured_content(const Json &v) {
    switch (v.type()) {
        case Json::Type::Object: return !v.fields().empty();
        case Json::Type::Array:  return !v.items().empty();
        case Json::Type::Null:   return false;
        default: return true;  // scalar
    }
}

/* True if every element of an array is an object (table-shaped). */
bool is_array_of_objects(const Json &arr) {
    if (arr.items().empty()) return false;
    for (auto &el : arr.items())
        if (el.type() != Json::Type::Object) return false;
    return true;
}

/* Collects the union of keys across all object rows, preserving the order in
 * which keys first appear (so column headers track the module's schema). */
std::vector<std::string> collect_keys(const Json &arr) {
    std::vector<std::string> keys;
    std::map<std::string, bool> seen;
    for (auto &row : arr.items()) {
        if (row.type() != Json::Type::Object) continue;
        for (auto &kv : row.fields()) {
            if (!seen[kv.first]) { seen[kv.first] = true; keys.push_back(kv.first); }
        }
    }
    return keys;
}

std::string json_to_html(const Json &v) {
    std::ostringstream h;
    switch (v.type()) {
        case Json::Type::Null:
            h << "<span class=\"null\">&mdash;</span>";
            break;
        case Json::Type::Bool:
            h << (v.as_bool() ? "yes" : "no");
            break;
        case Json::Type::Int:
            h << v.as_int();
            break;
        case Json::Type::Double:
            h << v.as_double();
            break;
        case Json::Type::String:
            h << "<span class=\"str\">" << html_escape(v.as_string()) << "</span>";
            break;
        case Json::Type::Array: {
            const auto &items = v.items();
            if (items.empty()) {
                h << "<span class=\"empty\">(empty)</span>";
                break;
            }
            if (is_array_of_objects(v)) {
                auto keys = collect_keys(v);
                h << "<table class=\"grid\"><thead><tr>";
                for (auto &k : keys) h << "<th>" << html_escape(k) << "</th>";
                h << "</tr></thead><tbody>";
                for (auto &row : items) {
                    h << "<tr>";
                    // Build a lookup so missing keys render as empty cells.
                    std::map<std::string, const Json *> vals;
                    for (auto &kv : row.fields()) vals[kv.first] = &kv.second;
                    for (auto &k : keys) {
                        h << "<td>";
                        auto it = vals.find(k);
                        if (it != vals.end()) h << json_to_html(*it->second);
                        else h << "&nbsp;";
                        h << "</td>";
                    }
                    h << "</tr>";
                }
                h << "</tbody></table>";
            } else {
                h << "<ul class=\"scalar-list\">";
                for (auto &el : items) h << "<li>" << json_to_html(el) << "</li>";
                h << "</ul>";
            }
            break;
        }
        case Json::Type::Object: {
            bool has_compound = false;
            for (auto &kv : v.fields()) if (is_compound(kv.second)) { has_compound = true; break; }
            if (!has_compound && !v.fields().empty()) {
                h << scalar_object_table(v);
            } else {
                h << "<div class=\"obj\">";
                for (auto &kv : v.fields()) {
                    h << "<div class=\"field\"><span class=\"field-key\">"
                      << html_escape(kv.first) << "</span>: ";
                    h << json_to_html(kv.second) << "</div>";
                }
                h << "</div>";
            }
            break;
        }
    }
    return h.str();
}

/* ---- Findings ------------------------------------------------------------ */

const char *sev_class(Severity s) {
    switch (s) {
        case Severity::Error: return "sev-error";
        case Severity::Warn:  return "sev-warn";
        case Severity::Info:  return "sev-info";
    }
    return "sev-info";
}

std::string render_findings_table(const Report &rep, Severity min_severity) {
    auto shown = filter_severity(rep.findings, min_severity);
    std::ostringstream h;
    if (shown.empty()) {
        h << "<p class=\"empty\">No findings at this severity threshold.</p>";
        return h.str();
    }
    h << "<table class=\"findings\"><thead><tr>"
      << "<th>Sev</th><th>ID</th><th>Module</th><th>Location</th>"
      << "<th>Message</th><th>Evidence</th>"
      << "</tr></thead><tbody>";
    for (auto &f : shown) {
        std::string loc = html_escape(f.file);
        if (f.line > 0) loc += ":" + std::to_string(f.line);
        std::string spec_col;
        if (f.spec != ' ') spec_col = std::string(1, f.spec);
        if (!f.columns.empty()) {
            if (!spec_col.empty()) spec_col += " ";
            spec_col += f.columns;
        }
        if (!spec_col.empty()) loc += " <span class=\"loc-col\">(" + html_escape(spec_col) + ")</span>";

        std::ostringstream ev;
        for (size_t i = 0; i < f.evidence.size(); ++i) {
            if (i) ev << ", ";
            ev << "&sect;" << html_escape(f.evidence[i].section);
            if (f.evidence[i].line) ev << ":" << f.evidence[i].line;
        }
        h << "<tr>"
          << "<td><span class=\"badge " << sev_class(f.severity) << "\">"
          << severity_text(f.severity) << "</span></td>"
          << "<td class=\"mono\">" << html_escape(f.id) << "</td>"
          << "<td class=\"mono\">" << html_escape(f.module) << "</td>"
          << "<td class=\"mono\">" << loc << "</td>"
          << "<td>" << html_escape(f.message) << "</td>"
          << "<td class=\"evidence\">" << ev.str() << "</td>"
          << "</tr>";
    }
    h << "</tbody></table>";
    return h.str();
}

/* ---- One report (per-program dashboard) ---------------------------------- */

struct GroupedSection {
    std::string group;          // section_group, e.g. "FILES"
    const ModuleResult *res;
    const Section *sec;
};

/* Collects sections grouped by their module's section_group, in
 * first-appearance order across the report's results. */
std::vector<std::string> collect_groups(const Report &rep,
                                        std::map<std::string, std::vector<GroupedSection>> &by_group) {
    std::vector<std::string> order;
    for (auto &res : rep.results) {
        const ModuleInfo *info = find_module(res.id);
        std::string group = info ? info->section_group : res.title;
        if (group.empty()) group = "OTHER";
        if (by_group.find(group) == by_group.end()) order.push_back(group);
        for (auto &sec : res.sections) {
            // Skip sections with neither structured data nor text.
            if (!has_structured_content(sec.data) && sec.text_lines.empty()) continue;
            by_group[group].push_back({group, &res, &sec});
        }
    }
    return order;
}

std::string slug(const std::string &s, int idx) {
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        else if (c >= 'A' && c <= 'Z') out += (char)std::tolower(c);
    }
    if (out.empty()) out = "g" + std::to_string(idx);
    return out + std::to_string(idx);
}

std::string render_report(const Report &rep, const HtmlRenderOptions &opts,
                          bool multiple, size_t prog_index) {
    std::ostringstream h;

    // ---- Header / summary badges ----
    FindingCounts c = count_by_severity(rep.findings);
    h << "<section class=\"program\" id=\"prog" << prog_index << "\"><header class=\"program-header\">"
      << "<h2>RPG Analysis &mdash; " << html_escape(rep.program_id) << "</h2>"
      << "<div class=\"program-meta\">" << html_escape(rep.file) << "</div>"
      << "<div class=\"badges\">";
    if (c.errors)   h << "<span class=\"badge sev-error\">" << c.errors   << " error"   << (c.errors   > 1 ? "s" : "") << "</span>";
    if (c.warnings) h << "<span class=\"badge sev-warn\">"  << c.warnings << " warning" << (c.warnings > 1 ? "s" : "") << "</span>";
    if (c.infos)    h << "<span class=\"badge sev-info\">"  << c.infos    << " info"    << (c.infos    > 1 ? "s" : "") << "</span>";
    if (!c.errors && !c.warnings && !c.infos)
        h << "<span class=\"badge sev-clean\">no findings</span>";
    h << "</div></header>";

    // ---- Group sections by section_group ----
    std::map<std::string, std::vector<GroupedSection>> by_group;
    auto order = collect_groups(rep, by_group);

    bool show_findings = !opts.no_findings;
    auto shown = filter_severity(rep.findings, opts.min_severity);
    bool any_findings = !shown.empty();

    if (order.empty() && !show_findings) {
        h << "<p class=\"empty\">No sections to display.</p></section>";
        return h.str();
    }

    // ---- Tab bar ----
    h << "<nav class=\"tabs\" data-prog=\"" << prog_index << "\">";
    for (size_t i = 0; i < order.size(); ++i) {
        h << "<button class=\"tab" << (i == 0 ? " active" : "") << "\""
          << " data-tab=\"" << slug(order[i], (int)i) << "\">"
          << html_escape(order[i]) << "</button>";
    }
    if (show_findings && any_findings) {
        h << "<button class=\"tab tab-findings\" data-tab=\"findings"
          << prog_index << "\">Findings (" << shown.size() << ")</button>";
    }
    h << "</nav>";

    // ---- Tab panels ----
    for (size_t i = 0; i < order.size(); ++i) {
        const std::string &group = order[i];
        h << "<div class=\"tab-panel" << (i == 0 ? " active" : "") << "\""
          << " id=\"" << slug(group, (int)i) << "\">";
        const auto &group_secs = by_group[group];
        // Section index: quick links to each module card below. Only shown
        // when a tab carries more than one module (otherwise it's noise).
        if (group_secs.size() >= 2) {
            h << "<nav class=\"section-index\"><span class=\"index-label\">Sections:</span> ";
            for (size_t mi = 0; mi < group_secs.size(); ++mi) {
                if (mi) h << " <span class=\"index-sep\">&middot;</span> ";
                std::string anchor = "m" + std::to_string(prog_index) + "_" +
                                     std::to_string(i) + "_" + std::to_string(mi);
                h << "<a href=\"#" << anchor << "\">"
                  << html_escape(group_secs[mi].sec->title) << "</a>";
            }
            h << "</nav>";
        }
        for (size_t mi = 0; mi < group_secs.size(); ++mi) {
            const auto &gs = group_secs[mi];
            std::string anchor = "m" + std::to_string(prog_index) + "_" +
                                 std::to_string(i) + "_" + std::to_string(mi);
            h << "<div class=\"module-card\" id=\"" << anchor << "\">"
              << "<div class=\"module-head\"><span class=\"module-title\">"
              << html_escape(gs.sec->title) << "</span>"
              << "<span class=\"module-id\">[" << html_escape(gs.res->id) << "]</span></div>"
              << "<div class=\"module-body\">";
            // Prefer structured data; fall back to pre-rendered text.
            if (has_structured_content(gs.sec->data)) {
                h << json_to_html(gs.sec->data);
            } else if (!gs.sec->text_lines.empty()) {
                h << "<pre class=\"text-block\">";
                for (size_t li = 0; li < gs.sec->text_lines.size(); ++li) {
                    if (li) h << "\n";
                    h << html_escape(gs.sec->text_lines[li]);
                }
                h << "</pre>";
            }
            h << "</div></div>";
        }
        h << "</div>";
    }

    // ---- Findings panel ----
    if (show_findings && any_findings) {
        h << "<div class=\"tab-panel\" id=\"findings" << prog_index << "\">"
          << render_findings_table(rep, opts.min_severity)
          << "</div>";
    }

    h << "</section>";
    (void)multiple;
    return h.str();
}

} // namespace

std::string render_html(const std::vector<Report> &reps, const HtmlRenderOptions &opts) {
    std::ostringstream h;
    h << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "<title>RPG Analysis</title>\n<style>\n"
      << R"CSS(
:root {
  --bg: #f5f6f8;
  --card: #ffffff;
  --ink: #1f2328;
  --muted: #656d76;
  --border: #d3d8de;
  --accent: #2a6cb0;
  --accent-soft: #e8f0fb;
  --red: #cf222e;
  --red-soft: #ffebe9;
  --amber: #bf8700;
  --amber-soft: #fff8c5;
  --blue: #0969da;
  --blue-soft: #ddf4ff;
  --green: #1a7f37;
  --green-soft: #dafbe1;
  --stripe: #fafbfc;
  --code-bg: #f6f8fa;
  --shadow: 0 1px 3px rgba(31,35,40,.08), 0 1px 2px rgba(31,35,40,.06);
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0d1117;
    --card: #161b22;
    --ink: #e6edf3;
    --muted: #8b949e;
    --border: #30363d;
    --accent: #58a6ff;
    --accent-soft: #1f2a40;
    --red: #ff7b72;
    --red-soft: #4a1f1f;
    --amber: #d29922;
    --amber-soft: #3a2f0b;
    --blue: #58a6ff;
    --blue-soft: #14304a;
    --green: #3fb950;
    --green-soft: #14341f;
    --stripe: #1c2128;
    --code-bg: #161b22;
    --shadow: 0 1px 3px rgba(0,0,0,.4), 0 1px 2px rgba(0,0,0,.3);
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; padding: 24px;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  color: var(--ink); background: var(--bg); line-height: 1.5;
}
h1 { font-size: 1.5rem; margin: 0 0 4px; }
.program { max-width: 1100px; margin: 0 auto 32px; }
.program-header {
  background: var(--card); border: 1px solid var(--border);
  border-radius: 8px; padding: 16px 20px; margin-bottom: 12px;
  box-shadow: var(--shadow);
}
.program-header h2 { margin: 0 0 4px; font-size: 1.3rem; }
.program-meta { color: var(--muted); font-size: .9rem; font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
.badges { margin-top: 10px; display: flex; flex-wrap: wrap; gap: 8px; }
.badge {
  display: inline-block; padding: 2px 10px; border-radius: 12px;
  font-size: .78rem; font-weight: 600; font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
.sev-error { background: var(--red-soft); color: var(--red); }
.sev-warn  { background: var(--amber-soft); color: var(--amber); }
.sev-info  { background: var(--blue-soft); color: var(--blue); }
.sev-clean { background: var(--green-soft); color: var(--green); }
.tabs {
  position: sticky; top: 0; z-index: 10;
  display: flex; flex-wrap: wrap; gap: 4px;
  background: var(--bg); padding: 8px 0;
}
.tab {
  border: 1px solid var(--border); background: var(--card); color: var(--muted);
  padding: 6px 14px; border-radius: 6px 6px 0 0; cursor: pointer;
  font-size: .85rem; font-weight: 500; font-family: inherit;
  transition: background .12s, color .12s;
}
.tab:hover { background: var(--accent-soft); color: var(--accent); }
.tab.active { background: var(--accent); color: #fff; border-color: var(--accent); }
.tab-findings.active { background: var(--red); border-color: var(--red); }
.tab-panel { display: none; background: var(--card); border: 1px solid var(--border); border-top: none; border-radius: 0 0 8px 8px; padding: 16px 20px; box-shadow: var(--shadow); }
.tab-panel.active { display: block; }
.section-index {
  font-size: .82rem; color: var(--muted); margin-bottom: 16px; padding-bottom: 12px;
  border-bottom: 1px solid var(--border); line-height: 1.8;
}
.section-index .index-label { font-weight: 600; margin-right: 4px; }
.section-index .index-sep { color: var(--border); }
.section-index a { color: var(--accent); text-decoration: none; }
.section-index a:hover { text-decoration: underline; }
.module-card { margin-bottom: 20px; }
.module-card:last-child { margin-bottom: 0; }
.module-head {
  display: flex; align-items: baseline; gap: 10px;
  border-bottom: 1px solid var(--border); padding-bottom: 6px; margin-bottom: 12px;
}
.module-title { font-weight: 600; font-size: 1rem; }
.module-id { color: var(--muted); font-size: .78rem; font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
.module-body { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-size: .88rem; }
table.kv { width: auto; max-width: 100%; }
table.kv th { text-align: right; color: var(--muted); font-weight: 500; white-space: nowrap; }
table.kv td, table.kv th { padding: 4px 12px 4px 0; vertical-align: top; }
table.grid, table.findings { margin-top: 4px; }
table.grid th, table.findings th {
  text-align: left; background: var(--accent-soft); color: var(--accent);
  padding: 6px 10px; border: 1px solid var(--border); font-weight: 600; white-space: nowrap;
}
table.grid td, table.findings td { padding: 5px 10px; border: 1px solid var(--border); vertical-align: top; }
table.grid tbody tr:nth-child(even), table.findings tbody tr:nth-child(even) { background: var(--stripe); }
.mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: .82rem; }
.loc-col { color: var(--muted); font-size: .82rem; }
.evidence { font-size: .82rem; color: var(--muted); font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
.str { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
.null { color: var(--muted); }
.empty { color: var(--muted); font-style: italic; }
ul.scalar-list { margin: 4px 0; padding-left: 20px; }
.obj { margin: 4px 0; }
.field { margin: 2px 0; }
.field-key { font-weight: 500; color: var(--muted); font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
pre.text-block {
  background: var(--code-bg); border: 1px solid var(--border); border-radius: 6px;
  padding: 12px; overflow-x: auto; font-size: .82rem; line-height: 1.45;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
footer { max-width: 1100px; margin: 16px auto 0; color: var(--muted); font-size: .8rem; }
)CSS"
      << "</style>\n</head>\n<body>\n";

    h << "<h1>RPG Analysis Report</h1>\n";
    for (size_t i = 0; i < reps.size(); ++i)
        h << render_report(reps[i], opts, reps.size() > 1, i);

    h << "<footer>Generated by rpg-analyze</footer>\n";

    // ---- Minimal tab-switching JS (no dependencies) ----
    h << "<script>\n"
      << "(function(){\n"
      << "  document.querySelectorAll('.tabs').forEach(function(bar){\n"
      << "    var tabs = bar.querySelectorAll('.tab');\n"
      << "    tabs.forEach(function(t){\n"
      << "      t.addEventListener('click', function(){\n"
      << "        tabs.forEach(function(x){ x.classList.remove('active'); });\n"
      << "        t.classList.add('active');\n"
      << "        var prog = bar.closest('.program');\n"
      << "        prog.querySelectorAll('.tab-panel').forEach(function(p){ p.classList.remove('active'); });\n"
      << "        var target = prog.querySelector('#' + t.getAttribute('data-tab'));\n"
      << "        if (target) target.classList.add('active');\n"
      << "        // Jump to the top of the program dashboard so the newly\n"
      << "        // active tab's content starts at the top of the viewport,\n"
      << "        // clear of the sticky tab bar.\n"
      << "        var top = prog.querySelector('.program-header');\n"
      << "        if (top) top.scrollIntoView(); else window.scrollTo(0, 0);\n"
      << "      });\n"
      << "    });\n"
      << "  });\n"
      << "})();\n"
      << "</script>\n</body>\n</html>\n";

    return h.str();
}

} // namespace analyze
