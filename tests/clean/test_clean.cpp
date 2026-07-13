// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * test_clean.cpp -- unit tests for the cleanup pipeline (compiler/src/clean.cpp).
 *
 * Hand-rolled assert style: no external test framework, matching the project's
 * stdlib-only ethos. Each CHECK_* prints a one-line pass/fail; `failures`
 * accumulates and main() returns nonzero if any failed.
 *
 * Run:   ./build/tests/clean/test_clean   (built with -DBUILD_TESTING=ON)
 * ========================================================================== */
#include "clean.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace rpgc;

namespace {

int failures = 0;
int checks   = 0;

void check(bool cond, const char *expr, const char *where) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  \033[31mFAIL\033[0m %s: %s\n", where, expr);
    } else {
        std::printf("  \033[32mok\033[0m   %s\n", where);
    }
}
#define CHECK(cond) check((cond), #cond, __func__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __func__)

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

long count(const std::string &h, char c) {
    long n = 0;
    for (char x : h) if (x == c) ++n;
    return n;
}

/* -------------------------------------------------------------------------- */
/* Stage 1: trailing NUL padding.                                             */
/* -------------------------------------------------------------------------- */
void test_strips_trailing_nuls() {
    std::string data = "hello\nworld\n";
    data.append(20, '\0');
    CleanReport r = clean_source_bytes(data);
    CHECK_EQ(data, std::string("hello\nworld\n"));
    CHECK(r.notes.size() == 1);
    CHECK(contains(r.notes[0], "stripped 20 trailing NUL"));
}

void test_leaves_short_nul_runs_alone() {
    // A run of < 4 is not "padding"; leave it to the residue check.
    std::string data = "hello\n";
    data.append(2, '\0');
    CleanReport r = clean_source_bytes(data);
    // The 2 trailing NULs survive stages but the residue check flags them.
    CHECK(contains(data, "hello"));
    CHECK(r.suspicious);
}

/* -------------------------------------------------------------------------- */
/* Stage 2: EBCDIC.                                                           */
/* -------------------------------------------------------------------------- */
void test_decodes_ebcdic_cp037() {
    // Build an EBCDIC-encoded RPG comment line + a C-spec.
    // ASCII -> cp037 lookup (inverse of the embedded table). We use Python's
    // mapping by hand-rolling the few characters we need.
    // 'A' (0x41) -> cp037 0xC1, ' ' (0x20) -> 0x40, '*' (0x2A) -> 0x5C,
    // 'C' (0x43) -> 0xC3, '\n' (0x0A) -> 0x25, digits/letters below.
    auto to_cp037 = [](char c) -> unsigned char {
        switch (c) {
            case ' ': return 0x40;  case '*': return 0x5C;
            case '\n': return 0x25; case 'C': return 0xC3;
            case 'E': return 0xC5;  case 'V': return 0xE5;
            case 'A': return 0xC1;  case 'L': return 0xD3;
            case 'h': return 0x88;  case 'i': return 0x89;
            case 's': return 0xA2;  case '1': return 0xF1;
            case '=': return 0x7E;
            default: return 0x40; // space fallback
        }
    };
    std::string ascii_src =
        "     * this is a test\n"
        "     C                   EVAL      A = 1\n";
    std::string ebcdic;
    for (char c : ascii_src) ebcdic.push_back((char)to_cp037(c));

    CleanReport r = clean_source_bytes(ebcdic);
    CHECK(contains(r.notes[0], "decoded EBCDIC cp037"));
    // The decoded output should be all-printable ASCII.
    for (unsigned char c : ebcdic) {
        CHECK(c == '\n' || (c >= 0x20 && c < 0x7f));
    }
    CHECK(contains(ebcdic, "EVAL"));
}

void test_leaves_ascii_alone() {
    // A pure-ASCII file must NOT be mis-detected as EBCDIC.
    std::string data = "     C                   EVAL      A = 1\n";
    std::string orig = data;
    CleanReport r = clean_source_bytes(data);
    CHECK(data == orig);
    // No EBCDIC note should appear.
    for (const auto &n : r.notes)
        CHECK(!contains(n, "EBCDIC"));
}

/* -------------------------------------------------------------------------- */
/* Stage 3: separator sequences.                                             */
/* -------------------------------------------------------------------------- */
void test_replaces_5250_separators() {
    // Two records joined by the 5250 ESC control, no real newlines. After
    // separator repair the records are newline-separated, and the line-ending
    // stage adds the trailing newline -> 2 total.
    std::string data;
    data += "     C                   EVAL      A = 1";
    data += "\x1b\xc3\xab\xc2\x95\xc2\x94";           // 5250 sep
    data += "     C                   EVAL      B = 2";
    CleanReport r = clean_source_bytes(data);
    bool found_sep_note = false;
    for (const auto &n : r.notes)
        if (contains(n, "5250")) { found_sep_note = true; break; }
    CHECK(found_sep_note);
    CHECK(count(data, '\n') == 2);
    CHECK(contains(data, "A = 1"));
    CHECK(contains(data, "B = 2"));
}

void test_replaces_u0082_separators() {
    std::string data;
    data += "     C                   EVAL      A = 1";
    data += "\xc2\x82";                                 // U+0082 (EBCDIC NL)
    data += "     C                   EVAL      B = 2";
    CleanReport r = clean_source_bytes(data);
    CHECK(count(data, '\n') == 2);
    CHECK(contains(data, "A = 1"));
}

void test_no_separator_stage_when_newlines_present() {
    // If the file already has newlines, stray control bytes are not treated
    // as separators.
    std::string data = "line one\nline\x1b\xc3\xab\xc2\x95\xc2\x94 two\n";
    std::string orig = data;
    CleanReport r = clean_source_bytes(data);
    // The separator stage must not have fired (newlines present), so the ESC
    // bytes survive and the residue check flags the file as suspicious.
    bool sep_fired = false;
    for (const auto &n : r.notes)
        if (contains(n, "separator")) { sep_fired = true; break; }
    CHECK(!sep_fired);
    CHECK(r.suspicious);
}

/* -------------------------------------------------------------------------- */
/* Stage 4: fixed-width no-newline split.                                    */
/* -------------------------------------------------------------------------- */
void test_splits_fixed80() {
    std::string rec = "     C                   EVAL      A = 1";
    rec.resize(80, ' ');
    std::string data = rec + rec + rec;                 // 240 bytes, no newlines
    CleanReport r = clean_source_bytes(data);
    CHECK(count(data, '\n') == 3);
    // Each line is exactly 80 chars + newline.
    CHECK(data.size() == 3 * 81);
}

void test_no_split_when_newlines_present() {
    std::string data = "short line\n";
    CleanReport r = clean_source_bytes(data);
    CHECK(count(data, '\n') == 1);
}

void test_splits_per_line_blobs() {
    // A hybrid file: some lines already newline-terminated, one line is a
    // "blob" of width-100 C-spec records concatenated with no separators (the
    // ft06.mbr signature). The split stage must split just the blob line,
    // leaving the already-newlined lines intact, and pick width 100 over 96
    // because 100 keeps column 6 aligned (96 would drift).
    std::string data;
    data += "     C                   EVAL      PRE = 1\n";   // a clean 96-col line
    // blob: 3 records of width 100, content at col 6 = 'C'
    std::string rec1 = "     C                   EVAL      A = 1"; rec1.resize(100, ' ');
    std::string rec2 = "     C                   EVAL      B = 2"; rec2.resize(100, ' ');
    std::string rec3 = "     C                   EVAL      C = 3"; rec3.resize(100, ' ');
    data += rec1 + rec2 + rec3;                              // 300-byte blob, no newline
    CleanReport r = clean_source_bytes(data);
    // The blob (3 records) is split into 3 lines; the clean line stays; plus
    // the line-ending stage adds a trailing newline. So >= 4 newlines total.
    CHECK(count(data, '\n') >= 4);
    // Every recovered record must have 'C' at column 6 (no drift).
    bool all_c = true;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        std::string line = (nl == std::string::npos) ? data.substr(pos) : data.substr(pos, nl - pos);
        if (line.size() >= 6 && line.substr(0, 5) == "     ") {
            if (line[5] != 'C') { all_c = false; break; }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    CHECK(all_c);
}

void test_does_not_split_comment_prose_blob() {
    // A blob of comment prose (no spec letters at col 6) must NOT be split --
    // there's no column-6 signal to pick a width, so splitting would be a
    // guess. Leave it intact and let the residue check flag it suspicious.
    std::string data;
    data += "     C                   EVAL      A = 1\n";
    std::string prose(500, ' ');
    for (size_t i = 0; i < prose.size(); ++i) prose[i] = 'A' + (i % 26);
    data += prose;                                           // 500-byte prose blob
    CleanReport r = clean_source_bytes(data);
    CHECK(r.suspicious);                                    // long line flagged
    // The prose blob must survive as one line (not mis-split).
    CHECK(count(data, '\n') <= 2);
}

/* -------------------------------------------------------------------------- */
/* Stage 5: line endings.                                                    */
/* -------------------------------------------------------------------------- */
void test_normalizes_crlf() {
    std::string data = "line one\r\nline two\r\n";
    CleanReport r = clean_source_bytes(data);
    CHECK(count(data, '\r') == 0);
    CHECK(count(data, '\n') == 2);
}

void test_normalizes_lone_cr() {
    std::string data = "line one\rline two\r";
    CleanReport r = clean_source_bytes(data);
    CHECK(count(data, '\r') == 0);
    CHECK(count(data, '\n') == 2);
}

/* -------------------------------------------------------------------------- */
/* Stage 6: residue / suspicious.                                            */
/* -------------------------------------------------------------------------- */
void test_flags_non_printable_residue() {
    std::string data(100, 'A');                         // 100 printable
    for (int i = 0; i < 10; ++i) data[i] = '\x99';     // 10% junk, no trailing run
    data += "\n";
    CleanReport r = clean_source_bytes(data);
    CHECK(r.suspicious);
}

void test_flags_absurdly_long_line() {
    // A 500-char line with newlines is structurally suspect for fixed-format RPG.
    std::string data(500, 'A');
    data += "\n";
    CleanReport r = clean_source_bytes(data);
    CHECK(r.suspicious);
}

/* -------------------------------------------------------------------------- */
/* Idempotency: running clean twice must be a no-op the second time.         */
/* -------------------------------------------------------------------------- */
void test_idempotent_on_clean_output() {
    std::string rec = "     C                   EVAL      A = 1";
    rec.resize(80, ' ');
    std::string data = rec + rec + rec;
    CleanReport r1 = clean_source_bytes(data);
    std::string after_first = data;
    CleanReport r2 = clean_source_bytes(data);
    CHECK(data == after_first);
    CHECK(r2.notes.empty());
    CHECK(!r2.suspicious);
}

/* -------------------------------------------------------------------------- */
/* Integration: a clean .rpg file is byte-identical after cleaning.          */
/* -------------------------------------------------------------------------- */
void test_clean_file_is_unchanged() {
    std::string data =
        "     * comment line\n"
        "     C                   EVAL      A = 1\n"
        "     C                   EVAL      B = 2\n";
    std::string orig = data;
    CleanReport r = clean_source_bytes(data);
    CHECK(data == orig);
    CHECK(r.notes.empty());
    CHECK(!r.suspicious);
}

} // namespace

int main() {
    std::printf("--- Stage 1: trailing NUL padding ---\n");
    test_strips_trailing_nuls();
    test_leaves_short_nul_runs_alone();
    std::printf("--- Stage 2: EBCDIC ---\n");
    test_decodes_ebcdic_cp037();
    test_leaves_ascii_alone();
    std::printf("--- Stage 3: separator sequences ---\n");
    test_replaces_5250_separators();
    test_replaces_u0082_separators();
    test_no_separator_stage_when_newlines_present();
    std::printf("--- Stage 4: fixed-width split ---\n");
    test_splits_fixed80();
    test_no_split_when_newlines_present();
    test_splits_per_line_blobs();
    test_does_not_split_comment_prose_blob();
    std::printf("--- Stage 5: line endings ---\n");
    test_normalizes_crlf();
    test_normalizes_lone_cr();
    std::printf("--- Stage 6: residue ---\n");
    test_flags_non_printable_residue();
    test_flags_absurdly_long_line();
    std::printf("--- idempotency & clean-file ---\n");
    test_idempotent_on_clean_output();
    test_clean_file_is_unchanged();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
