// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * clean.cpp -- implementation. See clean.h for the staged-pipeline design.
 *
 * Stage order matters and is chosen so each stage sees the input the next one
 * expects:
 *
 *   1. strip trailing NUL padding    (cheap; must precede EBCDIC scoring which
 *                                     would otherwise be swamped by NULs)
 *   2. EBCDIC -> ASCII               (only on high confidence; transforms the
 *                                     whole byte stream, so do it before any
 *                                     ASCII-only separator/recognition logic)
 *   3. separator-sequence repair     (the 5250 ESC / EBCDIC-NL-as-C1 cases;
 *                                     now operating on ASCII bytes)
 *   4. fixed-width no-newline split  (the S/34/S/36 card-image case)
 *   5. line-ending normalization     (guarantee every record ends in LF)
 *   6. final residue check           (flag suspicious leftover non-printables)
 *
 * Each stage is a pair (detect_*, apply_*). detect_* returns true and fills a
 * short detail string when the stage should fire. The driver only calls the
 * matching apply_* when detect fired, and records the detail. Keeping detect
 * and apply separate is what makes the tool "smart enough to determine the
 * issue before converting" and lets --check mode report without modifying.
 * ========================================================================== */
#include "clean.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace rpgc {

namespace {

/* -------------------------------------------------------------------------- */
/* EBCDIC -> ASCII translate tables.                                          */
/*                                                                            */
/* Each table: table[ebcdic_byte] -> latin-1/ASCII byte. Generated from the   */
/* canonical IBM codepage definitions (cp037 = US EBCDIC, the most common on  */
/* IBM i US source; cp500 = international EBCDIC). 128 of the 256 positions   */
/* map to non-ASCII latin-1 (printable but >127); those bytes are vanishingly */
/* rare in real RPG source, which is exactly what the scoring detector relies */
/* on to tell EBCDIC from ASCII-with-junk.                                    */
/* -------------------------------------------------------------------------- */
static const unsigned char kCP037[256] = {
    0x00, 0x01, 0x02, 0x03, 0x9c, 0x09, 0x86, 0x7f, 0x97, 0x8d, 0x8e, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x9d, 0x85, 0x08, 0x87, 0x18, 0x19, 0x92, 0x8f, 0x1c, 0x1d, 0x1e, 0x1f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x0a, 0x17, 0x1b, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x05, 0x06, 0x07,
    0x90, 0x91, 0x16, 0x93, 0x94, 0x95, 0x96, 0x04, 0x98, 0x99, 0x9a, 0x9b, 0x14, 0x15, 0x9e, 0x1a,
    0x20, 0xa0, 0xe2, 0xe4, 0xe0, 0xe1, 0xe3, 0xe5, 0xe7, 0xf1, 0xa2, 0x2e, 0x3c, 0x28, 0x2b, 0x7c,
    0x26, 0xe9, 0xea, 0xeb, 0xe8, 0xed, 0xee, 0xef, 0xec, 0xdf, 0x21, 0x24, 0x2a, 0x29, 0x3b, 0xac,
    0x2d, 0x2f, 0xc2, 0xc4, 0xc0, 0xc1, 0xc3, 0xc5, 0xc7, 0xd1, 0xa6, 0x2c, 0x25, 0x5f, 0x3e, 0x3f,
    0xf8, 0xc9, 0xca, 0xcb, 0xc8, 0xcd, 0xce, 0xcf, 0xcc, 0x60, 0x3a, 0x23, 0x40, 0x27, 0x3d, 0x22,
    0xd8, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xab, 0xbb, 0xf0, 0xfd, 0xfe, 0xb1,
    0xb0, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0xaa, 0xba, 0xe6, 0xb8, 0xc6, 0xa4,
    0xb5, 0x7e, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0xa1, 0xbf, 0xd0, 0xdd, 0xde, 0xae,
    0x5e, 0xa3, 0xa5, 0xb7, 0xa9, 0xa7, 0xb6, 0xbc, 0xbd, 0xbe, 0x5b, 0x5d, 0xaf, 0xa8, 0xb4, 0xd7,
    0x7b, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0xad, 0xf4, 0xf6, 0xf2, 0xf3, 0xf5,
    0x7d, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0xb9, 0xfb, 0xfc, 0xf9, 0xfa, 0xff,
    0x5c, 0xf7, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xb2, 0xd4, 0xd6, 0xd2, 0xd3, 0xd5,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0xb3, 0xdb, 0xdc, 0xd9, 0xda, 0x9f,
};

static const unsigned char kCP500[256] = {
    0x00, 0x01, 0x02, 0x03, 0x9c, 0x09, 0x86, 0x7f, 0x97, 0x8d, 0x8e, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x9d, 0x85, 0x08, 0x87, 0x18, 0x19, 0x92, 0x8f, 0x1c, 0x1d, 0x1e, 0x1f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x0a, 0x17, 0x1b, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x05, 0x06, 0x07,
    0x90, 0x91, 0x16, 0x93, 0x94, 0x95, 0x96, 0x04, 0x98, 0x99, 0x9a, 0x9b, 0x14, 0x15, 0x9e, 0x1a,
    0x20, 0xa0, 0xe2, 0xe4, 0xe0, 0xe1, 0xe3, 0xe5, 0xe7, 0xf1, 0x5b, 0x2e, 0x3c, 0x28, 0x2b, 0x21,
    0x26, 0xe9, 0xea, 0xeb, 0xe8, 0xed, 0xee, 0xef, 0xec, 0xdf, 0x5d, 0x24, 0x2a, 0x29, 0x3b, 0x5e,
    0x2d, 0x2f, 0xc2, 0xc4, 0xc0, 0xc1, 0xc3, 0xc5, 0xc7, 0xd1, 0xa6, 0x2c, 0x25, 0x5f, 0x3e, 0x3f,
    0xf8, 0xc9, 0xca, 0xcb, 0xc8, 0xcd, 0xce, 0xcf, 0xcc, 0x60, 0x3a, 0x23, 0x40, 0x27, 0x3d, 0x22,
    0xd8, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xab, 0xbb, 0xf0, 0xfd, 0xfe, 0xb1,
    0xb0, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0xaa, 0xba, 0xe6, 0xb8, 0xc6, 0xa4,
    0xb5, 0x7e, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0xa1, 0xbf, 0xd0, 0xdd, 0xde, 0xae,
    0xa2, 0xa3, 0xa5, 0xb7, 0xa9, 0xa7, 0xb6, 0xbc, 0xbd, 0xbe, 0xac, 0x7c, 0xaf, 0xa8, 0xb4, 0xd7,
    0x7b, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0xad, 0xf4, 0xf6, 0xf2, 0xf3, 0xf5,
    0x7d, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0xb9, 0xfb, 0xfc, 0xf9, 0xfa, 0xff,
    0x5c, 0xf7, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xb2, 0xd4, 0xd6, 0xd2, 0xd3, 0xd5,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0xb3, 0xdb, 0xdc, 0xd9, 0xda, 0x9f,
};

struct Codepage {
    const char *name;
    const unsigned char *table;
};

const Codepage kCodepages[] = {
    {"cp037", kCP037},
    {"cp500", kCP500},
};

/* Known multi-byte sequences that real IBM i / 5250 tooling has been observed
 * to splice in where line terminators belong. Each is replaced with LF.
 * Add a new dialect by appending one entry -- no logic change required.
 *
 *   U+0082            : C1 control. The EBCDIC 0x15 "New Line" char maps here
 *                       under some EBCDIC->Unicode tables; a botched transcode
 *                       then UTF-8-encodes it to bytes C2 82. (drvuni.mbr.)
 *   ESC (0x1B) alone  : a lone escape with no 5250 following bytes; rare, but
 *                       harmless to fold to LF when it appears as a separator.
 *   ESC + 0xEB + ...  : the 5250 "ESC ë <mw> <mh>" transparent-write control.
 *                       In a UTF-8 dump 0xEB (ë) -> C3 AB and the two trailing
 *                       EBCDIC bytes -> C2 95 / C2 94, giving 1B C3 AB C2 95
 *                       C2 94. (drvuni.mbr, every C-spec line.)
 *
 * Patterns are matched literally via a Boyer-Moore-Horspool search. */
struct KnownSeparator {
    std::string_view bytes;
    const char *label;
};
const KnownSeparator kKnownSeparators[] = {
    {"\xc2\x82",            "C1 control U+0082 (EBCDIC 0x15 New Line -> UTF-8)"},
    {"\x1b\xc3\xab\xc2\x95\xc2\x94",
                            "5250 ESC transparent-write control (UTF-8)"},
};

/* -------------------------------------------------------------------------- */
/* Small byte-string helpers.                                                 */
/* -------------------------------------------------------------------------- */

bool contains(std::string_view hay, std::string_view needle) {
    return std::string::npos != hay.find(needle);
}

long count_occurrences(std::string_view hay, std::string_view needle) {
    long n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

/* Boyer-Moore-Horspool, repeated. Replaces every occurrence of `needle` in
 * `data` with `repl`. Used for separator repair. */
void replace_all(std::string &data, std::string_view needle, std::string_view repl) {
    if (needle.empty()) return;
    std::string out;
    out.reserve(data.size());
    size_t pos = 0, prev = 0;
    while ((pos = data.find(needle, prev)) != std::string::npos) {
        out.append(data, prev, pos - prev);
        out.append(repl);
        prev = pos + needle.size();
    }
    out.append(data, prev, std::string::npos);
    data = std::move(out);
}

/* -------------------------------------------------------------------------- */
/* Stage 1: strip trailing NUL padding.                                       */
/* -------------------------------------------------------------------------- */

bool detect_trailing_nuls(std::string_view data, std::string &detail) {
    // Count a trailing run of NULs. Only "fires" if the run is at least, say,
    // 4 bytes -- a stray embedded NUL is handled by the residue check, not
    // here. (IBM i member dumps pad to a record boundary, typically tens or
    // hundreds of bytes.)
    if (data.empty()) return false;
    size_t end = data.size();
    size_t start = end;
    while (start > 0 && data[start - 1] == '\0') --start;
    size_t run = end - start;
    if (run < 4) return false;
    detail = "stripped " + std::to_string(run) + " trailing NUL bytes";
    return true;
}

void apply_strip_trailing_nuls(std::string &data) {
    size_t end = data.size();
    while (end > 0 && data[end - 1] == '\0') --end;
    data.erase(end);
}

/* -------------------------------------------------------------------------- */
/* Stage 2: EBCDIC -> ASCII.                                                  */
/* -------------------------------------------------------------------------- */

/* Score how "ASCII RPG source"-like a byte buffer is, on a 0..1 scale.
 *   - printable ASCII (space..~) and tab/CR/LF are "good"
 *   - bytes >= 0x80 are "bad" (EBCDIC letters, latin-1 accents)
 *   - other control bytes (< 0x20 except tab/CR/LF) are "bad"
 * The fraction good/total is the score. */
double ascii_score(std::string_view s) {
    if (s.empty()) return 1.0;
    long good = 0;
    for (unsigned char c : s) {
        bool ok = (c == '\t' || c == '\r' || c == '\n' ||
                   (c >= 0x20 && c < 0x7f));
        if (ok) ++good;
    }
    return (double)good / (double)s.size();
}

/* Translate a copy of `data` through an EBCDIC codepage table. */
std::string translate(std::string_view data, const unsigned char *table) {
    std::string out;
    out.resize(data.size());
    for (size_t i = 0; i < data.size(); ++i)
        out[i] = (char)table[(unsigned char)data[i]];
    return out;
}

/* Pick the best EBCDIC codepage. Returns true (and sets best/best_name) if the
 * best decoded score is high enough AND clearly better than the raw ASCII
 * score -- i.e. transcoding actually helps and the result is sane. */
bool detect_ebcdic(std::string_view data, const unsigned char *&best_table,
                   std::string &best_name, std::string &detail) {
    double raw = ascii_score(data);
    // Only attempt EBCDIC if the raw buffer already looks non-ASCII. A pure
    // ASCII file would decode to garbage under EBCDIC and score worse, but we
    // avoid the wasted work and any risk of a borderline flip.
    if (raw >= 0.95) return false;

    double best_score = -1.0;
    best_table = nullptr;
    best_name.clear();
    for (const auto &cp : kCodepages) {
        std::string candidate = translate(data, cp.table);
        double sc = ascii_score(candidate);
        if (sc > best_score) {
            best_score = sc;
            best_table = cp.table;
            best_name  = cp.name;
        }
    }
    // High confidence: the decoded result is mostly printable ASCII, and
    // transcoding beat the raw bytes by a clear margin. Without the margin
    // requirement, "ASCII-with-a-few-bad-bytes" files could be mis-detected.
    const double kMinDecodedScore = 0.90;
    const double kMinMargin       = 0.10;
    if (best_score >= kMinDecodedScore && best_score >= raw + kMinMargin) {
        detail = "decoded EBCDIC " + best_name +
                 " (ascii score " + std::to_string(raw).substr(0, 4) +
                 " -> " + std::to_string(best_score).substr(0, 4) + ")";
        return true;
    }
    return false;
}

void apply_translate(std::string &data, const unsigned char *table) {
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (char)table[(unsigned char)data[i]];
}

/* -------------------------------------------------------------------------- */
/* Stage 3: separator-sequence repair.                                        */
/* -------------------------------------------------------------------------- */

bool detect_separator_seqs(std::string_view data, std::string &detail) {
    // Fire only if there are NO real newlines AND at least one known separator
    // appears. If the file already has newlines, stray control bytes are more
    // likely genuine data than record separators -- leave them to the residue
    // check.
    if (contains(data, "\n") || contains(data, "\r")) return false;
    long total = 0;
    std::string parts;
    for (const auto &ks : kKnownSeparators) {
        long n = count_occurrences(data, ks.bytes);
        if (n > 0) {
            total += n;
            if (!parts.empty()) parts += ", ";
            parts += std::to_string(n) + "x " + ks.label;
        }
    }
    if (total == 0) return false;
    detail = "replaced separator sequences (" + parts + ")";
    return true;
}

void apply_separator_repair(std::string &data) {
    for (const auto &ks : kKnownSeparators)
        replace_all(data, ks.bytes, "\n");
}

/* -------------------------------------------------------------------------- */
/* Stage 4: fixed-width no-newline split (whole-file AND per-line).           */
/*                                                                            */
/* Two cases:                                                                 */
/*   (a) The whole file has no newlines at all -- a classic S/34/S/36 card-   */
/*       image dump. Split it into `width`-byte records.                      */
/*   (b) The file is a mix: some lines already newline-terminated, others are */
/*       "blobs" (records concatenated with no separators, as in ft06.mbr).   */
/*       For each line that is much longer than a record and divides near-    */
/*       evenly by a candidate width, split just that line. Candidate widths  */
/*       are tried in order and the best fit wins: 80 (RPG II card image) and */
/*       96 (the 5250/C-spec dump width seen in drvuni/ft06-style exports),   */
/*       plus the user's --width if it isn't one of those.                   */
/*                                                                            */
/* Conservative guards: a line must be at least ~2x the candidate width AND   */
/* divide it with only a short trailing partial record (<= 1/4 width) before  */
/* we'll touch it. This avoids splitting prose or ambiguous lines.            */
/* -------------------------------------------------------------------------- */

/* The widths to try, in preference order. 80 is RPG II; 96 is the 5250 dump
 * width (96-col records); 100 is the same records with a 4-byte trailing
 * sequence/date stamp (seen in ft06.mbr's blob region). The user's --width is
 * appended if it isn't one of these. */
std::vector<int> candidate_widths(int user_width) {
    std::vector<int> w = {80, 96, 100};
    bool found = false;
    for (int x : w) if (x == user_width) { found = true; break; }
    if (!found && user_width > 0) w.push_back(user_width);
    return w;
}

/* Score a candidate width for a given line. Returns the fraction of records
 * whose column 6 is a real RPG form-type LETTER (H/F/I/C/O/E/L/U/S/D), or -1
 * if the width is infeasible.
 *
 * We count only letters (not space/comment) because drifting splits tend to
 * land on spaces and would otherwise score deceptively well. Example: a
 * 2496-byte blob of 100-byte C-spec records divides evenly by both 96 and
 * 100; splitting at 96 makes every record drift 4 bytes left, so col 6
 * becomes a space after the first record, while splitting at 100 keeps col 6
 * = 'C' for every record. Counting letters only makes 100 win decisively. */
double score_split_width(std::string_view line, int width) {
    if (width <= 0) return -1.0;
    size_t len = line.size();
    size_t nrecs = len / (size_t)width;
    if (nrecs < 2) return -1.0;
    // Allow a short trailing record up to the full width (a short last record
    // is normal); only reject a remainder that would imply we're ignoring
    // almost a whole extra record's worth of data -- but since a short last
    // record is allowed to be nearly full, we don't reject on remainder here.
    long letters = 0, total = 0;
    for (size_t i = 0; i < len; i += (size_t)width) {
        size_t end = std::min(i + (size_t)width, len);
        if (end - i < 6) break;                  // record too short to have col 6
        ++total;
        char c = line[i + 5];
        // Count letters strongly; treat space/'*' as neutral (neither helps
        // nor hurts) so a comment block doesn't get mis-split by drift either.
        if (std::isalpha((unsigned char)c)) {
            char up = (char)std::toupper((unsigned char)c);
            switch (up) {
                case 'H': case 'F': case 'I': case 'C': case 'O':
                case 'E': case 'L': case 'U': case 'S': case 'D':
                    ++letters;
                    break;
                default:
                    break;
            }
        }
    }
    if (total == 0) return -1.0;
    return (double)letters / (double)total;
}

/* Given a line, pick the candidate width whose column-6 letter score is
 * highest. Requires >= 0.75 of records to have a real spec letter at col 6 --
 * high enough that drift-induced mis-splits (which land on spaces, not
 * letters) lose, but low enough to accept a real spec block with a few
 * non-spec lines (e.g. a continuation line whose col 6 is blank). Lines that
 * are pure comment prose score ~0 on every width and are left alone. */
int best_split_width(std::string_view line, const std::vector<int> &widths) {
    int best = 0;
    double best_score = 0.75;                    // minimum confidence
    for (int w : widths) {
        double sc = score_split_width(line, w);
        if (sc > best_score) {
            best_score = sc;
            best = w;
        }
    }
    return best;
}

/* Split a single line (no embedded newline) into width-byte records, each
 * terminated by \n, padding a short trailing record with spaces. */
std::string split_one_line(std::string_view line, int width) {
    std::string out;
    out.reserve(line.size() + line.size() / width + 1);
    for (size_t i = 0; i < line.size(); i += (size_t)width) {
        size_t end = std::min(i + (size_t)width, line.size());
        size_t reclen = end - i;
        out.append(line.substr(i, reclen));
        for (size_t p = reclen; p < (size_t)width; ++p) out.push_back(' ');
        out.push_back('\n');
    }
    return out;
}

bool detect_fixed_width_split(std::string_view data, int width, std::string &detail) {
    if (data.empty()) return false;
    auto widths = candidate_widths(width);

    // Case (a): whole file has no newlines. Pick the candidate width whose
    // column-6 alignment scores best (falls back to 80 if none validate, to
    // preserve the classic fixed-80 behavior).
    if (!contains(data, "\n") && !contains(data, "\r")) {
        int w = best_split_width(data, widths);
        if (w == 0) w = 80;                      // classic default
        long nrecs = (long)((data.size() + w - 1) / (size_t)w);
        if (nrecs >= 2) {
            detail = "split whole file into " + std::to_string(nrecs) +
                     " fixed-width records of " + std::to_string(w);
            return true;
        }
        return false;
    }

    // Case (b): per-line split of long blob lines. Count how many lines would
    // be split so the detail message is informative.
    long split_lines = 0, extra_records = 0;
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t nl = data.find('\n', pos);
        std::string_view line = (nl == std::string::npos)
            ? data.substr(pos) : data.substr(pos, nl - pos);
        int w = best_split_width(line, widths);
        if (w > 0) {
            ++split_lines;
            size_t nrecs = (line.size() + w - 1) / (size_t)w;
            extra_records += (long)nrecs - 1;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (split_lines == 0) return false;
    detail = "split " + std::to_string(split_lines) + " long line(s) into " +
             std::to_string(extra_records + split_lines) +
             " fixed-width records (auto-detected width per line)";
    return true;
}

void apply_fixed_width_split(std::string &data, int width) {
    auto widths = candidate_widths(width);

    // Case (a): whole file, no newlines.
    if (!contains(data, "\n") && !contains(data, "\r")) {
        int w = best_split_width(data, widths);
        if (w == 0) w = 80;
        data = split_one_line(data, w);
        return;
    }

    // Case (b): rebuild line by line, splitting long blob lines.
    // NOTE: take a string_view over `data` and slice THAT. Slicing `data`
    // directly (data.substr(...)) returns a std::string temporary; binding a
    // string_view to that temporary dangles once the temporary destroys at the
    // end of the expression -- a heap-use-after-free when we read `line` below.
    std::string out;
    out.reserve(data.size() + data.size() / 16);
    std::string_view data_view(data);
    size_t pos = 0;
    while (pos <= data_view.size()) {
        size_t nl = data_view.find('\n', pos);
        std::string_view line = (nl == std::string::npos)
            ? data_view.substr(pos) : data_view.substr(pos, nl - pos);
        int w = best_split_width(line, widths);
        if (w > 0) {
            out += split_one_line(line, w);
        } else {
            out.append(line);
            out.push_back('\n');
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    data = std::move(out);
}

/* -------------------------------------------------------------------------- */
/* Stage 5: line-ending normalization.                                        */
/* -------------------------------------------------------------------------- */

bool detect_line_endings(std::string_view data, std::string &detail) {
    // Fire if there's at least one CR, or if the non-empty file has no LF at
    // all and isn't going to be width-split (the only legitimate no-LF case).
    // We normalize: CRLF -> LF, lone CR -> LF, and ensure the file ends with LF.
    if (data.empty()) return false;
    bool has_cr = contains(data, "\r");
    bool ends_lf = data.back() == '\n';
    bool has_any_lf = contains(data, "\n");
    if (!has_cr && ends_lf) return false;        // already clean
    if (!has_cr && !has_any_lf) return false;    // leave for width-split stage
    detail = "normalized line endings";
    return true;
}

void apply_normalize_line_endings(std::string &data) {
    // CRLF -> LF first, then any remaining lone CR -> LF.
    replace_all(data, "\r\n", "\n");
    replace_all(data, "\r", "\n");
    if (!data.empty() && data.back() != '\n') data.push_back('\n');
}

/* -------------------------------------------------------------------------- */
/* Stage 6: residue check (sets suspicious, never modifies bytes).            */
/* -------------------------------------------------------------------------- */

bool residue_is_suspicious(std::string_view data, std::string &detail) {
    if (data.empty()) return false;

    // (a) Non-printable / non-ASCII residue. After all stages, anything that
    // isn't tab/CR/LF or printable ASCII is leftover junk.
    long bad = 0;
    for (unsigned char c : data) {
        bool ok = (c == '\t' || c == '\r' || c == '\n' ||
                   (c >= 0x20 && c < 0x7f));
        if (!ok) ++bad;
    }
    double frac = (double)bad / (double)data.size();
    if (frac >= 0.01) {
        detail = std::to_string(bad) + " non-printable/non-ASCII bytes remain (" +
                 std::to_string(frac * 100.0).substr(0, 4) + "%)";
        return true;
    }

    // (b) Absurdly long lines. Real RPG fixed-format source is <= 100 columns
    // (RPG IV) or <= 80 (RPG II). A line much longer than that after cleaning
    // means a region didn't split on separators and didn't divide cleanly by
    // the fixed width -- the drvuni.mbr middle "blob" case, where the comment
    // header + H/F/D specs were concatenated with no recoverable record
    // boundaries. We can't safely guess where to cut it, so we flag it.
    const size_t kMaxReasonableLine = 120;
    size_t longest = 0, nl_count = 0;
    for (size_t i = 0, start = 0; i <= data.size(); ++i) {
        if (i == data.size() || data[i] == '\n') {
            size_t len = i - start;
            if (len > longest) longest = len;
            if (i < data.size()) ++nl_count;
            start = i + 1;
        }
    }
    if (longest > kMaxReasonableLine) {
        detail = "longest line is " + std::to_string(longest) + " bytes " +
                 "(expected <=~100 for fixed-format RPG; record boundaries " +
                 "may be unrecoverable in part of this file)";
        return true;
    }
    return false;
}

} // namespace

/* -------------------------------------------------------------------------- */
/* Public driver.                                                             */
/* -------------------------------------------------------------------------- */

CleanReport clean_source_bytes(std::string &data, int width) {
    CleanReport rep;

    {   // Stage 1: trailing NULs.
        std::string d;
        if (detect_trailing_nuls(data, d)) {
            apply_strip_trailing_nuls(data);
            rep.notes.push_back(std::move(d));
        }
    }
    {   // Stage 2: EBCDIC.
        std::string d, name;
        const unsigned char *tab = nullptr;
        if (detect_ebcdic(data, tab, name, d)) {
            apply_translate(data, tab);
            rep.notes.push_back(std::move(d));
        }
    }
    {   // Stage 3: separator sequences.
        std::string d;
        if (detect_separator_seqs(data, d)) {
            apply_separator_repair(data);
            rep.notes.push_back(std::move(d));
        }
    }
    {   // Stage 4: fixed-width split.
        std::string d;
        if (detect_fixed_width_split(data, width, d)) {
            apply_fixed_width_split(data, width);
            rep.notes.push_back(std::move(d));
        }
    }
    {   // Stage 5: line endings.
        std::string d;
        if (detect_line_endings(data, d)) {
            apply_normalize_line_endings(data);
            rep.notes.push_back(std::move(d));
        }
    }
    {   // Stage 6: residue.
        std::string d;
        if (residue_is_suspicious(data, d)) {
            rep.notes.push_back("suspicious: " + std::move(d));
            rep.suspicious = true;
        }
    }
    return rep;
}

} // namespace rpgc
