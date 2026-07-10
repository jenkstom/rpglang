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

/* Emit the current line buffer to `file_id`, followed by `space_after` newlines
 * (default 1). Then clears the buffer. */
void rpg_rt_emit_line(int file_id, int space_after);

/* Skip `file_id`'s output to absolute line `line_no` on the page (Section D,
 * D13). A skip to a line at or before the current position starts a new page
 * (form-feed, page counter incremented). */
void rpg_rt_skip(int file_id, int line_no);

/* Return the current page number. `which` 0 => `file_id`'s own page; 1..7 =>
 * the page of the nth-opened output file (PAGE1-PAGE7). Section D (D14). */
int rpg_rt_page(int file_id, int which);

/* Close every opened file. Called on LR (last record). */
void rpg_rt_close_all(void);

/* Compare two fixed-length character buffers (left-aligned, blank-padded to the
 * longer length). Returns -1/0/1 (a<b, a==b, a>b). Phase 10. */
int rpg_rt_cmp_str(const char *a, int alen, const char *b, int blen);

/* LOKUP a numeric `key` in `arr` (count elements). Starts at index *idx (1-based)
 * and scans. On an equal match, sets *idx to that element and returns 0. If no
 * equal but a higher element exists, returns +1 (and sets *idx to it). If a
 * lower element exists, returns -1. Else returns -2 (nothing). For an unsorted
 * array only equality (0) is meaningful; high/low require an ascending array.
 * Phase 10. */
int rpg_rt_lokup(long key, const int *arr, int count, int *idx);

/* Format a numeric value into `out` (capacity out_cap) using an RPG II edit
 * code (codes '1'-'4', 'A'-'D', 'J'-'M', 'N', 'O'). Returns the string length.
 * The result is right-justified into `width` characters if width > 0.
 * Phase 10. */
int rpg_rt_edit(long value, char edit_code, int width, char *out, int out_cap);

/* Like rpg_rt_edit, but the stored value is a scaled integer whose last
 * `decimals` digits are the fractional part (Section C). decimals<=0 behaves
 * like rpg_rt_edit. Returns the string length. */
int rpg_rt_edit_dec(long value, char edit_code, int width, int decimals,
                    char *out, int out_cap);

/* Format `value` (a scaled integer with `decimals` fractional digits) using an
 * RPG II edit word (Section D, D16). The word's blanks are replaceable (filled
 * by source digits), the first '0' stops zero-suppression, the first '*' does
 * check-protection, a trailing '-' or "CR" is a sign, '&' forces a blank.
 * Returns the string length. */
int rpg_rt_edit_word(long value, const char *word, int word_len,
                     int decimals, char *out, int out_cap);

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
 * Reads newline-delimited records from `path` and parses fixed-width ASCII
 * decimal fields of `len_a` digits each, storing up to `total` elements into
 * `out_a`. When `len_b > 0` and `out_b` is non-NULL, the partner array/table
 * is loaded in alternating format: A1 B1 A2 B2 ... on each record, with
 * `len_b`-digit partner fields. Returns the number of elements stored in
 * out_a. The output arrays are zero-initialised by the caller; a short file
 * leaves the unused tail as zero. */
int rpg_rt_load_arrays(const char *path, int len_a, int len_b,
                       int total, int *out_a, int *out_b);

/* Close every opened file. Called on LR (last record). */
void rpg_rt_close_all(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RPG_RUNTIME_H */
