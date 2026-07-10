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
#include <stdlib.h>
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
    int   line;          /* current line on the page (output) */
    int   page;          /* current page number (output)      */
    /* Look-ahead peek cache (E19): one buffered record per file. */
    char  peek[1024];    /* the peeked record (NUL-terminated) */
    int   peek_len;      /* bytes in peek (0 = no record peeked) */
    int   peek_valid;    /* 1 = peek holds the next record */
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

/* Read one physical record from `file_id`'s stream into `buf`. The core of
 * rpg_rt_read_next, shared with rpg_rt_peek_next (E19). Returns 1 on record,
 * 0 on EOF. */
static int read_one(int file_id, char *buf, size_t buflen) {
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
            if (got == 0) return 0;
            break;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[got++] = (char)c;
    }

    if (got == rl) {
        int peek = fgetc(fp);
        if (peek == '\r') {
            int peek2 = fgetc(fp);
            if (peek2 != '\n' && peek2 != EOF) ungetc(peek2, fp);
        } else if (peek != '\n' && peek != EOF) {
            ungetc(peek, fp);
        }
    }

    while (got < rl) buf[got++] = ' ';
    buf[rl] = '\0';
    return 1;
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
    /* If a look-ahead peek is pending (E19), consume it first. */
    if (g_files[file_id].peek_valid) {
        size_t n = (size_t)g_files[file_id].peek_len;
        if (n + 1 > buflen) n = buflen - 1;
        memcpy(buf, g_files[file_id].peek, n);
        buf[n] = '\0';
        g_files[file_id].peek_valid = 0;
        return 1;
    }
    return read_one(file_id, buf, buflen);
}

/* Peek at the next record without consuming it (E19 look-ahead). The record is
 * read into a per-file cache and copied to `buf`; a subsequent read_next
 * returns the cached record. Returns 1 on record, 0 on EOF (look-ahead fields
 * are then filled with 9s by the caller). */
int rpg_rt_peek_next(int file_id, char *buf, size_t buflen) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) {
        return 0;
    }
    if (!g_files[file_id].peek_valid) {
        char tmp[sizeof g_files[file_id].peek];
        if (!read_one(file_id, tmp, sizeof tmp)) return 0;
        size_t n = strlen(tmp);
        memcpy(g_files[file_id].peek, tmp, n);
        g_files[file_id].peek[n] = '\0';
        g_files[file_id].peek_len = (int)n;
        g_files[file_id].peek_valid = 1;
    }
    size_t n = (size_t)g_files[file_id].peek_len;
    if (n + 1 > buflen) n = buflen - 1;
    memcpy(buf, g_files[file_id].peek, n);
    buf[n] = '\0';
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
/* Packed-decimal and binary input decoders (Section C, C9).                   */
/*                                                                            */
/* The field spans record bytes [from..to] (1-based). Packed format packs two  */
/* BCD digits per byte with a sign nibble in the low-order byte's low nibble   */
/* (F=+, D=-). Binary is big-endian (S/36 convention): 2-byte = int16,        */
/* 4-byte = int32. The decoded value is the same scaled integer a zoned field  */
/* of equal digits would produce, so it flows into the i32 model unchanged.    */
/* -------------------------------------------------------------------------- */
long rpg_rt_get_packed(const char *rec, int reclen, int from, int to) {
    if (from < 1) from = 1;
    if (to > reclen) to = reclen;
    if (to < from) return 0;
    long v = 0;
    int n = to - from + 1;            /* number of packed bytes */
    for (int k = 0; k < n; ++k) {
        unsigned char b = (unsigned char)rec[from - 1 + k];
        int hi = (b >> 4) & 0x0F;     /* high nibble = a digit (all bytes) */
        int lo = b & 0x0F;            /* low nibble = digit, or sign on last */
        if (k < n - 1) {
            v = v * 10 + hi;
            v = v * 10 + lo;
        } else {
            v = v * 10 + hi;          /* last byte: high nibble is a digit */
            /* low nibble is the sign: D = negative, anything else = positive */
        }
    }
    /* Sign from the low nibble of the final byte. */
    unsigned char last = (unsigned char)rec[to - 1];
    int sign = last & 0x0F;
    if (sign == 0x0D) return -v;
    return v;
}

long rpg_rt_get_binary(const char *rec, int reclen, int from, int to) {
    if (from < 1) from = 1;
    if (to > reclen) to = reclen;
    int n = to - from + 1;
    if (n <= 0) return 0;
    if (n >= 4) {
        /* 4-byte (or more) big-endian signed int32. */
        long v = 0;
        for (int k = 0; k < 4; ++k)
            v = (v << 8) | (unsigned char)rec[from - 1 + k];
        return v;
    }
    /* 2-byte big-endian signed int16, sign-extended. */
    int v = ((unsigned char)rec[from - 1] << 8) | (unsigned char)rec[from];
    if (v & 0x8000) v -= 0x10000;     /* sign-extend */
    return v;
}

/* -------------------------------------------------------------------------- */
/* Sign-overpunch encode/decode (Section C, C10).                             */
/*                                                                            */
/* In a zoned-decimal / alphameric string the last character's "zone" carries  */
/* the sign. ASCII sign-overpunch (matching the manual's letter sets):         */
/*   positive: A-I encode digits 1-9, { encodes 0, plain 0-9 stay as-is.      */
/*   negative: J-R encode digits 1-9, } encodes 0.                             */
/* rpg_rt_overpunch_in reads the leading digits and applies the trailing sign; */
/* rpg_rt_overpunch_out writes `len` digits and overpunches the last.          */
/* -------------------------------------------------------------------------- */
long rpg_rt_overpunch_in(const char *s, int len) {
    if (!s || len <= 0) return 0;
    int neg = 0;
    long v = 0;
    for (int i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)s[i];
        int digit;
        if (ch >= '0' && ch <= '9')      digit = ch - '0';          /* plain */
        else if (ch >= 'A' && ch <= 'I') digit = ch - 'A' + 1;      /* +1..+9 */
        else if (ch >= 'a' && ch <= 'i') digit = ch - 'a' + 1;
        else if (ch >= 'J' && ch <= 'R') { digit = ch - 'J' + 1; neg = 1; } /* -1..-9 */
        else if (ch >= 'j' && ch <= 'r') { digit = ch - 'j' + 1; neg = 1; }
        else if (ch == '{' || ch == '}') { digit = 0; if (ch == '}') neg = 1; }
        else                              digit = 0;                 /* blank etc. */
        v = v * 10 + digit;
    }
    return neg ? -v : v;
}

int rpg_rt_overpunch_out(long value, char *out, int len) {
    if (!out || len <= 0) return 0;
    int neg = value < 0;
    unsigned long uv = neg ? (unsigned long)(-value) : (unsigned long)value;
    /* Write digits right-to-left, then overpunch the last (rightmost) one. */
    for (int i = len - 1; i >= 0; --i) {
        int d = (int)(uv % 10);
        uv /= 10;
        char ch;
        if (i == len - 1) {
            /* the units digit carries the sign via overpunch */
            if (neg) ch = (d == 0) ? '}' : (char)('J' + d - 1);
            else     ch = (d == 0) ? '0' : (char)('A' + d - 1);
        } else {
            ch = (char)('0' + d);
        }
        out[i] = ch;
    }
    return len;
}
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
/* Prerun-time array/table loader (Section B).                                 */
/*                                                                            */
/* Reads the whole file into memory, then walks it once extracting fixed-      */
/* width ASCII-decimal fields. For alternating arrays/tables the primary and   */
/* partner elements interleave on each record (A1 B1 A2 B2 ...).              */
/* -------------------------------------------------------------------------- */

/* Parse `len` chars at `p` as a non-negative decimal integer (blanks and      */
/* leading zeros ignored; non-digit stops the parse => 0).                     */
static long parse_dec(const char *p, int len) {
    long v = 0;
    for (int i = 0; i < len; ++i) {
        char c = p[i];
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
        else if (c == ' ')        continue;   /* embedded blanks skipped */
        else                      break;
    }
    return v;
}

int rpg_rt_load_arrays(const char *path, int len_a, int len_b,
                       int total, int *out_a, int *out_b) {
    if (!path || !out_a || total <= 0 || len_a <= 0) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "rpg_rt: cannot open prerun-time array file '%s'\n", path);
        return 0;
    }
    if (len_b < 0) len_b = 0;

    /* Slurp the file into a memory buffer (prerun-time arrays are small). */
    char *buf = NULL;
    size_t cap = 0, n = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); fclose(fp); return 0; }
            buf = nb;
        }
        buf[n++] = (char)ch;
    }
    fclose(fp);

    int stored = 0;            /* elements written to out_a */
    int i = 0;                 /* cursor into buf */
    int pair = 0;              /* partner elements written (out_b) */
    while (stored < total && i + len_a <= (int)n) {
        /* Skip a newline before the next primary element (records are
         * newline-delimited; element fields are fixed width). */
        while (i < (int)n && (buf[i] == '\n' || buf[i] == '\r')) ++i;
        if (i + len_a > (int)n) break;
        out_a[stored++] = (int)parse_dec(buf + i, len_a);
        i += len_a;
        if (len_b > 0 && out_b && pair < total) {
            if (i + len_b <= (int)n) {
                out_b[pair++] = (int)parse_dec(buf + i, len_b);
                i += len_b;
            } else {
                out_b[pair++] = 0;
            }
        }
    }
    free(buf);
    return stored;
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
/* Edit codes. rpg_rt_edit_dec honours a decimal-position count (Section C):   */
/* the stored value is a scaled integer, so the last `decimals` digits are the */
/* fractional part. rpg_rt_edit is the decimals=0 legacy entry point.         */
/* -------------------------------------------------------------------------- */
int rpg_rt_edit_dec(long value, char code, int width, int decimals,
                    char *out, int out_cap) {
    int neg = value < 0;
    unsigned long v = neg ? (unsigned long)(-value) : (unsigned long)value;
    if (decimals < 0) decimals = 0;

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
    /* When the field carries decimal positions, force a decimal point even if
     * the edit code (3/4) would normally omit it. */
    if (decimals > 0) use_decimal = 1;

    /* Build digits into a temp buffer (reversed), padded so the fractional
     * part always has `decimals` digits. */
    char digits[40];
    int nd = 0;
    int need = decimals + 1;            /* at least one integer digit */
    if (v == 0) { digits[nd++] = '0'; }
    while (v > 0) { digits[nd++] = (char)('0' + (v % 10)); v /= 10; }
    while (nd < need) digits[nd++] = '0';

    /* Assemble the formatted string left-to-right. */
    char buf[64];
    int len = 0;
    if (sign_style == 2) { buf[len++] = neg ? '-' : '+'; }
    /* integer digits = everything above the decimal positions */
    int nint = nd - decimals;
    for (int i = nd - 1; i >= decimals; --i) {
        buf[len++] = digits[i];
        /* commas are placed every three integer digits counting from the
         * units place (i == decimals is the units digit). */
        if (use_comma && i > decimals) {
            int from_units = i - decimals;     /* 1.. for digits above units */
            if (from_units % 3 == 0) buf[len++] = ',';
        }
    }
    if (use_decimal) {
        buf[len++] = '.';
        if (decimals == 0) {
            buf[len++] = '0';   /* integer-model default fractional digit */
        } else {
            for (int i = decimals - 1; i >= 0; --i) buf[len++] = digits[i];
        }
    }
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

int rpg_rt_edit(long value, char code, int width, char *out, int out_cap) {
    return rpg_rt_edit_dec(value, code, width, 0, out, out_cap);
}

/* -------------------------------------------------------------------------- */
/* Edit words (Section D, D16).                                               */
/*                                                                            */
/* The edit word is a pattern in which blanks are "replaceable" (filled by    */
/* source digits), the first '0' stops zero-suppression, the first '*' fills   */
/* suppressed zeros with asterisks, a trailing '-' or "CR" is a negative sign  */
/* (printed only if negative), '&' forces a literal blank, and any other char  */
/* (',', '.', '/', etc.) is a constant printed in place. Source digits are     */
/* consumed right-to-left from the value's decimal-adjusted digit string.      */
/* -------------------------------------------------------------------------- */
int rpg_rt_edit_word(long value, const char *word, int word_len,
                     int decimals, char *out, int out_cap) {
    if (!out || out_cap <= 0) return 0;
    int neg = value < 0;
    unsigned long uv = neg ? (unsigned long)(-value) : (unsigned long)value;
    if (decimals < 0) decimals = 0;

    /* Build the magnitude's digit string in normal (MSD-first) order. */
    char mag[40];
    int mnd = 0;
    if (uv == 0) mag[mnd++] = '0';
    {
        char rev[40];
        int rnd = 0;
        unsigned long t = uv;
        while (t > 0) { rev[rnd++] = (char)('0' + t % 10); t /= 10; }
        while (rnd > 0) mag[mnd++] = rev[--rnd];
    }

    /* Count the replaceable positions in the word (blanks, the first 0, the
     * first *). The source digits are right-aligned over these positions. */
    int stop = -1;          /* index of first '0' or '*' (suppression stop) */
    char stopch = 0;
    int nrep = 0;
    int negat = -1;         /* index of a trailing '-' sign indicator */
    for (int i = 0; i < word_len; ++i) {
        char w = word[i];
        int rep = (w == ' ' || w == '0' || w == '*');
        if (rep) {
            nrep++;
            if (stop < 0 && (w == '0' || w == '*')) { stop = i; stopch = w; }
        }
        if (w == '-' && i > 0 && word[i-1] == ' ') negat = i;
    }
    int negCR = -1;
    if (word_len >= 2 && word[word_len-2] == 'C' && word[word_len-1] == 'R')
        negCR = word_len - 2;

    /* Right-align magnitude digits over the nrep positions (pad leading 0). */
    char src[64];
    int sp = 0;             /* cursor into src as we consume left-to-right */
    if (nrep > 0) {
        int pad = nrep - mnd;
        for (int i = 0; i < pad; ++i) src[sp++] = '0';
        for (int i = 0; i < mnd && sp < nrep; ++i) src[sp++] = mag[i];
        sp = 0;             /* consume from the start */
    }

    /* Walk the word left-to-right, emitting output. */
    char tmp[96];
    int oi = 0;
    int significant = 0;    /* a non-zero digit has been placed */
    for (int i = 0; i < word_len && oi < (int)sizeof(tmp) - 2; ++i) {
        char w = word[i];
        if (w == '&') { tmp[oi++] = ' '; continue; }       /* forced blank */
        if (i == negat) { if (neg) tmp[oi++] = '-'; continue; }
        if (i == negCR) { if (neg) { tmp[oi++] = 'C'; tmp[oi++] = 'R'; }
                          ++i; continue; }

        int rep = (w == ' ' || w == '0' || w == '*');
        if (rep) {
            char d = (sp < nrep) ? src[sp++] : '0';
            if (d != '0') significant = 1;
            int at_stop = (i == stop);
            if (!significant && !at_stop) {
                /* suppression zone */
                tmp[oi++] = (stopch == '*') ? '*' : ' ';
            } else {
                if (at_stop) significant = 1;   /* '0'/'*' anchor prints its digit */
                tmp[oi++] = d;
            }
        } else {
            /* Constant char (',', '.', '/', etc.). */
            if (!significant && stop >= 0 && i > stop) tmp[oi++] = ' ';
            else                                       tmp[oi++] = w;
        }
    }
    int n = oi;
    if (n > out_cap - 1) n = out_cap - 1;
    for (int i = 0; i < n; ++i) out[i] = tmp[i];
    out[n] = '\0';
    return n;
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
            g_files[id].line     = 0;
            g_files[id].page     = 1;   /* the first page is page 1 (D14) */
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

void rpg_rt_line_put_num_dec(long value, int end_pos, int decimals) {
    /* Section C: value is a scaled integer; emit it with `decimals` fractional
     * digits, right-justified to end_pos. decimals<=0 falls back to put_num. */
    if (decimals <= 0) { rpg_rt_line_put_num(value, end_pos); return; }
    int neg = value < 0;
    unsigned long uv = neg ? (unsigned long)(-value) : (unsigned long)value;
    char digits[24];
    int nd = 0;
    int need = decimals + 1;
    if (uv == 0) digits[nd++] = '0';
    while (uv > 0) { digits[nd++] = (char)('0' + uv % 10); uv /= 10; }
    while (nd < need) digits[nd++] = '0';
    char tmp[32];
    int oi = 0;
    if (neg) tmp[oi++] = '-';
    for (int i = nd - 1; i >= decimals; --i) tmp[oi++] = digits[i];
    tmp[oi++] = '.';
    for (int i = decimals - 1; i >= 0; --i) tmp[oi++] = digits[i];
    place_right(tmp, oi, end_pos);
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
    /* Track the line position on the current page (D13). */
    g_files[file_id].line += 1 + (space_after > 0 ? space_after : 0);
}

/* -------------------------------------------------------------------------- */
/* Skip and page support (Section D, D13/D14).                                 */
/*                                                                            */
/* rpg_rt_skip advances `file_id`'s output to absolute line `line_no` on the  */
/* current page. If `line_no` is at or before the current line, that means a  */
/* new page: emit a form-feed, reset the line counter, and bump the page      */
/* counter; then advance (with blank lines) to `line_no`.                     */
/* -------------------------------------------------------------------------- */
void rpg_rt_skip(int file_id, int line_no) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return;
    if (line_no < 1) line_no = 1;
    FILE *fp = g_files[file_id].fp;
    int cur = g_files[file_id].line;
    if (line_no <= cur || cur == 0) {
        /* New page: form-feed, reset line, advance page counter. */
        fputc('\f', fp);
        g_files[file_id].line = 0;
        g_files[file_id].page += 1;
        cur = 0;
    }
    /* Advance to the requested line with blank lines. */
    while (g_files[file_id].line < line_no - 1) {
        fputc('\n', fp);
        g_files[file_id].line++;
    }
}

/* Return the current page number for a file. `which`: 0 => this file's own
 * page counter; 1..7 => the page counter of the nth opened output file. */
int rpg_rt_page(int file_id, int which) {
    if (which >= 1 && which <= 7) {
        /* Find the nth output file in slot order. */
        int seen = 0;
        for (int id = 0; id < RPG_RT_MAX_FILES; ++id) {
            if (g_files[id].fp && !g_files[id].is_input) {
                ++seen;
                if (seen == which) return g_files[id].page;
            }
        }
        return 0;
    }
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES) return 0;
    return g_files[file_id].page;
}

void rpg_rt_close_all(void) {
    for (int id = 0; id < RPG_RT_MAX_FILES; ++id) {
        if (g_files[id].fp) {
            fclose(g_files[id].fp);
            g_files[id].fp = NULL;
        }
    }
}
