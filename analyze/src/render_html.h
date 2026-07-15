// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * render_html.h -- render a Report as a self-contained, tabbed HTML document.
 *
 * The third renderer alongside render_text / render_json. Like render_json it
 * walks the in-memory Report, but emits an HTML dashboard grouped by module
 * section_group, one tab per group plus a Findings tab. No external assets --
 * CSS and JS are inlined.
 * ========================================================================== */
#ifndef RPGANALYZE_RENDER_HTML_H
#define RPGANALYZE_RENDER_HTML_H

#include "finding.h"
#include "report.h"

#include <string>
#include <vector>

namespace analyze {

struct HtmlRenderOptions {
    bool no_findings = false;
    Severity min_severity = Severity::Info;
};

/* Renders one or more reports as a single HTML document. Multiple reports
 * (one per input file) stack as separate per-program dashboards. */
std::string render_html(const std::vector<Report> &reps, const HtmlRenderOptions &opts);

} // namespace analyze

#endif // RPGANALYZE_RENDER_HTML_H
