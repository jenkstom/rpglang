#ifndef RPGANALYZE_RENDER_JSON_H
#define RPGANALYZE_RENDER_JSON_H

#include "finding.h"
#include "json.h"
#include "report.h"

namespace analyze {

struct JsonRenderOptions {
    Severity min_severity = Severity::Info;
};

Json render_json(const Report &rep, const JsonRenderOptions &opts);

} // namespace analyze

#endif // RPGANALYZE_RENDER_JSON_H
