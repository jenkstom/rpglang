// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

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

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
    int   is_update;     /* 1 = update file (r+, in-place rewrite) */
    int   reclen;        /* declared fixed record length     */
    int   line;          /* current line on the page (output) */
    int   page;          /* current page number (output)      */
    /* Printer overflow (Section F, F22). */
    int   lines_per_page;/* page depth (default 66)           */
    int   overflow_line; /* line at which overflow occurs (default 60) */
    int   overflow_latched; /* 1 if overflow line was reached */
    /* Look-ahead peek cache (E19): one buffered record per file. */
    char  peek[1024];    /* the peeked record (NUL-terminated) */
    int   peek_len;      /* bytes in peek (0 = no record peeked) */
    int   peek_valid;    /* 1 = peek holds the next record */
    /* Keyed/random access (Section G, G24). */
    int   key_start;     /* 1-based record column where the key begins */
    int   key_len;       /* key field length (0 = no key) */
    long  last_rec_off;  /* byte offset of the most recently read record
                            (for in-place UPDATE/DELETE, G25) */
    long  cur_off;       /* logical position cursor (SETLL/READ) */
    /* In-memory key index, built lazily on first keyed op. */
    char **idx_keys;     /* sorted key strings (reclen-wide, space-padded) */
    long  *idx_offs;     /* matching byte offsets */
    int    idx_count;
    int    idx_built;    /* 1 once the index has been built */
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
            g_files[id].is_update = 0;
            g_files[id].reclen   = 0;   /* set per-program; 0 => unbounded */
            g_files[id].key_start = 0;
            g_files[id].key_len   = 0;
            g_files[id].last_rec_off = 0;
            g_files[id].cur_off   = 0;
            g_files[id].idx_keys  = NULL;
            g_files[id].idx_offs  = NULL;
            g_files[id].idx_count = 0;
            g_files[id].idx_built = 0;
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

    /* Remember where this record begins, for in-place UPDATE/DELETE (G25). */
    g_files[file_id].last_rec_off = ftell(fp);

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

/* -------------------------------------------------------------------------- */
/* Keyed / random access (Section G, G24).                                      */
/*                                                                              */
/* On an indexed DISK file the compiler calls rpg_rt_set_key once after open    */
/* to declare the key's record position and length. The first keyed op then     */
/* builds an in-memory index (sorted key -> byte offset) by scanning the file   */
/* once. CHAIN does a binary search for a key; SETLL positions at the lower     */
/* bound for sequential READ/READE. RRN access (no key set) uses the index's    */
/* offset array directly: record N is the Nth stored offset (1-based).          */
/* -------------------------------------------------------------------------- */

/* Comparison for sorting/searching the key index: space-padded, memcmp-style. */
static int key_cmp(const char *a, const char *b, int len) {
    return memcmp(a, b, (size_t)len);
}

/* qsort helper: compare idx_keys entries (each key_len wide). The key length
 * for the sort in progress is carried in s_sort_key_len. */
struct IdxEntry { char *key; long off; };
static int s_sort_key_len = 0;
static int idx_qsort_cmp(const void *pa, const void *pb) {
    const struct IdxEntry *a = (const struct IdxEntry *)pa;
    const struct IdxEntry *b = (const struct IdxEntry *)pb;
    int kl = s_sort_key_len > 0 ? s_sort_key_len : 1;
    int c = memcmp(a->key, b->key, (size_t)kl);
    /* Ties broken by file order (stable-ish): lower offset first. */
    if (c == 0) return (a->off < b->off) ? -1 : (a->off > b->off ? 1 : 0);
    return c;
}

void rpg_rt_set_key(int file_id, int key_start, int key_len) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES) return;
    g_files[file_id].key_start = key_start;
    g_files[file_id].key_len   = key_len;
    g_files[file_id].idx_built = 0;   /* rebuild if key changed */
}

/* Scan the whole file once, recording each record's key bytes and byte offset.
 * Leaves the stream positioned back at the start. */
static void build_index(int file_id) {
    if (g_files[file_id].idx_built) return;
    int kl = g_files[file_id].key_len;
    int ks = g_files[file_id].key_start;   /* 1-based */
    int rl = g_files[file_id].reclen;
    FILE *fp = g_files[file_id].fp;
    if (!fp) return;

    int cap = 64, n = 0;
    struct IdxEntry *ents = (struct IdxEntry *)malloc(sizeof(struct IdxEntry) * cap);
    if (!ents) return;

    long save = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *line = (char *)malloc(1024);
    if (!line) { fseek(fp, save, SEEK_SET); free(ents); return; }
    while (read_one(file_id, line, 1024)) {
        /* Extract the key slice [ks-1 .. ks-1+kl-1], space-padded to kl. */
        if (n == cap) { cap *= 2; ents = (struct IdxEntry *)realloc(ents, sizeof(struct IdxEntry) * cap); if (!ents) { fseek(fp, save, SEEK_SET); return; } }
        char *k = (char *)malloc((size_t)(kl > 0 ? kl : rl) + 1);
        if (kl > 0 && ks > 0) {
            int i;
            for (i = 0; i < kl; ++i) {
                int pos = ks - 1 + i;
                k[i] = (pos < (int)strlen(line)) ? line[pos] : ' ';
            }
            k[kl] = '\0';
        } else {
            /* No key: use the whole record (RRN-only access relies on offset order). */
            strncpy(k, line, (size_t)rl);
            k[rl] = '\0';
        }
        ents[n].key = k;
        /* last_rec_off was set by read_one to this record's start. */
        ents[n].off = g_files[file_id].last_rec_off;
        ++n;
    }
    /* Sort by key if a key is declared. */
    if (kl > 0) {
        s_sort_key_len = kl;
        qsort(ents, (size_t)n, sizeof(struct IdxEntry), idx_qsort_cmp);
        s_sort_key_len = 0;
    }
    /* Transfer into the per-file arrays. */
    g_files[file_id].idx_keys = (char **)malloc(sizeof(char *) * (n > 0 ? n : 1));
    g_files[file_id].idx_offs = (long *)malloc(sizeof(long) * (n > 0 ? n : 1));
    for (int i = 0; i < n; ++i) {
        g_files[file_id].idx_keys[i] = ents[i].key;
        g_files[file_id].idx_offs[i] = ents[i].off;
    }
    g_files[file_id].idx_count = n;
    g_files[file_id].idx_built = 1;
    free(ents);
    free(line);
    fseek(fp, save, SEEK_SET);
    g_files[file_id].last_rec_off = ftell(fp);
}

/* Read the record at byte offset `off` into buf (using read_one after seeking).
 * Returns 1 on success. */
static int read_at(int file_id, long off, char *buf, size_t buflen) {
    FILE *fp = g_files[file_id].fp;
    if (!fp) return 0;
    fseek(fp, off, SEEK_SET);
    return read_one(file_id, buf, buflen);
}

void rpg_rt_set_key_placeholder(void) {}  /* (kept for symbol symmetry) */

/* Format `value` as a zero-padded decimal string of width `width` into `out`
 * (right-justified, e.g. value=2 width=2 -> "02"). Used by CHAIN/SETLL/READE
 * to build a numeric key buffer. Returns the width. (Section G, G24.) */
int rpg_rt_fmt_key(long value, int width, char *out) {
    if (width < 1) width = 1;
    int neg = value < 0;
    unsigned long uv = neg ? (unsigned long)(-value) : (unsigned long)value;
    char tmp[24];
    int n = 0;
    if (uv == 0) tmp[n++] = '0';
    while (uv > 0) { tmp[n++] = (char)('0' + uv % 10); uv /= 10; }
    int pad = width - n - (neg ? 1 : 0);
    int oi = 0;
    if (neg && pad >= 0) out[oi++] = '-';
    for (int i = 0; i < pad; ++i) out[oi++] = '0';
    for (int i = n - 1; i >= 0; --i) out[oi++] = tmp[i];
    out[oi] = '\0';
    return width;
}

int rpg_rt_chain(int file_id, const char *key, int keylen, char *buf, size_t buflen) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return 0;
    int kl = g_files[file_id].key_len;
    if (kl > 0) {
        /* Keyed access: binary-search the index. */
        build_index(file_id);
        int lo = 0, hi = g_files[file_id].idx_count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = key_cmp(key, g_files[file_id].idx_keys[mid], (size_t)kl);
            if (c == 0) {
                /* First matching offset (there may be duplicate keys). */
                while (mid > 0 && key_cmp(key, g_files[file_id].idx_keys[mid-1], (size_t)kl) == 0) --mid;
                return read_at(file_id, g_files[file_id].idx_offs[mid], buf, buflen);
            }
            if (c < 0) hi = mid - 1; else lo = mid + 1;
        }
        return 0;   /* not found */
    }
    /* RRN access: keylen bytes hold a numeric relative-record number. The
     * compiler passes the RRN in `key` as a decimal string. Record N (1-based)
     * = idx_offs[N-1]. (No key declared, so the index is in file order.) */
    build_index(file_id);
    long rrn = 0;
    for (int i = 0; i < keylen; ++i) {
        if (key[i] < '0' || key[i] > '9') break;
        rrn = rrn * 10 + (key[i] - '0');
    }
    if (rrn < 1 || rrn > g_files[file_id].idx_count) return 0;
    return read_at(file_id, g_files[file_id].idx_offs[rrn - 1], buf, buflen);
}

int rpg_rt_setll(int file_id, const char *key, int keylen) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return 0;
    int kl = g_files[file_id].key_len;
    build_index(file_id);
    if (kl > 0) {
        /* lower_bound: first index entry with key >= search key. */
        int lo = 0, hi = g_files[file_id].idx_count;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (key_cmp(g_files[file_id].idx_keys[mid], key, (size_t)kl) < 0) lo = mid + 1;
            else hi = mid;
        }
        g_files[file_id].cur_off = (lo < g_files[file_id].idx_count)
            ? g_files[file_id].idx_offs[lo] : -1;
    } else {
        g_files[file_id].cur_off = 0;   /* no key: position at start */
    }
    return 1;
}

/* Read the next record from the current logical position (SETLL cursor or the
 * physical stream), advancing it. Returns 1 on record, 0 on EOF. */
int rpg_rt_read(int file_id, char *buf, size_t buflen) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return 0;
    int kl = g_files[file_id].key_len;
    if (kl > 0) {
        /* Keyed sequential read advances through the sorted index. */
        build_index(file_id);
        /* Find the index entry at/after cur_off. cur_off == -1 means exhausted. */
        if (g_files[file_id].cur_off < 0) return 0;
        int i;
        for (i = 0; i < g_files[file_id].idx_count; ++i)
            if (g_files[file_id].idx_offs[i] >= g_files[file_id].cur_off) break;
        if (i >= g_files[file_id].idx_count) { g_files[file_id].cur_off = -1; return 0; }
        int ok = read_at(file_id, g_files[file_id].idx_offs[i], buf, buflen);
        g_files[file_id].cur_off = (i + 1 < g_files[file_id].idx_count)
            ? g_files[file_id].idx_offs[i + 1] : -1;
        return ok;
    }
    /* No key: plain sequential read from the physical stream. */
    return read_one(file_id, buf, buflen);
}

/* Read the next record only if its key == search key; otherwise signal unequal
 * (return 0) without consuming the non-matching record. */
int rpg_rt_reade(int file_id, const char *key, int keylen, char *buf, size_t buflen) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return 0;
    int kl = g_files[file_id].key_len;
    if (kl <= 0) return rpg_rt_read(file_id, buf, buflen);  /* no key: behave as READ */
    build_index(file_id);
    if (g_files[file_id].cur_off < 0) return 0;
    int i;
    for (i = 0; i < g_files[file_id].idx_count; ++i)
        if (g_files[file_id].idx_offs[i] >= g_files[file_id].cur_off) break;
    if (i >= g_files[file_id].idx_count) { g_files[file_id].cur_off = -1; return 0; }
    if (key_cmp(g_files[file_id].idx_keys[i], key, (size_t)kl) != 0) return 0;  /* unequal */
    int ok = read_at(file_id, g_files[file_id].idx_offs[i], buf, buflen);
    g_files[file_id].cur_off = (i + 1 < g_files[file_id].idx_count)
        ? g_files[file_id].idx_offs[i + 1] : -1;
    return ok;
}

/* Read the prior record relative to the current logical position (the SETLL/
 * READ/READP cursor), moving it backward. Shares the same offset index as
 * rpg_rt_read/rpg_rt_setll -- key order when a key is declared, file order
 * otherwise -- so READP walks the mirror image of READ's direction. Returns
 * 1 on record, 0 on beginning-of-file. */
int rpg_rt_readp(int file_id, char *buf, size_t buflen) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return 0;
    build_index(file_id);
    int cnt = g_files[file_id].idx_count;
    if (cnt == 0) return 0;
    int i;
    if (g_files[file_id].cur_off < 0) {
        i = cnt - 1;   /* cursor past the end: prior record is the last one */
    } else {
        /* Lower bound: first index entry with offset >= cur_off; the record
         * immediately before the cursor is the one before that. */
        int lo = 0, hi = cnt;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (g_files[file_id].idx_offs[mid] < g_files[file_id].cur_off) lo = mid + 1;
            else hi = mid;
        }
        i = lo - 1;
    }
    if (i < 0) return 0;   /* beginning of file */
    int ok = read_at(file_id, g_files[file_id].idx_offs[i], buf, buflen);
    g_files[file_id].cur_off = g_files[file_id].idx_offs[i];
    return ok;
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

/* LOKUP. Scans arr[0..count-1] for `key`. Updates *idx (1-based) on a match --
 * an exact match, OR the nearest qualifying HI/LO element (manual 113147-
 * 113162: "the entry ... nearest to ... the search word"), not just an exact
 * match (A11). The array/table must be in the given sequence (manual
 * 80417-80459, E-spec column 45; `ascending` nonzero = 'A', zero = 'D' or
 * unspecified -- see B2) for HI/LO to mean anything:
 *
 *   ascending:  nearest lower  = the LAST element seen that is < key
 *               (each later one seen is larger, so closer to key from below);
 *               nearest higher = the FIRST element seen that is > key
 *               (later ones only grow, so farther from key).
 *   descending: mirror image -- nearest lower is the FIRST seen < key,
 *               nearest higher is the LAST seen > key.
 *
 * Returns 0=equal found, +1=higher found, -1=lower found, -2=nothing. */
int rpg_rt_lokup(long key, const int *arr, int count, int *idx, int ascending) {
    int start = (idx && *idx > 0) ? *idx - 1 : 0;
    int have_hi = 0, have_lo = 0;
    int hi_idx = -1, lo_idx = -1;
    for (int i = start; i < count; ++i) {
        if (arr[i] == key) {
            if (idx) *idx = i + 1;
            return 0;
        }
        if (arr[i] > key) {
            if (ascending) {
                if (!have_hi) { have_hi = 1; hi_idx = i; }   /* first wins */
            } else {
                have_hi = 1; hi_idx = i;                     /* last wins */
            }
        } else {
            if (ascending) {
                have_lo = 1; lo_idx = i;                     /* last wins */
            } else {
                if (!have_lo) { have_lo = 1; lo_idx = i; }    /* first wins */
            }
        }
    }
    /* Equal takes precedence (handled above); between HI/LO, HI wins if both
     * indicators were assigned (manual: "if resulting indicators are assigned
     * both to high and to low, the indicator assigned to low is ignored") --
     * that policy is applied by the caller via which indicator it reads, so
     * here we just report whichever nearest match(es) exist. */
    if (have_hi) { if (idx) *idx = hi_idx + 1; return +1; }
    if (have_lo) { if (idx) *idx = lo_idx + 1; return -1; }
    if (idx) *idx = 1;   /* manual: unsuccessful search resets the index to 1 */
    return -2;
}

/* SORTA (Group C, C4): sort a numeric array in place, ascending or descending
 * per its declared E-spec sequence flag. A plain insertion sort -- these
 * arrays are small (E-spec-declared, compile-time element counts). */
void rpg_rt_sorta(int *arr, int count, int ascending) {
    for (int i = 1; i < count; ++i) {
        int v = arr[i];
        int j = i - 1;
        if (ascending) {
            while (j >= 0 && arr[j] > v) { arr[j + 1] = arr[j]; --j; }
        } else {
            while (j >= 0 && arr[j] < v) { arr[j + 1] = arr[j]; --j; }
        }
        arr[j + 1] = v;
    }
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

/* Decode one `len`-byte field at `p` per `fmt` (0=zoned ASCII, 1=packed,
 * 2=binary), reusing the same decoders ordinary input fields use so a
 * prerun-time array/table sees the identical packed/binary encoding rules
 * (E7). `rpg_rt_get_packed`/`rpg_rt_get_binary` index 1-based [from..to]
 * within a `reclen`-byte record; a bare `len`-byte chunk is exactly that
 * record, so from=1, to=reclen=len. */
static long decode_field(const char *p, int len, int fmt) {
    if (fmt == 1) return rpg_rt_get_packed(p, len, 1, len);
    if (fmt == 2) return rpg_rt_get_binary(p, len, 1, len);
    return parse_dec(p, len);
}

int rpg_rt_load_arrays(const char *path, int len_a, int len_b,
                       int total, int *out_a, int *out_b,
                       int fmt_a, int fmt_b) {
    if (!path || !out_a || total <= 0 || len_a <= 0) return 0;
    FILE *fp = fopen(path, "rb");
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

    /* E7: packed/binary data is a flat sequence of fixed-width bytes with no
     * line structure -- unlike zoned ASCII, a packed/binary byte can equal
     * '\n' or '\r' by coincidence of its digit/sign encoding, so those bytes
     * must not be skipped as record separators the way zoned text is. */
    int line_delimited = (fmt_a == 0 && (len_b <= 0 || fmt_b == 0));

    int stored = 0;            /* elements written to out_a */
    int i = 0;                 /* cursor into buf */
    int pair = 0;              /* partner elements written (out_b) */
    while (stored < total && i + len_a <= (int)n) {
        if (line_delimited) {
            /* Skip a newline before the next primary element (records are
             * newline-delimited; element fields are fixed width). */
            while (i < (int)n && (buf[i] == '\n' || buf[i] == '\r')) ++i;
            if (i + len_a > (int)n) break;
        }
        out_a[stored++] = (int)decode_field(buf + i, len_a, fmt_a);
        i += len_a;
        if (len_b > 0 && out_b && pair < total) {
            if (i + len_b <= (int)n) {
                out_b[pair++] = (int)decode_field(buf + i, len_b, fmt_b);
                i += len_b;
            } else {
                out_b[pair++] = 0;
            }
        }
    }
    free(buf);
    return stored;
}

/* Alphameric counterpart of rpg_rt_load_arrays (A9): copies raw fixed-width
 * bytes instead of parsing a decimal value, so a prerun-time array/table
 * declared alphanumeric (E-spec col 44 blank) loads its actual text instead
 * of being silently skipped. Mirrors the numeric loader's line-skip and
 * missing-partner handling. */
int rpg_rt_load_char_arrays(const char *path, int len_a, int len_b,
                            int total, char *out_a, char *out_b) {
    if (!path || !out_a || total <= 0 || len_a <= 0) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "rpg_rt: cannot open prerun-time array file '%s'\n", path);
        return 0;
    }
    if (len_b < 0) len_b = 0;

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

    int stored = 0;
    int i = 0;
    int pair = 0;
    while (stored < total && i + len_a <= (int)n) {
        while (i < (int)n && (buf[i] == '\n' || buf[i] == '\r')) ++i;
        if (i + len_a > (int)n) break;
        memcpy(out_a + (size_t)stored * len_a, buf + i, (size_t)len_a);
        stored++;
        i += len_a;
        if (len_b > 0 && out_b && pair < total) {
            if (i + len_b <= (int)n) {
                memcpy(out_b + (size_t)pair * len_b, buf + i, (size_t)len_b);
                i += len_b;
            } else {
                memset(out_b + (size_t)pair * len_b, ' ', (size_t)len_b);
            }
            pair++;
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
                    char fill, char *out, int out_cap) {
    int neg = value < 0;
    unsigned long v = neg ? (unsigned long)(-value) : (unsigned long)value;
    if (decimals < 0) decimals = 0;

    /* Determine formatting flags from the edit code (Table 8, manual
     * 62103-62330). Sign style: 1-4 print no sign at all; A-D print a
     * trailing CR; J-M print a trailing minus; N-Q (not in this manual's
     * table but a harmless AS/400-compatible extension) print a leading
     * sign. Comma pairing is {1,2}/{A,B}/{J,K} = commas, {3,4}/{C,D}/{L,M} =
     * no commas -- NOT tied to the odd/even code letter the way the old
     * table mistakenly assumed. Zero-balance: the *second* code of each pair
     * (2,4,B,D,K,M) blanks a zero value entirely instead of printing
     * ".00"/"0" (A6). */
    int use_comma = 0, use_decimal = 0, zero_blank = 0;
    enum { SIGN_NONE = 0, SIGN_CR = 1, SIGN_MINUS = 2, SIGN_LEAD = 3 };
    int sign_style = SIGN_NONE;
    char c = code;
    if (c >= 'A' && c <= 'D') { sign_style = SIGN_CR;    c = (char)('1' + (c - 'A')); }
    else if (c >= 'J' && c <= 'M') { sign_style = SIGN_MINUS; c = (char)('1' + (c - 'J')); }
    else if (c >= 'N' && c <= 'Q') { sign_style = SIGN_LEAD;  c = (char)('1' + (c - 'N')); }
    switch (c) {
        case '1': use_comma = 1; use_decimal = 1; zero_blank = 0; break;
        case '2': use_comma = 1; use_decimal = 1; zero_blank = 1; break;
        case '3': use_comma = 0; use_decimal = 0; zero_blank = 0; break;
        case '4': use_comma = 0; use_decimal = 0; zero_blank = 1; break;
        default:
            /* Unknown code: plain decimal. */
            break;
    }
    /* When the field carries decimal positions, force a decimal point even if
     * the edit code (3/4) would normally omit it. */
    if (decimals > 0) use_decimal = 1;

    /* Zero-balance blank rule (A6): the value (including any decimals) is
     * exactly zero and this code's pairing calls for blanking rather than
     * printing ".00"/"0". Render as pure blanks, padded to `width` like the
     * normal path but with no digits at all. */
    if (zero_blank && value == 0) {
        int total = width > 0 ? width : 0;
        if (total + 1 > out_cap) total = out_cap - 1;
        int oi = 0;
        for (int i = 0; i < total; ++i) out[oi++] = ' ';
        out[oi] = '\0';
        return oi;
    }

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
    if (sign_style == SIGN_LEAD) { buf[len++] = neg ? '-' : '+'; }
    /* A13: a floating fill character (O-spec cols 45-47 -- a currency symbol
     * quoted like '$', or a bare '*') prints immediately to the left of the
     * first digit (manual 62678-62762, Figures 166-168). Since this function
     * never pads to a fixed width (A1: the caller right-aligns the natural-
     * length result itself), "immediately left of the first digit" is simply
     * "right here", after any leading sign and before the integer digits. */
    if (fill) buf[len++] = fill;
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
    /* trailing sign / CR. SIGN_NONE (codes 1-4) prints no sign at all --
     * the manual's own headline for edit codes: "all of them remove the
     * sign of the field" except where the code explicitly adds one back. */
    if (sign_style == SIGN_MINUS && neg) buf[len++] = '-';
    else if (sign_style == SIGN_CR && neg) { buf[len++] = 'C'; buf[len++] = 'R'; }

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
    return rpg_rt_edit_dec(value, code, width, 0, 0, out, out_cap);
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
                     int decimals, char currency, char *out, int out_cap) {
    if (!currency) currency = '$';
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
    /* A13/D1: the currency symbol (H-spec col 18, default '$') directly
     * followed by '0' floats -- it prints immediately to the left of the
     * first significant digit instead of at its own position (manual
     * 63666-63669). A currency char not followed by '0' is left alone (falls
     * through as an ordinary constant, unchanged). */
    int curr = -1;
    for (int i = 0; i < word_len; ++i) {
        char w = word[i];
        int rep = (w == ' ' || w == '0' || w == '*');
        if (rep) {
            nrep++;
            if (stop < 0 && (w == '0' || w == '*')) { stop = i; stopch = w; }
        }
        if (w == '-' && i > 0 && word[i-1] == ' ') negat = i;
        if (curr < 0 && w == currency && i + 1 < word_len && word[i+1] == '0') curr = i;
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
    int curr_pending = (curr >= 0);   /* A13: floating '$' not yet placed */
    for (int i = 0; i < word_len && oi < (int)sizeof(tmp) - 2; ++i) {
        char w = word[i];
        if (i == curr) continue;   /* A13: prints later, not at its own slot */
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
                if (curr_pending) { tmp[oi++] = currency; curr_pending = 0; }
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
            g_files[id].is_update = 0;
            g_files[id].reclen   = 0;
            g_files[id].line     = 0;
            g_files[id].page     = 1;   /* the first page is page 1 (D14) */
            g_files[id].lines_per_page = 66;  /* default page depth (F22) */
            g_files[id].overflow_line   = 60;  /* default overflow line (F22) */
            g_files[id].overflow_latched = 0;
            g_files[id].key_start = 0;
            g_files[id].key_len   = 0;
            g_files[id].last_rec_off = 0;
            g_files[id].cur_off   = 0;
            g_files[id].idx_keys  = NULL;
            g_files[id].idx_offs  = NULL;
            g_files[id].idx_count = 0;
            g_files[id].idx_built = 0;
            return id;
        }
    }
    fprintf(stderr, "rpg_rt: too many open files\n");
    return -1;
}

/* Open an update file (Section G, G25): read+write, in place, no truncate. */
int rpg_rt_open_update(const char *path) {
    for (int id = 0; id < RPG_RT_MAX_FILES; ++id) {
        if (g_files[id].fp == NULL) {
            FILE *fp = fopen(path, "r+");   /* must exist; read+write */
            if (!fp) {
                fprintf(stderr, "rpg_rt: cannot open update file '%s'\n", path);
                return -1;
            }
            g_files[id].fp       = fp;
            g_files[id].is_input = 1;   /* readable like an input file */
            g_files[id].is_update = 1;
            g_files[id].reclen   = 0;
            g_files[id].line     = 0;
            g_files[id].page     = 1;
            g_files[id].lines_per_page = 66;
            g_files[id].overflow_line   = 60;
            g_files[id].overflow_latched = 0;
            g_files[id].key_start = 0;
            g_files[id].key_len   = 0;
            g_files[id].last_rec_off = 0;
            g_files[id].cur_off   = 0;
            g_files[id].idx_keys  = NULL;
            g_files[id].idx_offs  = NULL;
            g_files[id].idx_count = 0;
            g_files[id].idx_built = 0;
            return id;
        }
    }
    fprintf(stderr, "rpg_rt: too many open files\n");
    return -1;
}

/* Append a record to `file_id` (O-spec ADD, G25). The buffer holds a complete
 * fixed-width record; it is written followed by a newline. */
void rpg_rt_write_rec(int file_id, const char *buf, int len) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return;
    FILE *fp = g_files[file_id].fp;
    long end = ftell(fp);
    /* Seek to end so writes append regardless of the read cursor. */
    long cur = end;
    fseek(fp, 0, SEEK_END);
    fwrite(buf, 1, (size_t)len, fp);
    fputc('\n', fp);
    fflush(fp);
    fseek(fp, cur, SEEK_SET);
}

/* Rewrite the most recently read record in place (O-spec UPDATE, G25). The
 * record is written at last_rec_off, padded to reclen. */
void rpg_rt_update_rec(int file_id, const char *buf, int len) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return;
    FILE *fp = g_files[file_id].fp;
    int rl = g_files[file_id].reclen;
    long off = g_files[file_id].last_rec_off;
    long cur = ftell(fp);
    fseek(fp, off, SEEK_SET);
    fwrite(buf, 1, (size_t)len, fp);
    /* pad to the fixed record width so the next record stays aligned */
    for (int i = len; i < rl; ++i) fputc(' ', fp);
    fputc('\n', fp);
    fflush(fp);
    fseek(fp, cur, SEEK_SET);
}

/* Delete the most recently read record (O-spec DEL, G25). Per the manual, a
 * deleted record is filled with hexadecimal FFs in place. */
void rpg_rt_delete_rec(int file_id) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return;
    FILE *fp = g_files[file_id].fp;
    int rl = g_files[file_id].reclen;
    long off = g_files[file_id].last_rec_off;
    long cur = ftell(fp);
    fseek(fp, off, SEEK_SET);
    for (int i = 0; i < rl; ++i) fputc('\xFF', fp);
    fputc('\n', fp);
    fflush(fp);
    fseek(fp, cur, SEEK_SET);
}

/* Write/rewrite the current line buffer (populated by line_begin/line_put_*) as
 * a disk record. `op` 0 = ADD (append), 1 = UPDATE (in-place). Lets O-spec
 * ADD/UPDATE records reuse the printer field-placement logic. (Section G, G25.) */
void rpg_rt_flush_rec(int file_id, int op) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES || !g_files[file_id].fp) return;
    int len = g_line_len;
    while (len > 1 && g_line[len - 1] == ' ') --len;   /* rtrim like emit_line */
    if (op == 1) rpg_rt_update_rec(file_id, g_line, len);
    else         rpg_rt_write_rec(file_id, g_line, len);
    g_line_len = 0;
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
    /* Overflow detection (Section F, F22): latching when the output reaches
     * or passes the configured overflow line. */
    if (g_files[file_id].overflow_line > 0 &&
        g_files[file_id].line >= g_files[file_id].overflow_line) {
        g_files[file_id].overflow_latched = 1;
    }
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
    int new_page = 0;
    if (line_no <= cur || cur == 0) {
        /* New page: form-feed, reset line, advance page counter. */
        fputc('\f', fp);
        g_files[file_id].line = 0;
        g_files[file_id].page += 1;
        cur = 0;
        new_page = 1;
    }
    /* Advance to the requested line with blank lines. */
    while (g_files[file_id].line < line_no - 1) {
        fputc('\n', fp);
        g_files[file_id].line++;
    }
    /* Overflow detection (Section F, F22). Skipping past the overflow line on
     * the SAME page latches overflow; a skip that started a new page does not. */
    if (!new_page && g_files[file_id].overflow_line > 0 &&
        g_files[file_id].line >= g_files[file_id].overflow_line) {
        g_files[file_id].overflow_latched = 1;
    }
}

/* Configure overflow for a PRINTER file (Section F, F22). Called once after the
 * file is opened when the program assigns an overflow indicator. The line
 * numbers come from the line-counter (L) spec, or the manual defaults. */
void rpg_rt_set_overflow(int file_id, int lines_per_page, int overflow_line) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES) return;
    if (lines_per_page > 0) g_files[file_id].lines_per_page = lines_per_page;
    g_files[file_id].overflow_line = overflow_line;
    g_files[file_id].overflow_latched = 0;
}

/* Return 1 if the overflow line was reached since the last call, then clear the
 * latch (Section F, F22). The generated cycle polls this at total time to drive
 * the overflow indicator. */
int rpg_rt_take_overflow(int file_id) {
    if (file_id < 0 || file_id >= RPG_RT_MAX_FILES) return 0;
    int v = g_files[file_id].overflow_latched;
    g_files[file_id].overflow_latched = 0;
    return v;
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
        /* Free the keyed index (Section G). */
        if (g_files[id].idx_keys) {
            for (int i = 0; i < g_files[id].idx_count; ++i)
                free(g_files[id].idx_keys[i]);
            free(g_files[id].idx_keys);
            g_files[id].idx_keys = NULL;
        }
        if (g_files[id].idx_offs) {
            free(g_files[id].idx_offs);
            g_files[id].idx_offs = NULL;
        }
        g_files[id].idx_count = 0;
        g_files[id].idx_built = 0;
    }
}

/* TIME (Group C, C5): current time-of-day as a 6-digit hhmmss integer
 * (manual 124880-124913). This compiler represents numeric fields as plain
 * 32-bit scaled integers with no separate digit-length attribute, so only
 * the always-fitting 6-digit time-of-day form is produced; see the codegen
 * comment on emit_time for the 12-digit time+date variant's limitation. */
long rpg_rt_time(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    return (long)tmv.tm_hour * 10000 + (long)tmv.tm_min * 100 + (long)tmv.tm_sec;
}

/* -------------------------------------------------------------------------- */
/* Program linkage: a runtime name -> entry-point registry. Every compiled    */
/* program self-registers at process startup; CALL becomes a lookup +        */
/* indirect call here instead of a direct LLVM call.                         */
/* -------------------------------------------------------------------------- */
#define RPG_RT_MAX_PROGRAMS   64
#define RPG_RT_MAX_CALL_DEPTH 32
#define RPG_RT_PROG_NAME_LEN  32

static struct {
    char name[RPG_RT_PROG_NAME_LEN];
    rpg_entry_fn fn;
    int initialized;
} g_programs[RPG_RT_MAX_PROGRAMS];
static int g_program_count = 0;

/* The programs currently "on the stack" (a CALL in progress), used to
 * reject a program calling itself or a program higher in the stack (manual
 * restriction). */
static char g_call_stack[RPG_RT_MAX_CALL_DEPTH][RPG_RT_PROG_NAME_LEN];
static int  g_call_depth = 0;

static int find_program(const char *name) {
    for (int i = 0; i < g_program_count; ++i) {
        if (strcmp(g_programs[i].name, name) == 0) return i;
    }
    return -1;
}

void rpg_rt_register_program(const char *name, rpg_entry_fn fn) {
    int idx = find_program(name);
    if (idx < 0) {
        if (g_program_count >= RPG_RT_MAX_PROGRAMS) {
            fprintf(stderr, "rpg_rt: too many registered programs (max %d)\n",
                    RPG_RT_MAX_PROGRAMS);
            return;
        }
        idx = g_program_count++;
        strncpy(g_programs[idx].name, name, RPG_RT_PROG_NAME_LEN - 1);
        g_programs[idx].name[RPG_RT_PROG_NAME_LEN - 1] = '\0';
        g_programs[idx].initialized = 0;
    }
    g_programs[idx].fn = fn;
}

int rpg_rt_call(const char *name, void **parm_ptrs, int parm_count,
               int *out_error_ind, int *out_lr_ind) {
    *out_error_ind = 0;
    *out_lr_ind = 0;

    int idx = find_program(name);
    if (idx < 0) {
        fprintf(stderr,
                "rpg_rt: CALL '%s': no such program compiled into this "
                "executable\n", name);
        *out_error_ind = 1;
        return -1;
    }
    for (int i = 0; i < g_call_depth; ++i) {
        if (strcmp(g_call_stack[i], name) == 0) {
            fprintf(stderr,
                    "rpg_rt: CALL '%s': a program cannot call itself or a "
                    "program higher in the program stack\n", name);
            *out_error_ind = 1;
            return -1;
        }
    }
    if (g_call_depth >= RPG_RT_MAX_CALL_DEPTH) {
        fprintf(stderr, "rpg_rt: CALL '%s': program stack too deep (max %d)\n",
                name, RPG_RT_MAX_CALL_DEPTH);
        *out_error_ind = 1;
        return -1;
    }

    int first_call = !g_programs[idx].initialized;
    g_programs[idx].initialized = 1;

    strncpy(g_call_stack[g_call_depth], name, RPG_RT_PROG_NAME_LEN - 1);
    g_call_stack[g_call_depth][RPG_RT_PROG_NAME_LEN - 1] = '\0';
    g_call_depth++;

    int status = g_programs[idx].fn(parm_ptrs, parm_count, first_call);

    g_call_depth--;

    *out_error_ind = (status == 2);
    *out_lr_ind    = (status == 1);
    return 0;
}

int rpg_rt_free(const char *name) {
    int idx = find_program(name);
    if (idx < 0 || !g_programs[idx].initialized) return 1;
    g_programs[idx].initialized = 0;
    return 0;
}

int rpg_rt_field_to_cstr(const char *field, int len, char *out, int out_cap) {
    int n = len;
    while (n > 0 && field[n - 1] == ' ') n--;
    if (out_cap > 0 && n > out_cap - 1) n = out_cap - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) {
        char ch = field[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        out[i] = ch;
    }
    if (out_cap > 0) out[n] = '\0';
    return n;
}

/* -------------------------------------------------------------------------- */
/* WORKSTN (Chapter 7, "Using a WORKSTN File") -- W5.                         */
/*                                                                            */
/* Two backends behind one interface (rpg_runtime.h has the full contract):   */
/* "terminal" drives the real controlling tty via ANSI/VT100 escapes;         */
/* "headless" reads a line-oriented script and dumps each written screen,     */
/* for the non-interactive regression-test harness. Display-format layouts   */
/* are parsed from the .dspf file at runtime by a small parser mirroring the  */
/* compiler's own sspec.cpp/dspec.cpp column layout (see docs/SPEC_MAP.md);   */
/* duplicated rather than shared because the compiler-side parser lives in a  */
/* C++ static library this plain-C runtime doesn't link against.             */
/* -------------------------------------------------------------------------- */
#define WS_MAX_FILES    8
#define WS_MAX_FORMATS  16
#define WS_MAX_FIELDS   48
#define WS_MAX_DEVICES  8

typedef struct {
    char name[9];       /* blank => literal (see text) */
    int  usage;          /* 0=Output 1=Input 2=Both */
    int  row, col;        /* screen position (terminal backend) */
    int  from, to;        /* 1-based record-buffer byte range */
    int  decimals;        /* -1 = alphameric */
    int  protect;
    char color;
    int  reverse;
    int  blink;
    char text[80];        /* literal text (name blank) */
} ws_field_t;

typedef struct {
    char name[9];
    int  reclen;
    int  nfields;
    ws_field_t fields[WS_MAX_FIELDS];
} ws_format_t;

typedef struct {
    int  in_use;
    int  mode;                          /* 0=terminal 1=headless */
    int  nformats;
    ws_format_t formats[WS_MAX_FORMATS];
    int  ndevices;
    char devices[WS_MAX_DEVICES][3];
    int  next_device_forced;            /* -1 = none, else index into devices[] */
    int  last_read_device;              /* -1 = none */
    char last_written_format[9];
    FILE *headless_script;
    FILE *headless_dump;
} ws_file_t;

static ws_file_t g_ws[WS_MAX_FILES];

static ws_file_t *ws_get(int ws_id) {
    if (ws_id < 0 || ws_id >= WS_MAX_FILES || !g_ws[ws_id].in_use) return NULL;
    return &g_ws[ws_id];
}

/* Extract 1-based inclusive columns [first,last] from `line` (length `len`),
 * trimmed of surrounding whitespace, into `out` (capacity `outcap`). Mirrors
 * source.cpp's col_trim() for the compiler-side parser. */
static void ws_col_trim(const char *line, int len, int first, int last,
                        char *out, int outcap) {
    int begin = first - 1, end = last;
    if (begin < 0) begin = 0;
    if (end > len) end = len;
    int n = end - begin;
    if (n < 0) n = 0;
    if (n > outcap - 1) n = outcap - 1;
    if (begin < len && n > 0) memcpy(out, line + begin, (size_t)n);
    out[n < 0 ? 0 : n] = '\0';
    /* trim leading */
    int i = 0;
    while (out[i] == ' ' || out[i] == '\t') i++;
    if (i > 0) memmove(out, out + i, strlen(out + i) + 1);
    /* trim trailing */
    int m = (int)strlen(out);
    while (m > 0 && (out[m-1] == ' ' || out[m-1] == '\t')) out[--m] = '\0';
}

static void ws_upper(char *s) {
    for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

/* Parse a .dspf display-format file into `wf`'s format table. Same column
 * layout as sspec.cpp/dspec.cpp: S-line (col 6) name in 7-14, function/
 * command key lists in 16-39/41-70 (unused by this runtime -- function-key
 * *enablement* is a compile-time-only concern here, see WRKSTN_PLAN's W2);
 * D-line (col 6) usage col 16, row 18-19, col 21-22, protect col 31, color
 * col 33, reverse col 35, blink col 37, buffer From/To 44-47/48-51, decimals
 * col 52, name 53-58, literal text (quoted) from col 60 when name is blank. */
static int ws_load_formats(const char *path, ws_file_t *wf) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char raw[512];
    ws_format_t *cur = NULL;
    while (fgets(raw, sizeof raw, fp)) {
        int len = (int)strlen(raw);
        while (len > 0 && (raw[len-1] == '\n' || raw[len-1] == '\r')) raw[--len] = '\0';
        if (len < 6) continue;
        char ft = raw[5];              /* column 6 */
        if (len >= 7 && raw[6] == '*') continue;   /* comment line */
        char tmp[80];
        if (ft == 'S') {
            if (wf->nformats >= WS_MAX_FORMATS) continue;
            cur = &wf->formats[wf->nformats++];
            memset(cur, 0, sizeof *cur);
            ws_col_trim(raw, len, 7, 14, tmp, sizeof tmp);
            strncpy(cur->name, tmp, sizeof(cur->name) - 1);
        } else if (ft == 'D') {
            if (!cur || cur->nfields >= WS_MAX_FIELDS) continue;
            ws_field_t *f = &cur->fields[cur->nfields++];
            memset(f, 0, sizeof *f);
            ws_col_trim(raw, len, 53, 58, tmp, sizeof tmp);
            strncpy(f->name, tmp, sizeof(f->name) - 1);
            ws_col_trim(raw, len, 16, 16, tmp, sizeof tmp); ws_upper(tmp);
            f->usage = (tmp[0] == 'I') ? 1 : (tmp[0] == 'B') ? 2 : 0;
            ws_col_trim(raw, len, 18, 19, tmp, sizeof tmp);
            f->row = tmp[0] ? atoi(tmp) : 1;
            ws_col_trim(raw, len, 21, 22, tmp, sizeof tmp);
            f->col = tmp[0] ? atoi(tmp) : 1;
            ws_col_trim(raw, len, 31, 31, tmp, sizeof tmp); ws_upper(tmp);
            f->protect = (tmp[0] == 'P');
            ws_col_trim(raw, len, 33, 33, tmp, sizeof tmp); ws_upper(tmp);
            f->color = tmp[0];
            ws_col_trim(raw, len, 35, 35, tmp, sizeof tmp); ws_upper(tmp);
            f->reverse = (tmp[0] == 'R');
            ws_col_trim(raw, len, 37, 37, tmp, sizeof tmp); ws_upper(tmp);
            f->blink = (tmp[0] == 'B');
            ws_col_trim(raw, len, 44, 47, tmp, sizeof tmp);
            f->from = tmp[0] ? atoi(tmp) : 0;
            ws_col_trim(raw, len, 48, 51, tmp, sizeof tmp);
            f->to = tmp[0] ? atoi(tmp) : 0;
            if (f->to == 0) f->to = f->from;
            ws_col_trim(raw, len, 52, 52, tmp, sizeof tmp);
            f->decimals = (tmp[0] >= '0' && tmp[0] <= '9') ? (tmp[0] - '0') : -1;
            if (f->name[0] == '\0' && len >= 60) {
                char lit[128];
                ws_col_trim(raw, len, 60, len, lit, sizeof lit);
                int n = (int)strlen(lit);
                if (n >= 2 && lit[0] == '\'' && lit[n-1] == '\'') {
                    lit[n-1] = '\0';
                    memmove(lit, lit + 1, (size_t)n - 1);
                }
                strncpy(f->text, lit, sizeof(f->text) - 1);
                if (f->from == 0) { f->from = 1; f->to = (int)strlen(f->text); }
            }
            if (f->to > cur->reclen) cur->reclen = f->to;
        }
    }
    fclose(fp);
    return 1;
}

static ws_format_t *ws_find_format(ws_file_t *wf, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < wf->nformats; ++i)
        if (strcasecmp(wf->formats[i].name, name) == 0) return &wf->formats[i];
    return NULL;
}

/* ANSI SGR foreground color for a D-spec color letter (B/R/G/W/T/Y/P), 0 if
 * unrecognized (caller skips emitting an SGR code). */
static int ws_color_sgr(char c) {
    switch (c) {
        case 'B': return 34;   /* blue */
        case 'R': return 31;   /* red */
        case 'G': return 32;   /* green */
        case 'W': return 37;   /* white */
        case 'T': return 36;   /* turquoise -> cyan */
        case 'Y': return 33;   /* yellow */
        case 'P': return 35;   /* pink -> magenta */
        default:  return 0;
    }
}

static volatile sig_atomic_t g_ws_shtdn_signal = 0;
static void ws_shtdn_handler(int sig) { (void)sig; g_ws_shtdn_signal = 1; }
static void ws_install_shtdn_handler(void) {
    static int installed = 0;
    if (installed) return;
    installed = 1;
    signal(SIGTERM, ws_shtdn_handler);
    signal(SIGHUP, ws_shtdn_handler);
}

int rpg_rt_ws_open(const char *fmts_path, const char *program_id) {
    (void)program_id;
    int id = -1;
    for (int i = 0; i < WS_MAX_FILES; ++i) if (!g_ws[i].in_use) { id = i; break; }
    if (id < 0) {
        fprintf(stderr, "rpg_rt: too many open WORKSTN files (max %d)\n", WS_MAX_FILES);
        return -1;
    }
    ws_file_t *wf = &g_ws[id];
    memset(wf, 0, sizeof *wf);
    if (!ws_load_formats(fmts_path, wf)) {
        fprintf(stderr, "rpg_rt: cannot read WORKSTN display file '%s'\n", fmts_path);
        return -1;
    }
    const char *modeenv = getenv("RPG_WORKSTN_MODE");
    wf->mode = (modeenv && strcmp(modeenv, "headless") == 0) ? 1 : 0;
    wf->last_read_device = -1;
    wf->next_device_forced = -1;
    if (wf->mode == 1) {
        const char *script = getenv("RPG_WORKSTN_SCRIPT");
        if (!script) {
            fprintf(stderr, "rpg_rt: RPG_WORKSTN_MODE=headless requires "
                            "RPG_WORKSTN_SCRIPT\n");
            return -1;
        }
        wf->headless_script = fopen(script, "r");
        if (!wf->headless_script) {
            fprintf(stderr, "rpg_rt: cannot open RPG_WORKSTN_SCRIPT '%s'\n", script);
            return -1;
        }
        const char *dump = getenv("RPG_WORKSTN_DUMP");
        wf->headless_dump = dump ? fopen(dump, "w") : stdout;
        if (!wf->headless_dump) wf->headless_dump = stdout;
    } else {
        ws_install_shtdn_handler();
    }
    wf->in_use = 1;
    return id;
}

int rpg_rt_ws_acquire(int ws_id, const char *device_id, char *out_device_id) {
    ws_file_t *wf = ws_get(ws_id);
    if (!wf) return 0;
    if (wf->ndevices >= WS_MAX_DEVICES) return 0;
    char id2[3] = { 0, 0, 0 };
    if (device_id && device_id[0] && device_id[0] != ' ') {
        id2[0] = device_id[0];
        id2[1] = device_id[1] ? device_id[1] : ' ';
    } else {
        id2[0] = 'T';
        id2[1] = (char)('1' + (wf->ndevices % 9));
    }
    for (int i = 0; i < wf->ndevices; ++i)
        if (memcmp(wf->devices[i], id2, 2) == 0) return 0;   /* already attached */
    memcpy(wf->devices[wf->ndevices], id2, 2);
    wf->devices[wf->ndevices][2] = '\0';
    wf->ndevices++;
    if (out_device_id) { memcpy(out_device_id, id2, 2); out_device_id[2] = '\0'; }
    return 1;
}

int rpg_rt_ws_release(int ws_id, const char *device_id) {
    ws_file_t *wf = ws_get(ws_id);
    if (!wf) return 0;
    /* NULL/blank device_id => release the device that supplied the last
     * input (same default O-spec col 16 'R' relies on -- there is no
     * factor 1 on an O-spec release, unlike the REL operation code). */
    if (!device_id || !device_id[0] || device_id[0] == ' ') {
        if (wf->last_read_device < 0 || wf->last_read_device >= wf->ndevices) return 0;
        int i = wf->last_read_device;
        for (int j = i; j < wf->ndevices - 1; ++j) memcpy(wf->devices[j], wf->devices[j+1], 3);
        wf->ndevices--;
        wf->last_read_device = -1;
        return 1;
    }
    char id2[3]; id2[0] = device_id[0]; id2[1] = device_id[1] ? device_id[1] : ' '; id2[2] = '\0';
    for (int i = 0; i < wf->ndevices; ++i) {
        if (memcmp(wf->devices[i], id2, 2) != 0) continue;
        for (int j = i; j < wf->ndevices - 1; ++j) memcpy(wf->devices[j], wf->devices[j+1], 3);
        wf->ndevices--;
        if (wf->last_read_device == i) wf->last_read_device = -1;
        else if (wf->last_read_device > i) wf->last_read_device--;
        return 1;
    }
    return 0;
}

void rpg_rt_ws_next(int ws_id, const char *device_id) {
    ws_file_t *wf = ws_get(ws_id);
    if (!wf || !device_id) return;
    char id2[3]; id2[0] = device_id[0]; id2[1] = device_id[1] ? device_id[1] : ' '; id2[2] = '\0';
    for (int i = 0; i < wf->ndevices; ++i)
        if (memcmp(wf->devices[i], id2, 2) == 0) { wf->next_device_forced = i; return; }
}

/* Map a KEY token ("ENTER", "KA".."KY", or a command-key name) to
 * (funckey, cmdkey), matching cspec.cpp's KA-KY numbering (KA=1..KY=25) and
 * the manual's Print/RollUp/RollDown/Clear/Help/Home command-key order
 * (status codes 01121-01126, so cmdkey N <-> status 1120+N). */
static void ws_parse_key(const char *tok, int *funckey, int *cmdkey) {
    *funckey = 0; *cmdkey = 0;
    if (strcmp(tok, "ENTER") == 0) return;
    if (tok[0] == 'K' && tok[1] >= 'A' && tok[1] <= 'Y' && tok[2] == '\0') {
        *funckey = tok[1] - 'A' + 1;
        return;
    }
    if      (strcmp(tok, "PRINT")    == 0) *cmdkey = 1;
    else if (strcmp(tok, "ROLLUP")   == 0) *cmdkey = 2;
    else if (strcmp(tok, "ROLLDOWN") == 0) *cmdkey = 3;
    else if (strcmp(tok, "CLEAR")    == 0) *cmdkey = 4;
    else if (strcmp(tok, "HELP")     == 0) *cmdkey = 5;
    else if (strcmp(tok, "HOME")     == 0) *cmdkey = 6;
}

/* Place `fmt`'s fields (literals always; input/both fields from `values`,
 * looked up by name) into `buf` (buflen bytes, blank-filled first). Numeric
 * fields right-justify within their width, matching this runtime's ASCII-
 * digit convention (rpg_rt_get_decimal). Shared by both backends. */
static void ws_build_record(ws_format_t *fmt, char *buf, int buflen,
                            char names[][9], char values[][80], int nvalues) {
    memset(buf, ' ', (size_t)buflen);
    if (!fmt) return;
    for (int i = 0; i < fmt->nfields; ++i) {
        ws_field_t *f = &fmt->fields[i];
        int width = f->to - f->from + 1;
        if (width <= 0 || f->from < 1 || f->from - 1 + width > buflen) continue;
        if (f->name[0] == '\0') {
            int tlen = (int)strlen(f->text);
            int cpy = tlen < width ? tlen : width;
            memcpy(buf + f->from - 1, f->text, (size_t)cpy);
        }
    }
    for (int k = 0; k < nvalues; ++k) {
        for (int i = 0; i < fmt->nfields; ++i) {
            ws_field_t *f = &fmt->fields[i];
            if (f->name[0] == '\0' || strcasecmp(f->name, names[k]) != 0) continue;
            int width = f->to - f->from + 1;
            if (width <= 0 || f->from < 1 || f->from - 1 + width > buflen) break;
            int vlen = (int)strlen(values[k]);
            int cpy = vlen < width ? vlen : width;
            memset(buf + f->from - 1, ' ', (size_t)width);
            if (f->decimals >= 0) {
                int pad = width - cpy; if (pad < 0) pad = 0;
                memcpy(buf + f->from - 1 + pad, values[k], (size_t)cpy);
            } else {
                memcpy(buf + f->from - 1, values[k], (size_t)cpy);
            }
            break;
        }
    }
}

/* Headless read: consume one script block (FORMAT/DEVICE/FIELD lines up to
 * a terminating KEY line) from RPG_WORKSTN_SCRIPT. Returns 0 (EOF) once the
 * script is exhausted. See docs/SPEC_MAP.md for the script grammar. */
static int ws_read_headless(ws_file_t *wf, char *buf, int buflen,
                            char *out_device_id, int *out_funckey,
                            int *out_cmdkey, int *out_status) {
    if (!wf->headless_script) return 0;
    char line[256];
    char fmtname[16] = "";
    char devid[3] = "";
    char names[32][9];
    char values[32][80];
    int nvalues = 0;
    int got_key = 0, funckey = 0, cmdkey = 0;

    while (fgets(line, sizeof line, wf->headless_script)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        char kw[16]; int i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && i < 15) { kw[i] = (char)toupper((unsigned char)p[i]); i++; }
        kw[i] = '\0';
        char *rest = p + i;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (strcmp(kw, "FORMAT") == 0) {
            strncpy(fmtname, rest, sizeof(fmtname) - 1);
        } else if (strcmp(kw, "DEVICE") == 0) {
            devid[0] = rest[0] ? rest[0] : ' ';
            devid[1] = rest[0] && rest[1] ? rest[1] : ' ';
            devid[2] = '\0';
        } else if (strcmp(kw, "FIELD") == 0 && nvalues < 32) {
            int j = 0;
            while (rest[j] && rest[j] != ' ' && rest[j] != '\t' && j < 8) { names[nvalues][j] = rest[j]; j++; }
            names[nvalues][j] = '\0';
            char *val = rest + j;
            while (*val == ' ' || *val == '\t') val++;
            strncpy(values[nvalues], val, 79); values[nvalues][79] = '\0';
            nvalues++;
        } else if (strcmp(kw, "KEY") == 0) {
            char keyname[16]; strncpy(keyname, rest, 15); keyname[15] = '\0';
            ws_upper(keyname);
            ws_parse_key(keyname, &funckey, &cmdkey);
            got_key = 1;
            break;
        }
    }
    if (!got_key) return 0;

    ws_format_t *fmt = fmtname[0] ? ws_find_format(wf, fmtname)
                                  : (wf->nformats ? &wf->formats[0] : NULL);
    ws_build_record(fmt, buf, buflen, names, values, nvalues);

    int devidx = -1;
    if (devid[0]) {
        for (int i = 0; i < wf->ndevices; ++i)
            if (memcmp(wf->devices[i], devid, 2) == 0) devidx = i;
    } else if (wf->next_device_forced >= 0) {
        devidx = wf->next_device_forced;
    } else if (wf->ndevices > 0) {
        devidx = 0;
    }
    wf->next_device_forced = -1;
    wf->last_read_device = devidx;
    if (out_device_id) {
        if (devidx >= 0) { memcpy(out_device_id, wf->devices[devidx], 2); out_device_id[2] = '\0'; }
        else out_device_id[0] = out_device_id[1] = out_device_id[2] = '\0';
    }
    if (out_funckey) *out_funckey = funckey;
    if (out_cmdkey)  *out_cmdkey  = cmdkey;
    if (out_status)  *out_status  = cmdkey ? (1120 + cmdkey) : (funckey ? 2 : 0);
    return 1;
}

/* Terminal read: render the last-written format via ANSI cursor positioning
 * and SGR attributes, prompt (line-buffered, not raw single-keystroke -- see
 * rpg_runtime.h) for each unprotected input/both field, then for a
 * function/command key name. */
static int ws_read_terminal(ws_file_t *wf, char *buf, int buflen,
                            char *out_device_id, int *out_funckey,
                            int *out_cmdkey, int *out_status) {
    if (wf->ndevices == 0) return 0;
    int devidx = wf->next_device_forced >= 0 ? wf->next_device_forced : 0;
    wf->next_device_forced = -1;

    ws_format_t *fmt = wf->last_written_format[0]
        ? ws_find_format(wf, wf->last_written_format)
        : (wf->nformats ? &wf->formats[0] : NULL);

    char names[32][9]; char values[32][80]; int nvalues = 0;
    if (fmt) {
        printf("\x1b[2J\x1b[H");
        for (int i = 0; i < fmt->nfields; ++i) {
            ws_field_t *f = &fmt->fields[i];
            printf("\x1b[%d;%dH", f->row, f->col);
            if (f->reverse) printf("\x1b[7m");
            if (f->blink) printf("\x1b[5m");
            int sgr = ws_color_sgr(f->color);
            if (sgr) printf("\x1b[%dm", sgr);
            if (f->name[0] == '\0') printf("%s", f->text);
            printf("\x1b[0m");
        }
        printf("\x1b[24;1H\n");
        fflush(stdout);
        for (int i = 0; i < fmt->nfields && nvalues < 32; ++i) {
            ws_field_t *f = &fmt->fields[i];
            if (f->name[0] == '\0' || f->usage == 0 || f->protect) continue;
            printf("%s (%d): ", f->name, f->to - f->from + 1);
            fflush(stdout);
            char line[128];
            if (!fgets(line, sizeof line, stdin)) return 0;
            int n = (int)strlen(line);
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
            strncpy(names[nvalues], f->name, 8); names[nvalues][8] = '\0';
            strncpy(values[nvalues], line, 79); values[nvalues][79] = '\0';
            nvalues++;
        }
    }
    ws_build_record(fmt, buf, buflen, names, values, nvalues);

    printf("key (Enter, KA-KY, PRINT, ROLLUP, ROLLDOWN, CLEAR, HELP, HOME): ");
    fflush(stdout);
    char keyline[32] = "";
    if (!fgets(keyline, sizeof keyline, stdin)) return 0;
    int n = (int)strlen(keyline);
    while (n > 0 && (keyline[n-1] == '\n' || keyline[n-1] == '\r')) keyline[--n] = '\0';
    ws_upper(keyline);
    int funckey = 0, cmdkey = 0;
    ws_parse_key(keyline[0] ? keyline : "ENTER", &funckey, &cmdkey);

    wf->last_read_device = devidx;
    if (out_device_id) { memcpy(out_device_id, wf->devices[devidx], 2); out_device_id[2] = '\0'; }
    if (out_funckey) *out_funckey = funckey;
    if (out_cmdkey)  *out_cmdkey  = cmdkey;
    if (out_status)  *out_status  = cmdkey ? (1120 + cmdkey) : (funckey ? 2 : 0);
    return 1;
}

int rpg_rt_ws_read(int ws_id, char *buf, int buflen, char *out_device_id,
                   int *out_funckey, int *out_cmdkey, int *out_status) {
    ws_file_t *wf = ws_get(ws_id);
    if (!wf) return 0;
    if (wf->ndevices == 0 && wf->mode == 0) return 0;   /* all devices released */
    return wf->mode == 1
        ? ws_read_headless(wf, buf, buflen, out_device_id, out_funckey, out_cmdkey, out_status)
        : ws_read_terminal(wf, buf, buflen, out_device_id, out_funckey, out_cmdkey, out_status);
}

void rpg_rt_ws_write(int ws_id, const char *format_name, const char *device_id,
                     const char *buf, int buflen) {
    ws_file_t *wf = ws_get(ws_id);
    if (!wf) return;
    (void)device_id;   /* SRT: exactly one device is ever attached at a time in practice */
    strncpy(wf->last_written_format, format_name ? format_name : "",
           sizeof(wf->last_written_format) - 1);
    ws_format_t *fmt = ws_find_format(wf, wf->last_written_format);

    if (wf->mode == 1) {
        FILE *out = wf->headless_dump ? wf->headless_dump : stdout;
        fprintf(out, "SCREEN %s\n", wf->last_written_format);
        if (fmt) {
            for (int i = 0; i < fmt->nfields; ++i) {
                ws_field_t *f = &fmt->fields[i];
                int width = f->to - f->from + 1;
                if (width <= 0 || f->from < 1 || f->from - 1 + width > buflen) continue;
                char valbuf[128];
                int n = width < (int)sizeof(valbuf) - 1 ? width : (int)sizeof(valbuf) - 1;
                memcpy(valbuf, buf + f->from - 1, (size_t)n); valbuf[n] = '\0';
                if (f->name[0]) fprintf(out, "FIELD %s %s\n", f->name, valbuf);
                else             fprintf(out, "LITERAL %d %d %s\n", f->row, f->col, f->text);
            }
        }
        fprintf(out, "END\n");
        fflush(out);
        return;
    }

    if (!fmt) return;
    printf("\x1b[2J\x1b[H");
    for (int i = 0; i < fmt->nfields; ++i) {
        ws_field_t *f = &fmt->fields[i];
        printf("\x1b[%d;%dH", f->row, f->col);
        if (f->reverse) printf("\x1b[7m");
        if (f->blink) printf("\x1b[5m");
        int sgr = ws_color_sgr(f->color);
        if (sgr) printf("\x1b[%dm", sgr);
        int width = f->to - f->from + 1;
        if (f->name[0] == '\0') {
            printf("%s", f->text);
        } else if (width > 0 && f->from >= 1 && f->from - 1 + width <= buflen) {
            printf("%.*s", width, buf + f->from - 1);
        }
        printf("\x1b[0m");
    }
    printf("\x1b[24;1H");
    fflush(stdout);
}

void rpg_rt_ws_flush(int ws_id, const char *format_name, const char *device_id) {
    int len = g_line_len;
    while (len > 1 && g_line[len - 1] == ' ') --len;   /* rtrim like emit_line */
    rpg_rt_ws_write(ws_id, format_name, device_id, g_line, len);
    g_line_len = 0;
}

int rpg_rt_ws_post(int ws_id, const char *device_id, int *out_size,
                   int *out_mode, int *out_inp, int *out_out) {
    ws_file_t *wf = ws_get(ws_id);
    if (!wf || !device_id) return 0;
    char id2[3]; id2[0] = device_id[0]; id2[1] = device_id[1] ? device_id[1] : ' '; id2[2] = '\0';
    int found = 0;
    for (int i = 0; i < wf->ndevices; ++i)
        if (memcmp(wf->devices[i], id2, 2) == 0) { found = 1; break; }
    if (!found) return 0;
    if (out_size) *out_size = 1920;    /* 24x80 */
    if (out_mode) *out_mode = 0;       /* no DBCS support */
    if (out_inp)  *out_inp  = 0;
    if (out_out)  *out_out  = 0;
    return 1;
}

int rpg_rt_ws_shtdn(void) {
    const char *e = getenv("RPG_WORKSTN_SHTDN");
    if (e && strcmp(e, "1") == 0) return 1;
    return g_ws_shtdn_signal ? 1 : 0;
}

static void ws_put_char(char *ptr, int width, const char *text) {
    int n = text ? (int)strlen(text) : 0;
    if (n > width) n = width;
    memset(ptr, ' ', (size_t)width);
    if (n > 0) memcpy(ptr, text, (size_t)n);
}

void rpg_rt_ws_infds(char *status_ptr, char *opcode_ptr, char *record_ptr,
                     char *size_ptr, char *mode_ptr, char *inp_ptr,
                     char *out_ptr, int status, const char *opcode,
                     const char *record, int size_val, int mode_val,
                     int inp_val, int out_val) {
    char tmp[16];
    if (status_ptr) { rpg_rt_fmt_key(status, 5, tmp); memcpy(status_ptr, tmp, 5); }
    if (opcode_ptr) ws_put_char(opcode_ptr, 5, opcode);
    if (record_ptr) ws_put_char(record_ptr, 8, record);
    if (size_ptr) { rpg_rt_fmt_key(size_val, 4, tmp); memcpy(size_ptr, tmp, 4); }
    if (mode_ptr) { rpg_rt_fmt_key(mode_val, 2, tmp); memcpy(mode_ptr, tmp, 2); }
    if (inp_ptr)  { rpg_rt_fmt_key(inp_val, 2, tmp); memcpy(inp_ptr, tmp, 2); }
    if (out_ptr)  { rpg_rt_fmt_key(out_val, 2, tmp); memcpy(out_ptr, tmp, 2); }
}
