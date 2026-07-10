/* ========================================================================== *
 * rpg_runtime.c -- Implementation of the C runtime linked into every program
 *                 produced by the RPG II compiler.
 *
 * Phase 1 deliberately keeps this minimal: only `rpg_rt_version()` is
 * meaningfully implemented. The file-I/O functions are stubs that will be
 * fleshed out in Phase 3 (RPG cycle + sequential file reading) and Phase 4
 * (indexed/character processing).
 *
 * Compile with:  gcc -c rpg_runtime.c -o rpg_runtime.o
 * Archive with:  ar rcs librpgruntime.a rpg_runtime.o
 * ========================================================================== */

#include "rpg_runtime.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Phase 1 smoke-test.                                                        */
/* -------------------------------------------------------------------------- */
const char *rpg_rt_version(void) {
    /* Bump this whenever the runtime ABI/behaviour changes. The compiler
     * also embeds this expectation so we can catch ABI drift early. */
    return "rpgruntime 0.1 (phase 1)";
}

/* -------------------------------------------------------------------------- */
/* File I/O -- Phase 3 implementation.                                        */
/*                                                                            */
/* A small fixed table of open file handles; the file id is the table index.  */
/* Each slot tracks mode and the declared record length so that read_next can */
/* right-pad short physical lines up to the fixed record length.              */
/*                                                                            */
/* Record format: newline-delimited text lines (card-image style). See        */
/* docs/ARCHITECTURE.md for the porting rationale.                            */
/* -------------------------------------------------------------------------- */
#define RPG_RT_MAX_FILES 16

static struct {
    FILE *fp;            /* stdio stream, NULL if slot free */
    int   is_input;      /* 1 = read, 0 = write              */
    int   reclen;        /* declared fixed record length     */
} g_files[RPG_RT_MAX_FILES];

int rpg_rt_open_input(const char *path) {
    for (int id = 0; id < RPG_RT_MAX_FILES; ++id) {
        if (g_files[id].fp == NULL) {
            FILE *fp = fopen(path, "r");
            if (!fp) {
                fprintf(stderr, "rpg_rt: cannot open input file '%s'\n", path);
                return -1;
            }
            g_files[id].fp       = fp;
            g_files[id].is_input = 1;
            g_files[id].reclen   = 0;   /* set per-program; 0 => unbounded */
            return id;
        }
    }
    fprintf(stderr, "rpg_rt: too many open files\n");
    return -1;
}

int rpg_rt_read_next(int file_id, char *buf, size_t buflen) {
    /* Fixed-length record semantics.
     *
     * Each logical record is presented as exactly `reclen` characters. Bytes
     * are read one at a time so we can stop on a newline: if a CR/LF is seen
     * before `reclen` bytes have been gathered, the newline is consumed and
     * the rest of the window is padded with spaces. Lines longer than reclen
     * are split across successive records (pure fixed-length behaviour).
     *
     * `buflen` must be >= reclen+1; we NUL-terminate for convenience. If the
     * caller never set a reclen, fall back to a line-based read so the stub
     * still works.
     */
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) {
        return 0; /* treated as EOF */
    }
    FILE *fp = g_files[file_id].fp;
    int   rl = g_files[file_id].reclen;

    if (rl <= 0) {
        /* Fallback: line-based read (no fixed width declared). */
        if (!fgets(buf, (int)buflen, fp)) return 0;
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        return 1;
    }

    if ((size_t)rl + 1 > buflen) {
        /* Buffer too small; clamp to what we can safely write. */
        rl = (int)buflen - 1;
    }

    int got = 0;
    int c;
    while (got < rl) {
        c = fgetc(fp);
        if (c == EOF) {
            /* EOF before any byte => real end of file. Mid-record EOF pads the
             * partial record with spaces (a short last line is still a record). */
            if (got == 0) return 0;
            break;
        }
        if (c == '\n') {
            /* Newline fills out the rest of the record with spaces. */
            break;
        }
        if (c == '\r') {
            /* Swallow a lone CR; a following LF (CRLF) is handled next loop. */
            continue;
        }
        buf[got++] = (char)c;
    }

    /* If the record filled exactly to reclen (a full record, not newline-ended),
     * and the very next byte is a line terminator, consume that one terminator.
     * This aligns record boundaries with line boundaries for exactly-full lines
     * (e.g. an 80-col card image terminated by \n) and avoids a phantom empty
     * record. Long lines keep splitting: the byte after a full record is a data
     * byte, not a newline, so nothing is consumed. */
    if (got == rl) {
        int peek = fgetc(fp);
        if (peek == '\r') {
            int peek2 = fgetc(fp);
            if (peek2 != '\n' && peek2 != EOF) ungetc(peek2, fp);
        } else if (peek != '\n' && peek != EOF) {
            ungetc(peek, fp);
        }
    }

    /* Pad the remainder of the fixed-width window with spaces. */
    while (got < rl) buf[got++] = ' ';
    buf[rl] = '\0';
    return 1;
}

void rpg_rt_set_reclen(int file_id, int reclen) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES) return;
    g_files[file_id].reclen = reclen;
}

long rpg_rt_get_decimal(const char *rec, int reclen, int from, int to) {
    /* Clamp to the available record text. */
    if (from < 1) from = 1;
    if (to > reclen) to = reclen;
    if (to < from) return 0;

    /* Parse the [from..to] (1-based) slice as plain ASCII digits. Non-digit
     * characters stop the parse (strtol behaviour); blank => 0. */
    long v = 0;
    for (int i = from - 1; i < to; ++i) {
        char c = rec[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
        } else if (c == ' ') {
            /* leading/embedded blanks are skipped (treated as nothing) */
        } else {
            break;
        }
    }
    return v;
}

/* -------------------------------------------------------------------------- */
/* Character comparison (Phase 10). Left-aligned, blank-padded to the longer   */
/* length, byte-wise (ASCII collating order).                                 */
/* -------------------------------------------------------------------------- */
int rpg_rt_cmp_str(const char *a, int alen, const char *b, int blen) {
    int n = alen > blen ? alen : blen;
    for (int i = 0; i < n; ++i) {
        unsigned char ca = i < alen ? (unsigned char)a[i] : ' ';
        unsigned char cb = i < blen ? (unsigned char)b[i] : ' ';
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return 0;
}

/* LOKUP (Phase 10). Scans arr[0..count-1] for `key`. Updates *idx (1-based) on
 * a match. Returns 0=equal found, +1=higher found (ascending), -1=lower found,
 * -2=nothing. */
int rpg_rt_lokup(long key, const int *arr, int count, int *idx) {
    int start = (idx && *idx > 0) ? *idx - 1 : 0;
    int found_hi = 0, found_lo = 0;
    for (int i = start; i < count; ++i) {
        if (arr[i] == key) {
            if (idx) *idx = i + 1;
            return 0;
        }
        if (arr[i] > key) found_hi = 1;
        else              found_lo = 1;
    }
    return found_hi ? +1 : (found_lo ? -1 : -2);
}

/* -------------------------------------------------------------------------- */
/* Edit codes (Phase 10). Format a number with zero-suppression, commas, a     */
/* decimal point, and a sign. We implement the common subset:                  */
/*   '1' : zero-suppress, comma, decimal point, no sign (CR/blank for neg)    */
/*   '2' : zero-suppress, no comma, decimal point                              */
/*   '3' : zero-suppress, comma, no decimal point                              */
/*   '4' : zero-suppress, no comma, no decimal point                           */
/*   'J'-'M': like '1'-'4' but print a trailing minus for negatives           */
/*   'N'-'Q': like '1'-'4' but leading sign                                    */
/* The output is right-justified to `width` (spaces) when width > 0.          */
/* -------------------------------------------------------------------------- */
int rpg_rt_edit(long value, char code, int width, char *out, int out_cap) {
    int neg = value < 0;
    unsigned long v = neg ? (unsigned long)(-value) : (unsigned long)value;

    /* Determine formatting flags from the edit code. */
    int use_comma = 0, use_decimal = 0, sign_style = 0; /* 0=CR, 1=trailing-, 2=leading */
    char c = code;
    /* J-M map to 1-4 with trailing minus; N-Q map to 1-4 with leading sign. */
    if (c >= 'J' && c <= 'M') { sign_style = 1; c = (char)('1' + (c - 'J')); }
    else if (c >= 'N' && c <= 'Q') { sign_style = 2; c = (char)('1' + (c - 'N')); }
    switch (c) {
        case '1': use_comma = 1; use_decimal = 1; break;
        case '2': use_comma = 0; use_decimal = 1; break;
        case '3': use_comma = 1; use_decimal = 0; break;
        case '4': use_comma = 0; use_decimal = 0; break;
        default:
            /* Unknown code: plain decimal. */
            break;
    }

    /* Build digits into a temp buffer (reversed). */
    char digits[24];
    int nd = 0;
    if (v == 0) { digits[nd++] = '0'; }
    while (v > 0) { digits[nd++] = (char)('0' + (v % 10)); v /= 10; }

    /* Assemble the formatted string left-to-right. */
    char buf[48];
    int len = 0;
    /* leading sign */
    if (sign_style == 2) { buf[len++] = neg ? '-' : '+'; }
    /* insert commas every 3 digits (no decimals in this integer model) */
    for (int i = nd - 1; i >= 0; --i) {
        buf[len++] = digits[i];
        if (use_comma && i > 0 && i % 3 == 0) buf[len++] = ',';
    }
    if (use_decimal) { buf[len++] = '.'; buf[len++] = '0'; }
    /* trailing sign / CR */
    if (sign_style == 1 && neg) buf[len++] = '-';
    else if (sign_style == 0 && neg) { buf[len++] = 'C'; buf[len++] = 'R'; }

    /* Right-justify into width. */
    int total = (width > len) ? width : len;
    if (total + 1 > out_cap) total = out_cap - 1;
    int pad = width - len;
    if (pad < 0) pad = 0;
    int oi = 0;
    for (int i = 0; i < pad && oi < total; ++i) out[oi++] = ' ';
    for (int i = 0; i < len && oi < total; ++i) out[oi++] = buf[i];
    out[oi] = '\0';
    return oi;
}

/* -------------------------------------------------------------------------- */
/* Output line buffer (Phase 7).                                              */
/*                                                                            */
/* The generated code builds one output line at a time: line_begin(width)     */
/* fills the buffer with spaces; line_put_* places a value right-justified to */
/* an end position; emit_line() writes it out with N trailing newlines.       */
/* -------------------------------------------------------------------------- */
#define RPG_RT_LINE_MAX 1024
static char g_line[RPG_RT_LINE_MAX];
static int  g_line_len = 0;

int rpg_rt_open_output(const char *path) {
    for (int id = 0; id < RPG_RT_MAX_FILES; ++id) {
        if (g_files[id].fp == NULL) {
            FILE *fp = fopen(path, "w");
            if (!fp) {
                fprintf(stderr, "rpg_rt: cannot open output file '%s'\n", path);
                return -1;
            }
            g_files[id].fp       = fp;
            g_files[id].is_input = 0;
            g_files[id].reclen   = 0;
            return id;
        }
    }
    fprintf(stderr, "rpg_rt: too many open files\n");
    return -1;
}

void rpg_rt_line_begin(int width) {
    if (width < 0) width = 0;
    if (width > RPG_RT_LINE_MAX) width = RPG_RT_LINE_MAX;
    memset(g_line, ' ', (size_t)width);
    g_line_len = width;
}

/* Place the rightmost `len` chars of `s` so the field ends at `end_pos`. The
 * field starts at end_pos - field_len. If the field is longer than the gap to
 * the previous content it overwrites leftward (last field wins, per manual). */
static void place_right(const char *s, int len, int end_pos) {
    if (end_pos < 1) end_pos = len;
    if (end_pos > g_line_len) {
        /* extend the buffer with spaces up to end_pos */
        for (int i = g_line_len; i < end_pos && i < RPG_RT_LINE_MAX; ++i)
            g_line[i] = ' ';
        if (end_pos > RPG_RT_LINE_MAX) end_pos = RPG_RT_LINE_MAX;
        g_line_len = end_pos;
    }
    int start = end_pos - len;     /* 0-based start index */
    if (start < 0) {
        /* field is wider than the room: drop the leftmost excess */
        s += -start;
        len += start;
        start = 0;
    }
    for (int i = 0; i < len; ++i) {
        if (start + i < g_line_len) g_line[start + i] = s[i];
    }
}

void rpg_rt_line_put_str(const char *s, int len, int end_pos) {
    if (!s || len <= 0) return;
    place_right(s, len, end_pos);
}

void rpg_rt_line_put_num(long value, int end_pos) {
    /* Format as ASCII decimal into a temp, right-justified to end_pos. */
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%ld", value);
    place_right(tmp, n, end_pos);
}

void rpg_rt_emit_line(int file_id, int space_after) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return;
    if (g_line_len < 0) g_line_len = 0;
    /* Rtrim: RPG lines are space-padded; trim trailing spaces for readability
     * of the produced file, but keep at least one char. */
    int len = g_line_len;
    while (len > 1 && g_line[len - 1] == ' ') --len;
    fwrite(g_line, 1, (size_t)len, g_files[file_id].fp);
    if (space_after < 0) space_after = 0;
    for (int i = 0; i < (space_after ? space_after : 1); ++i)
        fputc('\n', g_files[file_id].fp);
    g_line_len = 0;
}

void rpg_rt_close_all(void) {
    for (int id = 0; id < RPG_RT_MAX_FILES; ++id) {
        if (g_files[id].fp) {
            fclose(g_files[id].fp);
            g_files[id].fp = NULL;
        }
    }
}
