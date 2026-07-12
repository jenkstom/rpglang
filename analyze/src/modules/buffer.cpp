/* buffer -- buffer integrity check (TOOLS_IDEAS.md §4.15).
 *
 * File-record BUF-OVERLAP/OVERRUN/GAP are covered by `recordmap`; this module
 * adds the checks unique to it: DS subfield overlap, and packed/binary length
 * vs. stated decimals.
 */
#include "modules.h"
#include "../util.h"

#include <algorithm>
#include <unordered_map>

namespace analyze {

namespace {

/* Max representable decimal digits for a `length`-byte packed field: the
 * last byte holds one digit + sign nibble, every other byte holds two. */
int packed_max_digits(int length) { return length <= 0 ? 0 : 2 * length - 1; }

/* Binary: 2 bytes -> 16-bit signed (~4-5 digits), >=4 bytes -> 32-bit (~9-10). */
int binary_max_digits(int length) { return length <= 2 ? 4 : 9; }

ModuleResult run(const ProgramIR &ir, const ModuleOptions &opts) {
    ModuleResult r;
    r.id = "buffer";
    r.title = "Buffer integrity check";

    std::vector<std::string> notes;

    // BUF-DSOVERLAP
    std::unordered_map<int, std::vector<const rpgc::ISpecSubfield *>> by_ds;
    for (auto &sub : ir.prog.ds_subfields) by_ds[sub.ds_index].push_back(&sub);
    for (auto &kv : by_ds) {
        auto subs = kv.second;
        std::stable_sort(subs.begin(), subs.end(),
                         [](auto *a, auto *b) { return a->from < b->from; });
        std::string ds_name;
        if (kv.first >= 0 && (size_t)kv.first < ir.prog.data_structures.size())
            ds_name = ir.prog.data_structures[kv.first].name;
        if (ds_name.empty()) ds_name = "DS#" + std::to_string(kv.first);

        for (size_t i = 1; i < subs.size(); ++i) {
            if (subs[i]->from <= subs[i - 1]->to) {
                Finding f;
                f.id = "BUF-DSOVERLAP";
                f.severity = Severity::Error;
                f.module = "buffer";
                f.message = "subfield '" + subs[i]->name + "' overlaps '" + subs[i - 1]->name +
                            "' in data structure " + ds_name;
                f.file = ir.path;
                f.line = subs[i]->lineno;
                f.spec = 'I';
                f.evidence.push_back({"buffer", subs[i - 1]->lineno});
                r.findings.push_back(f);
                notes.push_back(f.message);
            }
        }
    }

    // BUF-PACKEDLEN: stated decimals exceed what the byte length can hold.
    for (auto &fld : ir.prog.in_fields) {
        if (fld.decimals < 0 || (fld.data_format != 'P' && fld.data_format != 'B')) continue;
        int len = fld.to - fld.from + 1;
        int maxd = fld.data_format == 'P' ? packed_max_digits(len) : binary_max_digits(len);
        if (fld.decimals > maxd) {
            Finding f;
            f.id = "BUF-PACKEDLEN";
            f.severity = Severity::Error;
            f.module = "buffer";
            f.message = "field '" + fld.name + "' declares " + std::to_string(fld.decimals) +
                        " decimal(s), more than its " + std::to_string(len) + "-byte " +
                        (fld.data_format == 'P' ? "packed" : "binary") + " storage can hold (max " +
                        std::to_string(maxd) + ")";
            f.file = ir.path;
            f.line = fld.lineno;
            f.spec = 'I';
            f.columns = "52";
            r.findings.push_back(f);
            notes.push_back(f.message);
        }
    }
    for (auto &a : ir.prog.arrays) {
        if (a.decimals < 0 || (a.data_format != 'P' && a.data_format != 'B')) continue;
        int maxd = a.data_format == 'P' ? packed_max_digits(a.entry_len) : binary_max_digits(a.entry_len);
        if (a.decimals > maxd) {
            Finding f;
            f.id = "BUF-PACKEDLEN";
            f.severity = Severity::Error;
            f.module = "buffer";
            f.message = "array '" + a.name + "' declares " + std::to_string(a.decimals) +
                        " decimal(s), more than its " + std::to_string(a.entry_len) + "-byte " +
                        (a.data_format == 'P' ? "packed" : "binary") + " entries can hold (max " +
                        std::to_string(maxd) + ")";
            f.file = ir.path;
            f.line = a.lineno;
            f.spec = 'E';
            r.findings.push_back(f);
            notes.push_back(f.message);
        }
    }

    if (!opts.quiet) {
        Section sec;
        sec.id = "buffer";
        sec.title = "Buffer integrity check";
        sec.text_lines.push_back("(file-record overlap/overrun/gap: see the recordmap module)");
        if (notes.empty()) sec.text_lines.push_back("(no DS-overlap or packed-length issues detected)");
        else for (auto &n : notes) sec.text_lines.push_back(n);
        r.sections.push_back(std::move(sec));
    }

    return r;
}

} // namespace

ModuleInfo make_buffer_module() {
    return ModuleInfo{"buffer", "Buffer integrity check", "SECURITY", run};
}

} // namespace analyze
