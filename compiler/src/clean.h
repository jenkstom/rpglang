// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * clean.h -- normalize "mangled" RPG source bytes into clean LF-terminated
 *            ASCII lines.
 *
 * RPG source pulled off midrange systems (S/34, S/36, AS/400, IBM i) often
 * arrives in a form that is valid RPG *content* but damaged as a *file*:
 *
 *   - fixed-length 80-column records with no line terminators at all
 *     (S/34/S/36 card-image dumps), so the whole file reads as one line;
 *   - EBCDIC-encoded text (the native encoding of every system above);
 *   - line terminators replaced by stray control/C1 sequences produced by a
 *     botched EBCDIC->ASCII transcode (EBCDIC 0x15 "New Line" turned into the
 *     C1 control U+0082, i.e. UTF-8 bytes C2 82) or by 5250 workstation
 *     control sequences (ESC + 0xEB + ...) that got embedded in the source;
 *   - trailing NUL padding to a fixed record/file boundary.
 *
 * Any one of these breaks the compiler's `load_source` (which uses
 * `std::getline` and assumes plain ASCII lines) and every editor's highlighter.
 *
 * `clean_source_bytes` is a pure byte->byte normalizer that runs an ordered
 * pipeline of small, independently-detectable fix stages. Each stage detects
 * whether its issue is present and only applies if so; every stage that fires
 * appends a short human-readable note to the returned CleanReport so callers
 * can surface what changed. It is deliberately conservative: if a stage cannot
 * classify the input with confidence it leaves the bytes alone and flags the
 * file as `suspicious` so a human can look.
 *
 * Adding a new fix later is three local edits in clean.cpp: write a detect_*
 * helper, write an apply_* helper, and add one entry to the pipeline. The
 * separator-sequence stage is data-driven (a table of known byte patterns), so
 * a new separator dialect is a one-line table entry with no logic change.
 *
 * The compiler and analyzer both get cleanup for free because `load_source`
 * (the single chokepoint every source-reading code path funnels through) calls
 * this before splitting into SourceLine records. A standalone `rpg-clean` CLI
 * links rpgc_parse and calls it directly for on-disk repair.
 * ========================================================================== */
#ifndef RPGC_CLEAN_H
#define RPGC_CLEAN_H

#include <string>
#include <vector>

namespace rpgc {

/* What `clean_source_bytes` found and did. `notes` has one short entry per
 * stage that fired (e.g. "stripped 60 trailing NUL bytes", "decoded EBCDIC
 * cp037", "replaced 70 5250 separator sequences"). `suspicious` is set when a
 * stage could not classify the input confidently, or when a noticeable residue
 * of non-printable bytes remains after all stages -- the bytes were still
 * returned, but a human should look. */
struct CleanReport {
    std::vector<std::string> notes;
    bool suspicious = false;
};

/* Default fixed-record width for the no-newline splitting stage (column 1-80
 * card image). */
constexpr int kDefaultCleanWidth = 80;

/* Normalize raw source bytes in place and report what changed. `data` is
 * modified in place -- the caller owns the storage -- because load_source
 * needs both the cleaned bytes and the report, and the cleanup is the last
 * thing that touches the buffer before line-splitting. Pure otherwise: no I/O,
 * no diagnostics side effects. The returned CleanReport describes what (if
 * anything) each fired stage did. */
CleanReport clean_source_bytes(std::string &data,
                               int width = kDefaultCleanWidth);

} // namespace rpgc

#endif // RPGC_CLEAN_H
