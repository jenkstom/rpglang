// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * rpg_runtime.h -- Public API of the C runtime that RPG-generated programs
 *                  link against (librpgruntime.a).
 *
 * In Phase 1 the runtime is intentionally tiny: it only provides a tiny
 * entry helper so that we can prove the compile-link-run loop works end to
 * end. Later phases will add:
 *   - file description management (open/close keyed & sequential files),
 *   - the read-record hook used by the RPG cycle,
 *   - numeric/character conversion helpers (ZONED -> binary, etc.),
 *   - standard built-in functions (%SUBST, %TRIM, ...).
 *
 * All exported symbols use the `rpg_rt_` prefix to avoid colliding with the
 * generated program's own names.
 * ========================================================================== */

#ifndef RPG_RUNTIME_H
#define RPG_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Phase 1: smoke-test helper ---------------------------------------- */

/* Returns a fixed version string. Used only to prove that generated code can
 * call into the runtime library. */
const char *rpg_rt_version(void);

/* ----- Forward declarations for later phases ----------------------------- */
/* (implemented in Phase 3+) */

/* Open a sequential input file (newline-delimited text records). Returns a
 * non-negative file id on success or a negative value on failure. */
int rpg_rt_open_input(const char *path);

/* Read the next fixed-length record into `buf` (NUL-terminated). The record is
 * presented as exactly `reclen` characters: bytes are read up to `reclen`, and
 * if a newline (LF, or CR/CRLF) is hit first it is consumed and the remainder
 * of the window is space-padded. Lines longer than `reclen` are split across
 * successive records. `buf` must hold at least reclen+1 bytes. Returns 1 if a
 * record was read, 0 on EOF. */
int rpg_rt_read_next(int file_id, char *buf, size_t buflen);

/* Peek at the next record without consuming it (Section E, E19 look-ahead).
 * Reads the record into a per-file cache and copies it to `buf`; the next
 * rpg_rt_read_next returns the cached record. Returns 1 on record, 0 on EOF. */
int rpg_rt_peek_next(int file_id, char *buf, size_t buflen);

/* Set the declared record length for an open file; used by read_next to make
 * each record a fixed `reclen` characters. `reclen` <= 0 falls back to
 * line-based reading. */
void rpg_rt_set_reclen(int file_id, int reclen);

/* ----- Section G (G24): keyed / random file access ------------------------ */

/* Declare the key field for an indexed DISK file: `key_start` is the 1-based
 * record column where the key begins, `key_len` its length. Called once after
 * the file is opened. With no key set, CHAIN falls back to relative-record-
 * number access. */
void rpg_rt_set_key(int file_id, int key_start, int key_len);

/* Format `value` as a zero-padded decimal string of width `width` into `out`
 * (right-justified). Returns the width. Used to build a numeric key buffer. */
int rpg_rt_fmt_key(long value, int width, char *out);

/* Random read of one record (CHAIN). If a key is declared, `key` (keylen bytes)
 * is binary-searched in the file's key index; record N (1-based) is returned.
 * Without a key, `key` holds a decimal RRN. Returns 1 on a hit (record in buf),
 * 0 on no-record. */
int rpg_rt_chain(int file_id, const char *key, int keylen, char *buf, size_t buflen);

/* Position an indexed file at the first record whose key >= `key` (SETLL). A
 * subsequent rpg_rt_read / rpg_rt_reade reads from there. Returns 1. */
int rpg_rt_setll(int file_id, const char *key, int keylen);

/* Read the next record from the current position (full-procedural / demand
 * READ). Returns 1 on record, 0 on EOF. */
int rpg_rt_read(int file_id, char *buf, size_t buflen);

/* Read the next record only if its key == `key` (READE); otherwise return 0
 * (unequal / EOF) without advancing past the non-matching record. */
int rpg_rt_reade(int file_id, const char *key, int keylen, char *buf, size_t buflen);

/* Read the prior record relative to the current position (READP), moving the
 * cursor backward. Returns 1 on record, 0 on beginning-of-file (no prior
 * record) -- per the manual, the file must then be repositioned with SETLL
 * or CHAIN before further reads. */
int rpg_rt_readp(int file_id, char *buf, size_t buflen);

/* ----- Section G (G25): update files -------------------------------------- */

/* Open an update file (type U): read+write in place, no truncate. */
int rpg_rt_open_update(const char *path);

/* Append a record (O-spec ADD). */
void rpg_rt_write_rec(int file_id, const char *buf, int len);

/* Rewrite the most recently read record in place (O-spec UPDATE). */
void rpg_rt_update_rec(int file_id, const char *buf, int len);

/* Delete the most recently read record (O-spec DEL): fill it with 0xFF. */
void rpg_rt_delete_rec(int file_id);

/* Flush the current line buffer as a disk record. op 0 = ADD (append), 1 =
 * UPDATE (in-place). Reuses the printer field-placement logic. */
void rpg_rt_flush_rec(int file_id, int op);

/* Open a sequential output file (text, line-buffered). Returns a file id or
 * -1 on failure. The file is truncated on open. */
int rpg_rt_open_output(const char *path);

/* Begin a new output line buffer of `width` spaces. The runtime owns the
 * buffer; finish a line with rpg_rt_emit_line(). width is the record length. */
void rpg_rt_line_begin(int width);

/* Place a fixed string `s` (len chars) so its LAST character lands at 1-based
 * `end_pos` on the current line (right-justified placement, RPG style). Left
 * pad is the existing spaces in the buffer. */
void rpg_rt_line_put_str(const char *s, int len, int end_pos);

/* Place a numeric value as ASCII decimal, right-justified to `end_pos`. */
void rpg_rt_line_put_num(long value, int end_pos);

/* Like rpg_rt_line_put_num, but `value` is a scaled integer with `decimals`
 * fractional digits (Section C). decimals<=0 behaves like line_put_num. */
void rpg_rt_line_put_num_dec(long value, int end_pos, int decimals);

/* UDATE (Auto Report Ch. 26): the current date formatted "mm/dd/yy" (8 chars),
 * placed right-justified to `end_pos` -- the date field of a generated H-*AUTO
 * page heading. */
void rpg_rt_line_put_date(int end_pos);

/* Emit the current line buffer to `file_id`, followed by `space_after` newlines
 * (default 1). Then clears the buffer. */
void rpg_rt_emit_line(int file_id, int space_after);

/* DEBUG (Chapter 27): build and write the fixed-format DEBUG record(s) to
 * `file_id` (an already-open output file with record length `reclen`),
 * reusing the line_begin/line_put_str/emit_line machinery above.
 *
 * Record 1 (always written): "DEBUG = " (cols 1-8), the statement number
 * `stmtno` (cols 9-18), `label` (cols 19-26, factor 1's contents, truncated
 * to 8 chars; label_len 0 = none), "INDICATORS ON = " (cols 29-44), then the
 * 2-digit names of every on indicator in `ind_on` (an `nind`-element 0/1
 * array, index i-1 = indicator i), space-separated starting at col 45,
 * wrapping to additional records when a name doesn't fit.
 *
 * Record 2 (only when field_len > 0): "FIELD VALUE = " (cols 1-14) followed
 * by `field_val`'s bytes, wrapping across further records (no leading
 * literal on continuation records) when longer than one line -- this
 * compiler's own interpretation of the manual's "more than one output
 * record may be needed to contain the array", which gives no explicit
 * continuation-record layout. */
void rpg_rt_debug_write(int file_id, int reclen, long stmtno,
                        const char *label, int label_len,
                        const unsigned char *ind_on, int nind,
                        const char *field_val, int field_len);

/* Skip `file_id`'s output to absolute line `line_no` on the page (Section D,
 * D13). A skip to a line at or before the current position starts a new page
 * (form-feed, page counter incremented). */
void rpg_rt_skip(int file_id, int line_no);

/* Return the current page number. `which` 0 => `file_id`'s own page; 1..7 =>
 * the page of the nth-opened output file (PAGE1-PAGE7). Section D (D14). */
int rpg_rt_page(int file_id, int which);

/* Configure printer overflow for `file_id` (Section F, F22). `lines_per_page`
 * is the form depth (default 66); `overflow_line` is the line at which overflow
 * occurs (default 60, the manual's "six from the bottom"). Called once after a
 * PRINTER file is opened when the program assigns an overflow indicator. */
void rpg_rt_set_overflow(int file_id, int lines_per_page, int overflow_line);

/* Return 1 if the overflow line was reached since the last call, then clear the
 * latch (Section F, F22). The cycle polls this at total time to drive the
 * overflow indicator (OA-OG/OV). */
int rpg_rt_take_overflow(int file_id);

/* Close every opened file. Called on LR (last record). */
void rpg_rt_close_all(void);

/* Compare two fixed-length character buffers (left-aligned, blank-padded to the
 * longer length). Returns -1/0/1 (a<b, a==b, a>b). Phase 10. */
int rpg_rt_cmp_str(const char *a, int alen, const char *b, int blen);

/* LOKUP a numeric `key` in `arr` (count elements). Starts at index *idx (1-based)
 * and scans. On an equal match, sets *idx to that element and returns 0.
 * Otherwise finds the NEAREST qualifying element (manual 113147-113162), not
 * merely "does one exist": if a higher element exists, returns +1 and sets
 * *idx to the nearest one; if a lower element exists, returns -1 and sets
 * *idx to the nearest one (A11). Else returns -2 and sets *idx to 1. `ascending`
 * (nonzero for E-spec column 45 == 'A', zero for 'D'/unspecified) determines
 * which end of a scan run is "nearest" -- see B2 for where this flag is read
 * from the E-spec parse. Phase 10 / A11. */
int rpg_rt_lokup(long key, const int *arr, int count, int *idx, int ascending);

/* SORTA (Group C, C4): sort a numeric array of `count` elements in place.
 * `ascending` nonzero for E-spec column 45 == 'A' (or unspecified), zero for
 * 'D' -- the same flag rpg_rt_lokup reads (B2). */
void rpg_rt_sorta(int *arr, int count, int ascending);

/* TIME (Group C, C5): current time-of-day as a 6-digit hhmmss integer. */
long rpg_rt_time(void);

/* Format a numeric value into `out` (capacity out_cap) using an RPG II edit
 * code (codes '1'-'4', 'A'-'D', 'J'-'M', 'N', 'O'). Returns the string length.
 * The result is right-justified into `width` characters if width > 0.
 * Phase 10. */
int rpg_rt_edit(long value, char edit_code, int width, char *out, int out_cap);

/* Like rpg_rt_edit, but the stored value is a scaled integer whose last
 * `decimals` digits are the fractional part (Section C). decimals<=0 behaves
 * like rpg_rt_edit. `fill` (A13) is a floating fill character from O-spec
 * columns 45-47 -- a currency symbol or '*' -- printed immediately to the
 * left of the first digit; 0 means none. Returns the string length. */
int rpg_rt_edit_dec(long value, char edit_code, int width, int decimals,
                    char fill, char *out, int out_cap);

/* Format `value` (a scaled integer with `decimals` fractional digits) using an
 * RPG II edit word (Section D, D16). The word's blanks are replaceable (filled
 * by source digits), the first '0' stops zero-suppression, the first '*' does
 * check-protection, a trailing '-' or "CR" is a sign, '&' forces a blank. A
 * `currency` character directly followed by '0' floats to just left of the
 * first significant digit instead of printing at its own position (A13,
 * manual 63666-63669; D1: the character is now the H-spec col 18 currency
 * symbol, defaulting to '$', instead of a hardcoded '$'). Returns the string
 * length. */
int rpg_rt_edit_word(long value, const char *word, int word_len,
                     int decimals, char currency, char *out, int out_cap);

/* Extract a numeric field from a fixed-length record buffer. `from`/`to` are
 * 1-based inclusive record positions. The columns are decoded as plain ASCII
 * digits and parsed as a non-negative integer; blank/non-digit columns read as
 * 0 (leading/embedded blanks are skipped). Phase 3 ASCII-digit convention. */
long rpg_rt_get_decimal(const char *rec, int reclen, int from, int to);

/* Decode a packed-decimal field (I-spec col 43 = P) spanning record bytes
 * [from..to] (1-based): two BCD digits per byte, sign nibble (F=+, D=-) in the
 * low-order byte. Returns the decoded value. Section C (C9). */
long rpg_rt_get_packed(const char *rec, int reclen, int from, int to);

/* Decode a binary field (I-spec col 43 = B) spanning record bytes [from..to]
 * (1-based): big-endian; 2-byte = int16 sign-extended, 4-byte = int32. Section C. */
long rpg_rt_get_binary(const char *rec, int reclen, int from, int to);

/* Sign-overpunch decode: interpret a `len`-character alphameric string whose
 * last digit carries the sign (A-I/{ = positive 1-9/0, J-R/} = negative). Used
 * by alphameric->numeric MOVE (Section C, C10). Returns the signed value. */
long rpg_rt_overpunch_in(const char *s, int len);

/* Sign-overpunch encode: write `value` into `out` as `len` digits with the sign
 * overpunched on the last digit. Returns len. Section C (C10). */
int rpg_rt_overpunch_out(long value, char *out, int len);

/* Load a prerun-time array/table from a file at program start (Section B).
 * `fmt_a`/`fmt_b` (E-spec col 43/55, E7): 0 = zoned-decimal ASCII digits
 * (newline-delimited records, as before), 1 = packed-decimal, 2 = binary.
 * For fmt 0, records are newline-delimited and fields are parsed as ASCII
 * decimal digits, `len_a` digits each, storing up to `total` elements into
 * `out_a`. For fmt 1/2 the file is a flat sequence of fixed-width bytes with
 * no line structure (packed/binary data can legitimately contain byte values
 * that look like '\n'), decoded via the same packed/binary decoders used for
 * ordinary input fields. When `len_b > 0` and `out_b` is non-NULL, the
 * partner array/table is loaded in alternating format: A1 B1 A2 B2 ... on
 * each record/chunk, with `len_b`-byte partner fields decoded per `fmt_b`.
 * Returns the number of elements stored in out_a. The output arrays are
 * zero-initialised by the caller; a short file leaves the unused tail as
 * zero. */
int rpg_rt_load_arrays(const char *path, int len_a, int len_b,
                       int total, int *out_a, int *out_b,
                       int fmt_a, int fmt_b);

/* Alphameric counterpart of rpg_rt_load_arrays (A9): same file layout and
 * partner semantics, but copies raw `len_a`/`len_b`-byte fields verbatim
 * instead of parsing them as decimal digits, and blank-fills (not zero-fills)
 * a missing partner field. */
int rpg_rt_load_char_arrays(const char *path, int len_a, int len_b,
                            int total, char *out_a, char *out_b);

/* Close every opened file. Called on LR (last record). */
void rpg_rt_close_all(void);

/* ----- Program linkage (CALL/PARM/PLIST/RETRN/EXIT/RLABL/FREE) ------------- */
/*
 * Registry-dispatch model: every compiled program that could be a CALL
 * target is linked into the same executable and self-registers here via a
 * constructor emitted by the compiler (see codegen.cpp's create_entry_
 * function). CALL becomes a name lookup + indirect call instead of a direct
 * LLVM call, matching the manual's own two-tier lookup and its lazy
 * ("first CALL initializes; skip init on repeat CALLs unless FREE'd")
 * semantics.
 */

/* A registered program's entry point. `parm_ptrs` is an array of `parm_count`
 * raw field addresses (the caller's PLIST, in order; NULL/0 for a
 * parameterless CALL). `first_call` is 1 on the first invocation since
 * program start or the most recent rpg_rt_free(), else 0 -- the callee's
 * own generated prologue uses this to gate one-time initialization (heading
 * pass, prerun-time array loads). Returns a status: 0 = normal return
 * (RETRN without LR), 1 = normal return with LR on, 2 = abnormal end (a
 * halt indicator was on at RETRN/end of program). */
typedef int (*rpg_entry_fn)(void **parm_ptrs, int parm_count, int first_call);

/* Register a compiled program under `name` (its H-spec program-id,
 * upper-cased). Called from a constructor emitted by every compiled program
 * at process startup. A second registration under the same name replaces
 * the first (last one wins; this only happens if a name collides). */
void rpg_rt_register_program(const char *name, rpg_entry_fn fn);

/* Look up `name` and invoke it with `parm_ptrs`/`parm_count`. On return,
 * *out_error_ind is 1 if the call could not be made (no such program, or a
 * program tried to call itself or a program higher in the program stack --
 * both documented restrictions in the manual) or if the callee ended
 * abnormally; *out_lr_ind is 1 if the callee's LR indicator was on when it
 * returned. Neither out pointer may be NULL. Returns 0 on a completed call,
 * nonzero if the call could not be made at all (matches this project's
 * loud-error-over-silent-corruption precedent: the failure is reported to
 * stderr, but -- like every other runtime failure in this file -- does not
 * abort the process; it is up to the generated code's resulting indicators
 * to react). */
int rpg_rt_call(const char *name, void **parm_ptrs, int parm_count,
                int *out_error_ind, int *out_lr_ind);

/* Clear `name`'s "initialized" flag so its next CALL runs one-time init
 * again. Per the manual, this does NOT close any of that program's files.
 * Returns 0 if `name` was a registered, previously-initialized program
 * (cleared), 1 otherwise ("not successful", for the optional 56-57
 * resulting indicator on the FREE op). */
int rpg_rt_free(const char *name);

/* ----- WORKSTN (Chapter 7, "Using a WORKSTN File") ------------------------- */
/*
 * Two backends behind one interface, selected once at rpg_rt_ws_open() time
 * via the RPG_WORKSTN_MODE environment variable ("headless" or "terminal";
 * default "terminal"):
 *   - terminal: drives the real controlling tty with ANSI/VT100 cursor
 *     positioning and SGR attributes (color/reverse/blink); input fields are
 *     collected with ordinary line-buffered prompts rather than a full
 *     raw-mode single-keystroke editor (see WORKSTN support notes in
 *     docs/ARCHITECTURE.md for why -- this is the part expected to evolve
 *     most after first real use).
 *   - headless: reads a line-oriented script from RPG_WORKSTN_SCRIPT and
 *     dumps each written screen to RPG_WORKSTN_DUMP (default stdout); this
 *     is what the regression test suite drives, since it runs
 *     non-interactively.
 *
 * Only SRT (single requester terminal) programs are supported: no MRT
 * multi-terminal request sharing. A device is identified by a 2-character
 * id (3-byte buffers below hold it NUL-terminated).
 */

/* Open a WORKSTN file: parse its .dspf display-format file (`fmts_path`,
 * already resolved to an absolute/relative path by the compiler -- same
 * "literal path baked into the generated IR" convention as an ordinary DISK
 * filename) and initialize backend state. Returns a non-negative ws_id, or
 * -1 on failure (bad .dspf, or headless mode with no usable script). */
int rpg_rt_ws_open(const char *fmts_path, const char *program_id);

/* Acquire a device for `ws_id` (ACQ, or the cycle's own primary-file open).
 * `device_id` (2 chars, NULL/blank => let the backend assign one).
 * `out_device_id` (3-byte buffer) receives the attached device's id.
 * Returns 1 on success, 0 on failure (NUM limit reached, fspec.h). */
int rpg_rt_ws_acquire(int ws_id, const char *device_id, char *out_device_id);

/* Release `device_id` from `ws_id` (REL, or O-spec col 16 'R'). NULL/blank
 * `device_id` releases whichever device supplied the last input (O-spec 'R'
 * has no factor 1 to name one explicitly). Returns 1 on success, 0 if the
 * device was not attached. When this empties every attached device,
 * subsequent rpg_rt_ws_read calls return 0 (end of file). */
int rpg_rt_ws_release(int ws_id, const char *device_id);

/* Force the next rpg_rt_ws_read on `ws_id` to come from `device_id` (NEXT). */
void rpg_rt_ws_next(int ws_id, const char *device_id);

/* Read the next input record (implicit cycle input, or a WORKSTN READ).
 * `buf` (buflen bytes) is filled per the display format that was showing:
 * literal fields at their D-spec byte ranges, input fields at theirs (typed
 * value in the terminal backend; the current script block's FIELD values in
 * the headless backend). `out_device_id` (3 bytes) receives the responding
 * device. `out_funckey` is 0, or 1-25 for KA-KY (manual: "all function-key
 * indicators are turned off; then the appropriate one, if any, is turned
 * on"). `out_cmdkey` is 0, or 1-6 for Print/RollUp/RollDown/Clear/Help/Home
 * (a command-key "exception": no field data is returned). `out_status`
 * receives the *STATUS code (INFDS; see rpg_rt_ws_infds). Returns 1 on a
 * record or exception, 0 on end-of-file (every device released). */
int rpg_rt_ws_read(int ws_id, char *buf, int buflen, char *out_device_id,
                   int *out_funckey, int *out_cmdkey, int *out_status);

/* Write `buf` (buflen bytes, the format's record buffer, already field-
 * placed by ordinary O-spec byte-position output codegen) to `device_id`
 * (NULL/blank => the device that supplied the last input) using display
 * format `format_name`: renders it (terminal) or dumps it (headless). */
void rpg_rt_ws_write(int ws_id, const char *format_name, const char *device_id,
                     const char *buf, int buflen);

/* Flush the current line buffer (rpg_rt_line_begin/rpg_rt_line_put_*,
 * already populated by ordinary O-spec byte-position field placement -- the
 * manual confirms WORKSTN field placement is byte-offset based, not
 * row/column) to `ws_id` as a WORKSTN write, using display format
 * `format_name`. `device_id` may be NULL (use the device that supplied the
 * last input). Same "build in the shared line buffer, then hand off" shape
 * as rpg_rt_flush_rec for a disk record. */
void rpg_rt_ws_flush(int ws_id, const char *format_name, const char *device_id);

/* POST: retrieve status for `device_id` into the four INFDS status values
 * (manual: "*SIZE"/"*MODE"/"*INP"/"*OUT"; *STATUS/*OPCODE/*RECORD are left
 * untouched by POST). Returns 1 on success, 0 if `device_id` isn't attached
 * to `ws_id`. */
int rpg_rt_ws_post(int ws_id, const char *device_id, int *out_size,
                   int *out_mode, int *out_inp, int *out_out);

/* SHTDN: returns 1 if the system operator has requested shutdown (headless:
 * RPG_WORKSTN_SHTDN=1 in the environment; terminal: a SIGTERM/SIGHUP was
 * received since program start), else 0. */
int rpg_rt_ws_shtdn(void);

/* Write the up-to-seven INFDS subfields' raw bytes (manual "Coding the INFDS
 * Data Structure"). Each `*_ptr` may be NULL -- the program's WORKSTN file
 * has no INFDS, or (POST) that subfield isn't touched by this operation --
 * and is skipped. Fixed widths: *STATUS/*OPCODE 5, *RECORD 8, *SIZE 4,
 * *MODE/*INP/*OUT 2 (ispec.cpp assigns matching byte ranges when parsing an
 * INFDS DS's keyword subfield lines). `opcode` is "READ"/"ACQ"/"REL"/
 * "NEXT"/"POST"/"WRITE"/NULL; `record` is the format name (WRITE only, else
 * NULL); `size_val`/`mode_val`/`inp_val`/`out_val` are ignored (NULL
 * pointer) unless this call is filling those subfields (POST, or a READ
 * that also refreshes them). */
void rpg_rt_ws_infds(char *status_ptr, char *opcode_ptr, char *record_ptr,
                     char *size_ptr, char *mode_ptr, char *inp_ptr,
                     char *out_ptr, int status, const char *opcode,
                     const char *record, int size_val, int mode_val,
                     int inp_val, int out_val);

/* CALL/FREE's dynamic (field-valued) target-name form: `field` is a fixed-
 * width character field's raw bytes (`len` of them, blank-padded, not NUL-
 * terminated); this right-trims trailing blanks, upper-cases (name lookups
 * are case-insensitive, matching the literal form), and NUL-terminates the
 * result into `out` (capacity `out_cap`, truncated if the trimmed name
 * would not fit). Returns the trimmed length actually copied. */
int rpg_rt_field_to_cstr(const char *field, int len, char *out, int out_cap);

/* ----- KEYBORD/CRT (Chapter 10, "Using a CONSOLE, KEYBORD, or CRT File") --- */
/*
 * KEYBORD's interaction shape (one field prompted/entered at a time via KEY/
 * SET, never a laid-out screen format) is much smaller than WORKSTN's, so it
 * gets its own small backend rather than reusing ws_file_t -- but backend
 * selection still follows the exact same RPG_WORKSTN_MODE/RPG_WORKSTN_SCRIPT/
 * RPG_WORKSTN_DUMP environment-variable convention (a program can never have
 * both a WORKSTN and a KEYBORD file, see fspec.cpp's mutual exclusion, so
 * there's no ambiguity in sharing them). The headless script reuses the same
 * line-oriented file, with two of its own keywords: `RESP <text>` supplies a
 * KEY operation's typed response (or `RESP *DUP` for the Dup key), and `KEY
 * <name>` (the same KA-KY/PRINT/ROLLUP/... vocabulary WORKSTN's script uses)
 * supplies which function key SET's function-key form was answered with.
 */

/* Open the program's (at most one, fspec.cpp enforced) KEYBORD file.
 * `reclen` is the F-spec record length, used only by the terminal backend to
 * pick the manual's cosmetic six-line-of-40 vs twenty-four-line-of-79 layout
 * threshold. Returns a kb_id, or -1 on failure (headless mode with no usable
 * script). */
int rpg_rt_kb_open(int reclen);

/* KEY: prompt with `prompt` (prompt_len bytes) and read one response,
 * applying the manual's response rule directly against the caller-owned
 * `out` buffer (`width` bytes -- for a numeric result, the caller's own
 * current value pre-formatted as plain zero-padded ASCII digits via
 * rpg_rt_num_to_digits, decoded back out via rpg_rt_get_decimal afterward;
 * for alphameric, the field's own storage): typed text is right-justified/
 * zero-padded (`is_numeric` true) or left-justified/blank-padded (false); no
 * text (Enter alone) zero/blank-fills the same way; the Dup key leaves `out`
 * untouched. */
void rpg_rt_kb_key(int kb_id, const char *prompt, int prompt_len,
                   char *out, int width, int is_numeric);

/* SET: display `text` (text_len bytes; may be empty for no display change)
 * and, if `nallowed` > 0, pause for one of the function keys named in
 * `allowed` (1-based KA=1..KY=25 numbering, matching cspec.cpp's ind_token)
 * to be pressed. Returns the 1-based index into `allowed` of the key
 * pressed, or 0 if it matched none of them (the manual's "the program
 * stops" isn't implemented as a hard halt, matching WORKSTN's own
 * unsurfaced-INFSR-exception precedent) or if `nallowed` is 0 (pure
 * display, no pause). */
int rpg_rt_kb_set(int kb_id, const char *text, int text_len,
                  const int *allowed, int nallowed);

/* Format `value`'s digits, zero-padded/truncated to exactly `width` bytes
 * (no sign, no punctuation), into `out`. The inverse of rpg_rt_get_decimal
 * for the same plain-ASCII-digit convention. */
void rpg_rt_num_to_digits(long value, char *out, int width);

/* Open the program's CRT output file. CRT reuses the ordinary O-spec/
 * printer line-buffer machinery unchanged (rpg_rt_line_begin, the
 * rpg_rt_line_put_* functions, rpg_rt_emit_line -- all file-id generic
 * already) -- only the open differs, writing
 * to stdout (terminal mode) or the headless RPG_WORKSTN_DUMP convention
 * (headless mode) instead of a flat file. Returns a file id (registered in
 * the same g_files[] table ordinary output files use), or -1 on failure. */
int rpg_rt_open_crt(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RPG_RUNTIME_H */
