#ifndef RPGANALYZE_RENDER_TEXT_H
#define RPGANALYZE_RENDER_TEXT_H

#include "finding.h"
#include "report.h"

#include <string>

namespace analyze {

struct TextRenderOptions {
    bool color = true;
    bool no_findings = false;
    Severity min_severity = Severity::Info;
};

std::string render_text(const Report &rep, const TextRenderOptions &opts);

} // namespace analyze

#endif // RPGANALYZE_RENDER_TEXT_H
