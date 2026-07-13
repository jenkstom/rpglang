// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * ispec.h -- parse Input Specifications.
 *
 * An I-spec section for a file begins with a record-identification line and
 * is followed by zero or more field-description lines. We flatten these into:
 *   - one ISpecRec  per record-identification line, and
 *   - one ISpecField per field-description line, each tagged with its file.
 *
 * Phase 3 uses only: the record-identifying indicator, and for each field the
 * From/To record positions, the decimal-position count, and the field name.
 * ========================================================================== */
#ifndef RPGC_ISPEC_H
#define RPGC_ISPEC_H

#include "source.h"
#include <string>
#include <vector>

namespace rpgc {

/* One character-match in a record-identification code (E17). A record-id
 * line carries up to three of these; an AND/OR continuation line adds more.
 * `pos` is the 1-based record position; 0 marks an unused set. */
struct RecCodeSet {
    int  pos = 0;        // cols 21-24 / 28-31 / 35-38 (record position)
    bool negate = false; // cols 25 / 32 / 39 == 'N'
    char czd = 'C';      // cols 26 / 33 / 40: C=char, Z=zone, D=digit
    char ch = ' ';       // cols 27 / 34 / 41: the literal character to match
};

/* Record-identification line. */
struct ISpecRec {
    int         lineno = 0;
    std::string name;                 // cols 7-14 (the owning file)
    int         rec_indicator = 0;    // cols 19-20 (01-99); 0 if blank/special
    std::vector<RecCodeSet> codes;    // cols 21-41 record-identification codes (E17)
    bool is_lookahead = false;        // cols 19-20 == '**' (look-ahead marker, E19)
};

/* Field-description line. */
struct ISpecField {
    int         lineno = 0;
    std::string file;                 // the file this field belongs to
    int         from = 0;             // cols 44-47 (1-based record position)
    int         to   = 0;             // cols 48-51
    int         decimals = -1;        // col 52 (-1 = alphameric)
    char        data_format = 0;      // col 43: 0=blank(zoned), 'P'=packed, 'B'=binary
    std::string name;                 // cols 53-58
    std::string control_level;        // cols 59-60 (L1-L9)
    std::string match_field;          // cols 61-62 (M1-M9 matching field, Section F)
    int         record_id = 0;        // cols 63-64: record-identifying indicator (E17)
    int         plus_ind  = 0;        // cols 65-66: field indicator on + (E18)
    int         minus_ind = 0;        // cols 67-68: field indicator on - (E18)
    int         zero_ind  = 0;        // cols 69-70: field indicator on 0/blank (E18)
};

/* Data structure statement (D2, manual Ch. 15, cols 19-20 == "DS"). Must be
 * the last entries on the input specifications. */
struct ISpecDS {
    int         lineno = 0;
    std::string name;       // cols 7-12, may be blank (anonymous DS)
    bool        is_lda = false;   // col 18 == 'U': local data area for a
                                   // display station (WORKSTN). This project
                                   // implements SRT (single requester
                                   // terminal) programs only -- see WORKSTN
                                   // support notes in docs/ARCHITECTURE.md --
                                   // so "per device" degenerates to "per
                                   // program": codegen backs this with one
                                   // ordinary global buffer, same as any
                                   // other DS (codegen.cpp's
                                   // declare_data_structures).
};

/* W3: which INFDS keyword (manual "Coding the INFDS Data Structure") a
 * subfield line's cols 44-51 named, if any. None means an ordinary
 * numeric-position subfield. */
enum class InfdsKeyword { None, Status, Opcode, Record, Size, Mode, Inp, Out };

/* One subfield line under a data structure statement. `ds_index` identifies
 * the owning ISpecDS by position in ISpecs::data_structures (not by name --
 * a DS statement's name column may be blank, and name-based lookup can't
 * disambiguate two anonymous data structures in the same program). From/To
 * are positions within the data structure, not the input record.
 *
 * W3: an INFDS subfield line names a keyword (*STATUS/*OPCODE/*RECORD/
 * *SIZE/*MODE/*INP/*OUT) in cols 44-51 instead of a numeric From position
 * (manual: "columns 44 through 51 must contain a keyword that identifies
 * the location of the subfields"). This compiler auto-assigns From/To for
 * keyword subfields sequentially in declaration order within their DS
 * (there is no real predefined System/36 byte layout to match here, since
 * INFDS is entirely this project's own runtime-backed data structure, not
 * a byte-for-byte port) -- ordinary numeric-position subfields keep
 * whatever position the source gives them, same as always. */
struct ISpecSubfield {
    int         lineno = 0;
    int         ds_index = -1;
    int         from = 0;    // cols 44-47, relative to the DS start
    int         to   = 0;    // cols 48-51
    int         decimals = -1;   // col 52 (-1 = alphameric)
    std::string name;        // cols 53-58
    InfdsKeyword infds_kw = InfdsKeyword::None;
};

struct ISpecs {
    std::vector<ISpecRec>   records;
    std::vector<ISpecField> fields;
    std::vector<ISpecField> lookahead_fields;   // fields under a '**' line (E19)
    std::vector<ISpecDS>       data_structures; // D2
    std::vector<ISpecSubfield> ds_subfields;    // D2
};

/* Parse all I-specs. Maintains the "current file" as record-identification
 * lines are seen, so field lines inherit the right owner. */
ISpecs parse_ispecs(const std::vector<SourceLine> &src);

} // namespace rpgc

#endif // RPGC_ISPEC_H
