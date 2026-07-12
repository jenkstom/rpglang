// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * cmds/cmds.h -- the utility subcommands (TOOLS_IDEAS.md §8), each a small
 * self-contained function over one or more ProgramIRs. Unlike the report
 * modules, these produce a different artifact (decoded columns, a query
 * match list, a semantic diff, lint findings, a Markdown page) rather than a
 * Section/Finding pair, so they own their own text output and exit code.
 * ========================================================================== */
#ifndef RPGANALYZE_CMDS_H
#define RPGANALYZE_CMDS_H

#include <ostream>
#include <string>
#include <vector>

namespace analyze {

struct DecodeOptions {
    int line = 0;          // 0 = no single-line filter
    std::string range;     // "A-B", empty = whole file
};
int cmd_decode(const std::vector<std::string> &files, const DecodeOptions &opts, std::ostream &out);

int cmd_search(const std::vector<std::string> &files, const std::string &query, std::ostream &out);

int cmd_diff(const std::vector<std::string> &files, std::ostream &out);

int cmd_format(const std::vector<std::string> &files, bool no_color, std::ostream &out);

int cmd_docgen(const std::vector<std::string> &files, std::ostream &out);

int cmd_callgraph(const std::vector<std::string> &files, bool dot, std::ostream &out);

int cmd_duplicate(const std::vector<std::string> &files, double threshold, std::ostream &out);

int cmd_portfolio(const std::vector<std::string> &files, bool html, std::ostream &out);

int cmd_migrate(const std::vector<std::string> &files, std::ostream &out);

} // namespace analyze

#endif // RPGANALYZE_CMDS_H
