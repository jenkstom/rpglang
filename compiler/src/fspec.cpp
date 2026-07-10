/* ========================================================================== *
 * fspec.cpp -- parse File Description Specifications.
 * ========================================================================== */
#include "fspec.h"
#include "diagnostics.h"

#include <algorithm>
#include <cctype>

namespace rpgc {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}
} // namespace

std::vector<FSpec> parse_fspecs(const std::vector<SourceLine> &src) {
    std::vector<FSpec> out;
    for (const auto &sl : src) {
        if (sl.comment) continue;
        if (form_type(sl) != 'F') continue;

        FSpec f;
        f.lineno = sl.lineno;
        f.name   = col_trim(sl.text, 7, 14);
        if (f.name.empty()) {
            report("input", sl.lineno, 7, DiagKind::Warning,
                   "F-spec with no filename (ignored)");
            continue;
        }

        // File type (col 15)
        switch (upper(col_trim(sl.text, 15, 15))[0]) {
            case 'O': f.type = FileType::Output;    break;
            case 'U': f.type = FileType::Update;    break;
            case 'C': f.type = FileType::Combined;  break;
            default:  f.type = FileType::Input;     break; // 'I' or blank
        }

        // Designation (col 16)
        switch (upper(col_trim(sl.text, 16, 16))[0]) {
            case 'P': f.design = FileDesign::Primary;   break;
            case 'S': f.design = FileDesign::Secondary; break;
            case 'F': f.design = FileDesign::FullProc;  break;
            case 'C': f.design = FileDesign::Chained;   break;
            case 'R': f.design = FileDesign::RecordAddr;break;
            case 'T': f.design = FileDesign::Table;     break;
            case 'D': f.design = FileDesign::Demand;    break;
            default:
                // Output files have blank designation; primary is the cycle
                // default for input files.
                f.design = (f.type == FileType::Output) ? FileDesign::None
                                                        : FileDesign::Primary;
                break;
        }

        // Format (col 19): F or blank => fixed.
        std::string fmt = upper(col_trim(sl.text, 19, 19));
        f.format = fmt.empty() ? 'F' : fmt[0];

        // Record length (cols 24-27)
        std::string rl = col_trim(sl.text, 24, 27);
        if (!rl.empty()) {
            try { f.reclen = std::stoi(rl); }
            catch (...) { f.reclen = 0; }
        }

        // Device (cols 40-46)
        f.device_text = upper(col_trim(sl.text, 40, 46));
        if (f.device_text == "DISK")          f.device = Device::Disk;
        else if (f.device_text == "PRINTER")  f.device = Device::Printer;
        else if (f.device_text == "WORKSTN")  f.device = Device::Workstn;
        else if (f.device_text == "SPECIAL")  f.device = Device::Special;
        else if (f.device_text == "CONSOLE")  f.device = Device::Console;
        else                                  f.device = Device::Other;

        out.push_back(std::move(f));
    }
    return out;
}

const FSpec *find_primary_input(const std::vector<FSpec> &fs) {
    for (const auto &f : fs) {
        if (f.type == FileType::Input && f.design == FileDesign::Primary) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace rpgc
