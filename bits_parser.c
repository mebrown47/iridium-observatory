/*
 * Native port of iridium-toolkit's iridium-parser.py / bitsparser.py - see
 * bits_parser.h and docs/NATIVE_PARSER_PLAN.md.
 *
 * Frame formats ported from iridium-toolkit (https://github.com/muccc/iridium-toolkit),
 * (c) Sec & schneider, 2-Clause BSD License - see LICENSES/BSD-2-Clause-iridium-toolkit.txt
 * Reference: iridium-toolkit commit 8888124. Each function names the Python
 * one it follows. Bit strings are kept as strings of '0'/'1', as in the
 * toolkit, so the port follows it line by line.
 *
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "bits_parser.h"
#include "tk_rs.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- arena */

/* Every string of one parse lives in one arena, freed at the end. */
typedef struct arena_chunk { struct arena_chunk *next; size_t used, cap; char data[]; } arena_chunk;
typedef struct { arena_chunk *head; } arena_t;

static void *ar_alloc(arena_t *a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 ? n : 65536;
        arena_chunk *c = malloc(sizeof(*c) + cap);
        if (!c) abort();
        c->next = a->head; c->used = 0; c->cap = cap;
        a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

static void ar_free(arena_t *a)
{
    while (a->head) { arena_chunk *n = a->head->next; free(a->head); a->head = n; }
}

static char *ar_strndup(arena_t *a, const char *s, size_t n)
{
    char *d = ar_alloc(a, n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static char *ar_strdup(arena_t *a, const char *s) { return ar_strndup(a, s, strlen(s)); }

/* Python s[a:b] with a, b >= 0 (clamped like Python) */
static char *sub(arena_t *a, const char *s, size_t from, size_t to)
{
    size_t n = strlen(s);
    if (from > n) from = n;
    if (to > n) to = n;
    if (to < from) to = from;
    return ar_strndup(a, s + from, to - from);
}

static char *cat(arena_t *a, const char *x, const char *y)
{
    size_t nx = strlen(x), ny = strlen(y);
    char *d = ar_alloc(a, nx + ny + 1);
    memcpy(d, x, nx);
    memcpy(d + nx, y, ny + 1);
    return d;
}

/* ------------------------------------------------------- string builder */

typedef struct { char *s; size_t n, cap; } sb_t;

static void sb_grow(sb_t *b, size_t need)
{
    if (b->n + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (b->n + need + 1 > cap) cap *= 2;
    char *s = realloc(b->s, cap);
    if (!s) abort();
    b->s = s; b->cap = cap;
}

static void sb_puts(sb_t *b, const char *s)
{
    size_t n = strlen(s);
    sb_grow(b, n);
    memcpy(b->s + b->n, s, n + 1);
    b->n += n;
}

__attribute__((format(printf, 2, 3)))
static void sb_printf(sb_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

/* -------------------------------------------------- bitsparser helpers */

static const char iridium_access[] = "001100000011000011110011";
static const char uplink_access[]  = "110011000011110011111100";
static const char next_access_dl[] = "110011110011111111111100";
static const char next_access_ul[] = "001111000000000011111111";
static const char header_messaging[] = "00110011111100110011001111110011";
#define ACCESS_LEN 24
static const int messaging_bch_poly = 1897;
static const int ringalert_bch_poly = 1207;
static const int acch_bch_poly = 3545;
static const int hdr_poly = 29;

/* f_simplex / f_duplex, computed as bitsparser.py does */
static double f_simplex(void) { return (1626104e3 - 36e3 - 1e3) * (1 - 100e-6); }
static double f_duplex(void)  { return (1625979e3 + 36e3 + 1e3) * (1 + 100e-6); }

/* header_time_location = "11" + "0"*94 */
static int is_header_time_location(const char *s)
{
    if (strlen(s) < 96) return 0;
    if (s[0] != '1' || s[1] != '1') return 0;
    for (int i = 2; i < 96; i++) if (s[i] != '0') return 0;
    return 1;
}

static int startswith(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* bitsparser.symbol_reverse */
static char *symbol_reverse(arena_t *a, const char *bits)
{
    size_t n = strlen(bits);
    char *r = ar_strndup(a, bits, n);
    for (size_t x = 0; x + 1 < n; x += 2) { char t = r[x]; r[x] = r[x + 1]; r[x + 1] = t; }
    return r;
}

/* bitsparser.de_interleave -> (odd, even) */
static void de_interleave(arena_t *a, const char *group, char **odd, char **even)
{
    size_t n = strlen(group), ns = n / 2;       /* symbols; a trailing odd bit is ignored,
                                                 * as group[z+1] would raise for it in Python */
    char *o = ar_alloc(a, n + 1), *e = ar_alloc(a, n + 1);
    size_t no = 0, ne = 0;
    for (long x = (long)ns - 2; x >= 0; x -= 2) { e[ne++] = group[2 * x + 1]; e[ne++] = group[2 * x]; }
    for (long x = (long)ns - 1; x >= 0; x -= 2) { o[no++] = group[2 * x + 1]; o[no++] = group[2 * x]; }
    o[no] = 0; e[ne] = 0;
    *odd = o; *even = e;
}

/* bitsparser.de_interleave3 -> (first, second, third) */
static void de_interleave3(arena_t *a, const char *group, char **first, char **second, char **third)
{
    size_t n = strlen(group), ns = n / 2;
    char *f = ar_alloc(a, n + 1), *s = ar_alloc(a, n + 1), *t = ar_alloc(a, n + 1);
    size_t nf = 0, nsd = 0, nt = 0;
    for (long x = (long)ns - 3; x >= 0; x -= 3) { t[nt++] = group[2 * x + 1]; t[nt++] = group[2 * x]; }
    for (long x = (long)ns - 2; x >= 0; x -= 3) { s[nsd++] = group[2 * x + 1]; s[nsd++] = group[2 * x]; }
    for (long x = (long)ns - 1; x >= 0; x -= 3) { f[nf++] = group[2 * x + 1]; f[nf++] = group[2 * x]; }
    f[nf] = 0; s[nsd] = 0; t[nt] = 0;
    *first = f; *second = s; *third = t;
}

/* bitsparser.de_interleave_lcw -> (lcw1 7 bits, lcw2 13, lcw3 26) */
static void de_interleave_lcw(arena_t *a, const char *bits, char **l1, char **l2, char **l3)
{
    static const int tbl[46] = { 40, 39, 36, 35, 32, 31, 28, 27, 24, 23, 20, 19, 16, 15, 12, 11,  8,  7,  4,  3,
                                 41, 38, 37, 34, 33, 30, 29, 26, 25, 22, 21, 18, 17, 14, 13, 10,  9,  6,  5,  2,
                                  1, 46, 45, 44, 43, 42 };
    size_t n = strlen(bits);
    char lcw[47];
    int k = 0;
    for (int i = 0; i < 46; i++) {           /* bits[x-1:x] is "" past the end */
        size_t x = (size_t)tbl[i];
        if (x <= n) lcw[k++] = bits[x - 1];
        else lcw[k++] = 0;                    /* marker: missing */
    }
    /* join, skipping missing ones (Python joins empty strings) */
    char buf[3][47]; int nb[3] = {0, 0, 0};
    for (int i = 0; i < 46; i++) {
        int g = i < 7 ? 0 : i < 20 ? 1 : 2;
        if (lcw[i]) buf[g][nb[g]++] = lcw[i];
    }
    *l1 = ar_strndup(a, buf[0], nb[0]);
    *l2 = ar_strndup(a, buf[1], nb[1]);
    *l3 = ar_strndup(a, buf[2], nb[2]);
}

/* util.bitdiff: differences over the common length */
static int bitdiff(const char *x, const char *y)
{
    int d = 0;
    for (; *x && *y; x++, y++) d += *x != *y;
    return d;
}

/* util.slice_extra(string, n) -> blocks, extra */
static int slice_extra(arena_t *a, const char *s, size_t n, char ***blocks, char **extra)
{
    size_t len = strlen(s);
    int nblk = 0;
    for (size_t x = 0; x < len + 1; x += n) nblk++;
    char **b = ar_alloc(a, sizeof(char *) * (size_t)nblk);
    int i = 0;
    for (size_t x = 0; x < len + 1; x += n) b[i++] = sub(a, s, x, x + n);
    *extra = b[nblk - 1];
    *blocks = b;
    return nblk - 1;
}

/* ----------------------------------------------------------------- bch */

static int bit_length(uint64_t v) { int n = 0; while (v) { n++; v >>= 1; } return n; }

/* bch.nndivide */
static uint64_t nndivide(uint64_t poly, uint64_t num)
{
    if (num == 0) return 0;
    int bits = bit_length(num) - bit_length(poly);
    uint64_t pow = (uint64_t)1 << (bit_length(num) - 1);
    while (bits >= 0) {
        if (num >= pow) num ^= poly << bits;
        pow >>= 1;
        bits -= 1;
    }
    return num;
}

static uint64_t bits_to_int(const char *s)
{
    uint64_t v = 0;
    for (; *s; s++) v = (v << 1) | (uint64_t)(*s == '1');
    return v;
}

/* bch.ndivide */
static uint64_t ndivide(uint64_t poly, const char *bits) { return nndivide(poly, bits_to_int(bits)); }

/* "{0:0%db}".format(v) */
static char *fmt_bin(arena_t *a, uint64_t v, int width)
{
    int n = bit_length(v);
    if (n < width) n = width;
    char *s = ar_alloc(a, (size_t)n + 1);
    for (int i = 0; i < n; i++) s[n - 1 - i] = (char)('0' + ((v >> i) & 1));
    s[n] = 0;
    return s;
}

/* bch.syndromes, built as bch.init()/mk_syn do */
typedef struct { int ecnt; uint64_t eloc; } syn_t;
typedef struct { int poly, synbits; syn_t *tab; } syntab_t;
static syntab_t syntabs[6];
static pthread_once_t syn_once = PTHREAD_ONCE_INIT;

static void mk_syn(syntab_t *t, int poly, int bits, int synbits, int errors)
{
    t->poly = poly; t->synbits = synbits;
    t->tab = calloc((size_t)1 << synbits, sizeof(syn_t));
    if (!t->tab) abort();
    for (int n1 = 0; n1 < bits; n1++) {
        uint64_t val = (uint64_t)1 << n1;
        uint64_t r = nndivide((uint64_t)poly, val);
        t->tab[r].ecnt = 1; t->tab[r].eloc = val;
    }
    if (errors >= 2)
        for (int n1 = 0; n1 < bits; n1++)
            for (int n2 = n1 + 1; n2 < bits; n2++) {
                uint64_t val = ((uint64_t)1 << n1) | ((uint64_t)1 << n2);
                uint64_t r = nndivide((uint64_t)poly, val);
                if (t->tab[r].ecnt == 0) { t->tab[r].ecnt = 2; t->tab[r].eloc = val; }
                /* a collision raises in bch.py; the toolkit's six polys have none */
            }
}

static void syn_init(void)
{
    mk_syn(&syntabs[0], 29,   7,  4,  1);
    mk_syn(&syntabs[1], 465,  14, 8,  2);
    mk_syn(&syntabs[2], 41,   26, 5,  1);
    mk_syn(&syntabs[3], 1897, 31, 10, 2);
    mk_syn(&syntabs[4], 1207, 31, 10, 2);
    mk_syn(&syntabs[5], 3545, 31, 11, 2);
}

static syntab_t *syn_for(int poly)
{
    pthread_once(&syn_once, syn_init);
    for (int i = 0; i < 6; i++) if (syntabs[i].poly == poly) return &syntabs[i];
    return NULL;
}

/* bch.nrepair -> (errs, repaired) */
static int nrepair(arena_t *a, int poly, const char *b, char **repaired)
{
    uint64_t v = bits_to_int(b);
    uint64_t r = nndivide((uint64_t)poly, v);
    if (r == 0) { *repaired = ar_strdup(a, b); return 0; }
    syntab_t *t = syn_for(poly);
    if (!t || r >= ((uint64_t)1 << t->synbits) || t->tab[r].ecnt == 0) { *repaired = ar_strdup(a, b); return -1; }
    *repaired = fmt_bin(a, t->tab[r].eloc ^ v, (int)strlen(b));
    return t->tab[r].ecnt;
}

/* bch.nrepair1 -> (errs, repaired): one bit error by brute force */
static int nrepair1(arena_t *a, int poly, const char *b, char **repaired)
{
    uint64_t v = bits_to_int(b);
    if (nndivide((uint64_t)poly, v) == 0) { *repaired = ar_strdup(a, b); return 0; }
    int blen = (int)strlen(b);
    for (int b1 = 0; b1 < blen; b1++) {
        uint64_t v1 = v ^ ((uint64_t)1 << b1);
        if (nndivide((uint64_t)poly, v1) == 0) { *repaired = fmt_bin(a, v1, blen); return 1; }
    }
    *repaired = ar_strdup(a, b);
    return -1;
}

/* split repaired into data (all but the last deg bits) and bch */
static void bch_split(arena_t *a, int poly, const char *rep, char **data, char **bch)
{
    size_t deg = (size_t)bit_length((uint64_t)poly) - 1, n = strlen(rep);
    size_t cut = n > deg ? n - deg : 0;       /* Python rep[:-deg] / rep[-deg:] */
    if (data) *data = sub(a, rep, 0, cut);
    if (bch)  *bch  = sub(a, rep, cut, n);
}

/* bch.bch_repair -> (errs, data, bch) */
static int bch_repair(arena_t *a, int poly, const char *bits, char **data, char **bch)
{
    char *rep;
    int e = nrepair(a, poly, bits, &rep);
    bch_split(a, poly, rep, data, bch);
    return e;
}

/* bch.bch_repair1 -> (errs, data, bch) */
static int bch_repair1(arena_t *a, int poly, const char *bits, char **data, char **bch)
{
    char *rep;
    int e = nrepair1(a, poly, bits, &rep);
    bch_split(a, poly, rep, data, bch);
    return e;
}

/* ------------------------------------------------------------- message */

typedef enum {
    C_MESSAGE, C_IRIDIUM, C_LCW, C_SY, C_LCW3, C_VO, C_IP,
} cls_t;

static const char *cls_name(cls_t c)
{
    switch (c) {
    case C_MESSAGE: return "Message";
    case C_IRIDIUM: return "IridiumMessage";
    case C_LCW:     return "IridiumLCWMessage";
    case C_SY:      return "IridiumSYMessage";
    case C_LCW3:    return "IridiumLCW3Message";
    case C_VO:      return "IridiumVOMessage";
    case C_IP:      return "IridiumIPMessage";
    }
    return "?";
}

#define MAX_ERRS 16
#define MAX_DESCR 64

typedef struct {
    arena_t *a;
    cls_t cls;
    int parse_error, error;
    const char *error_msg[MAX_ERRS];
    int nerr;

    /* Message */
    int swapped, next;
    const char *filename;
    double timestamp;
    long frequency;
    char freq_print[24];
    int has_snr;
    double snr, noise;
    int has_access_ok, access_ok;
    const char *id;
    int confidence;
    double level, leveldb;
    const char *bitstream_raw;
    int symbols;
    const char *extra_data;             /* NULL = none */
    char fileinfo[96];
    int has_uplink, uplink;
    int has_ec_uw, ec_uw;
    int has_ec_lcw, ec_lcw;
    int has_fixederrs, fixederrs;

    /* IridiumMessage */
    char msgtype[4];                    /* "" = not set */
    const char *header;
    const char *descrambled[MAX_DESCR]; /* joined with "" for printing */
    int ndescr;
    const char *descramble_extra;       /* NULL = not set */
    int bc_type;
    const char *lcw1, *lcw2, *lcw3;
    int ft;

    /* IridiumLCWMessage and subclasses */
    int lcw_ft, lcw_code;
    int payload_f[64], payload_r[64], npay;   /* 8-bit, as read / bit-reversed */
    int payload_6[64], npay6;                 /* VO: 6-bit of data[:312] */
    int payload6[64], npayload6;              /* U3: 6-bit */
    int payload8[64], npayload8;              /* U3: 8-bit */
    char pattern[3];                          /* SY uplink */
    char utype[4];                            /* LCW3: IU3 / I38 / I36 */
    int rs8p, rs6p;
    int rs8m[64], nrs8m;
    unsigned csum;
    int rs6m[64], nrs6m;
    char vtype[4];                            /* VO */
    int vdata[64], nvdata;
    char itype[4];                            /* IP */
    int ip_hdr, ip_seq, ip_ack, ip_cs, ip_cs_ok;
    int ip_data[64], nip_data;
    unsigned ip_cksum;
    int flags, counter;
    int idata[64], nidata;
} msg_t;

/* Message._new_error */
static void new_error(msg_t *m, const char *text, const char *cls)
{
    m->error = 1;
    size_t n = strlen(text) + 64;
    char *s = ar_alloc(m->a, n);
    snprintf(s, n, "%s: %s", cls ? cls : cls_name(m->cls), text);
    if (m->nerr == 0 || strcmp(m->error_msg[m->nerr - 1], s) != 0)
        if (m->nerr < MAX_ERRS) m->error_msg[m->nerr++] = s;
}

static void descr_add(msg_t *m, const char *s) { if (m->ndescr < MAX_DESCR) m->descrambled[m->ndescr++] = s; }

/* ParserError: the message and the class that raised it */
typedef struct { const char *msg; cls_t cls; } perr_t;

/* bitsparser.Message.__init__ - the RAW line regex, by hand:
 * (RAW|RWA|NC1): ([^ ]*) (-?[\d.]+) (\d+) (?:N:([+-]?\d+(?:\.\d+)?)([+-]\d+(?:\.\d+)?)|A:(\w+))
 *   [IL]:(\w+) +(\d+)% ([\d.]+|inf|nan) +(\d+) ([\[\]<> 01]+)(.*) */
static int message_init(msg_t *m, const char *line)
{
    arena_t *a = m->a;
    const char *p = line;
    char kind[4];
    if (strncmp(p, "RAW: ", 5) == 0 || strncmp(p, "RWA: ", 5) == 0 || strncmp(p, "NC1: ", 5) == 0) {
        memcpy(kind, p, 3); kind[3] = 0;
        p += 5;
    } else return -1;
    const char *q = strchr(p, ' ');
    if (!q) return -1;
    m->filename = ar_strndup(a, p, (size_t)(q - p));
    p = q + 1;
    /* timestamp */
    q = p;
    if (*q == '-') q++;
    while (isdigit((unsigned char)*q) || *q == '.') q++;
    if (q == p || *q != ' ') return -1;
    m->timestamp = strtod(ar_strndup(a, p, (size_t)(q - p)), NULL);
    p = q + 1;
    /* frequency */
    q = p;
    while (isdigit((unsigned char)*q)) q++;
    if (q == p || *q != ' ') return -1;
    m->frequency = strtol(p, NULL, 10);
    p = q + 1;
    /* N:snr noise | A:access */
    if (strncmp(p, "N:", 2) == 0) {
        p += 2;
        q = p;
        if (*q == '+' || *q == '-') q++;
        if (!isdigit((unsigned char)*q)) return -1;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '.') { q++; if (!isdigit((unsigned char)*q)) return -1; while (isdigit((unsigned char)*q)) q++; }
        m->snr = strtod(ar_strndup(a, p, (size_t)(q - p)), NULL);
        p = q;
        if (*q != '+' && *q != '-') return -1;
        q++;
        if (!isdigit((unsigned char)*q)) return -1;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '.') { q++; if (!isdigit((unsigned char)*q)) return -1; while (isdigit((unsigned char)*q)) q++; }
        m->noise = strtod(ar_strndup(a, p, (size_t)(q - p)), NULL);
        m->has_snr = 1;
        if (*q != ' ') return -1;
        p = q + 1;
    } else if (strncmp(p, "A:", 2) == 0) {
        p += 2;
        q = p;
        while (isalnum((unsigned char)*q) || *q == '_') q++;
        if (q == p || *q != ' ') return -1;
        m->has_access_ok = 1;
        m->access_ok = (q - p == 2 && strncmp(p, "OK", 2) == 0);
        p = q + 1;
    } else return -1;
    /* [IL]:id */
    if ((*p != 'I' && *p != 'L') || p[1] != ':') return -1;
    p += 2;
    q = p;
    while (isalnum((unsigned char)*q) || *q == '_') q++;
    if (q == p || *q != ' ') return -1;
    m->id = ar_strndup(a, p, (size_t)(q - p));
    p = q;
    while (*p == ' ') p++;
    /* confidence% */
    q = p;
    while (isdigit((unsigned char)*q)) q++;
    if (q == p || *q != '%' || q[1] != ' ') return -1;
    m->confidence = atoi(p);
    p = q + 2;
    /* level */
    const char *lvl = p;
    q = p;
    if (strncmp(q, "inf", 3) == 0) q += 3;
    else if (strncmp(q, "nan", 3) == 0) q += 3;
    else { while (isdigit((unsigned char)*q) || *q == '.') q++; }
    if (q == p || *q != ' ') return -1;
    char *lvls = ar_strndup(a, lvl, (size_t)(q - lvl));
    p = q;
    while (*p == ' ') p++;
    /* symbols count (unused) */
    q = p;
    while (isdigit((unsigned char)*q)) q++;
    if (q == p || *q != ' ') return -1;
    p = q + 1;
    /* bits: [\[\]<> 01]+ */
    q = p;
    while (*q == '[' || *q == ']' || *q == '<' || *q == '>' || *q == ' ' || *q == '0' || *q == '1') q++;
    if (q == p) return -1;
    size_t nb = (size_t)(q - p);
    char *bits = ar_alloc(a, nb + 1);
    size_t k = 0;
    for (size_t i = 0; i < nb; i++) if (p[i] == '0' || p[i] == '1') bits[k++] = p[i];
    bits[k] = 0;
    const char *extra = q;

    m->swapped = strcmp(kind, "RWA") != 0;
    m->next = strcmp(kind, "NC1") == 0;
    if (strcmp(m->filename, "/dev/stdin") == 0) m->filename = "-";
    if (m->timestamp < 0 || m->timestamp > 1000.0 * 60 * 60 * 24 * 999)
        new_error(m, "Timestamp out of range", NULL);
    snprintf(m->freq_print, sizeof(m->freq_print), "%010ld", m->frequency);
    m->level = strtod(lvls, NULL);
    if (m->level == 0) m->level = strtod(cat(a, lvls, "1"), NULL);
    m->leveldb = 20 * (log(m->level) / log(10));   /* Python log(level, 10) */
    m->bitstream_raw = m->swapped ? symbol_reverse(a, bits) : bits;
    m->symbols = (int)(strlen(m->bitstream_raw) / 2);
    if (*extra) {
        m->extra_data = ar_strdup(a, extra);
        new_error(m, "There is crap at the end in extra_data", NULL);
    }
    /* file info: the current format i-<t0>-t1 */
    const char *f = m->filename;
    if (f[0] == 'i' && f[1] == '-' && isdigit((unsigned char)f[2])) {
        const char *d = f + 2;
        while (isdigit((unsigned char)*d)) d++;
        if (strcmp(d, "-t1") == 0) {
            snprintf(m->fileinfo, sizeof(m->fileinfo), "p-%.*s", (int)(d - (f + 2)), f + 2);
            /* %d of int(...): strip leading zeros like Python int() would */
            char *z = m->fileinfo + 2;
            while (z[0] == '0' && z[1]) memmove(z, z + 1, strlen(z));
            return 0;
        }
    }
    /* other file tags: "u-" + the name with '-' -> '.' (older formats with
     * dates in the name are not supported) */
    snprintf(m->fileinfo, sizeof(m->fileinfo), "u-%s", m->filename);
    for (char *c = m->fileinfo + 2; *c; c++) if (*c == '-') *c = '.';
    return 0;
}

/* bitsparser.Message._pretty_header (default options: flags always shown) */
static void pretty_header_message(msg_t *m, sb_t *b)
{
    char flags[16];
    int fx = m->has_fixederrs ? (m->fixederrs > 9 ? 9 : m->fixederrs) : 0;
    snprintf(flags, sizeof(flags), "-e%d%d%d", m->has_ec_uw ? m->ec_uw : 0,
             m->has_ec_lcw ? m->ec_lcw : 0, fx);
    sb_printf(b, "%s%s %014.4f", m->fileinfo, flags, m->timestamp);
    if (!m->has_snr)
        sb_printf(b, " %s %3d%% %7.3f", m->freq_print, m->confidence, m->level);
    else
        sb_printf(b, " %s %3d%% %06.2f|%07.2f|%05.2f", m->freq_print, m->confidence,
                  m->leveldb, m->noise, m->snr);
}

/* " ".join(slice(bs, 16)) */
static void put_sliced(sb_t *b, const char *bs, size_t n)
{
    size_t len = strlen(bs);
    for (size_t x = 0; x < len; x += n) {
        if (x) sb_puts(b, " ");
        char tmp[64];
        size_t k = len - x < n ? len - x : n;
        memcpy(tmp, bs + x, k); tmp[k] = 0;
        sb_puts(b, tmp);
    }
}

/* bitsparser.Message.pretty */
static void pretty_message(msg_t *m, sb_t *b)
{
    if (m->parse_error) { sb_puts(b, "ERR: "); return; }
    sb_puts(b, m->next ? "NC1: " : "RAW: ");
    pretty_header_message(m, b);
    const char *bs = m->bitstream_raw;
    if (m->has_uplink) {
        sb_printf(b, " %03d", m->symbols - ACCESS_LEN / 2);
        sb_puts(b, m->uplink ? " UL" : " DL");
        sb_printf(b, " <%s>", sub(m->a, bs, 0, ACCESS_LEN));
        bs = sub(m->a, bs, ACCESS_LEN, strlen(bs));
    }
    sb_puts(b, " ");
    put_sliced(b, bs, 16);
    if (m->extra_data) { sb_puts(b, " "); sb_puts(b, m->extra_data); }
}

/* bitsparser.IridiumMessage._pretty_header */
static void pretty_header_iridium(msg_t *m, sb_t *b)
{
    pretty_header_message(m, b);
    sb_printf(b, " %03d", m->symbols - ACCESS_LEN / 2);
    sb_puts(b, m->uplink ? " UL" : " DL");
    if (m->header && *m->header) { sb_puts(b, " "); sb_puts(b, m->header); }
}

/* bitsparser.IridiumMessage._pretty_trailer */
static void pretty_trailer_iridium(msg_t *m, sb_t *b)
{
    if (m->descramble_extra && *m->descramble_extra) {
        sb_puts(b, " descr_extra:");
        sb_puts(b, m->descramble_extra);
    }
}

static char *descr_joined(msg_t *m)
{
    size_t n = 0;
    for (int i = 0; i < m->ndescr; i++) n += strlen(m->descrambled[i]);
    char *s = ar_alloc(m->a, n + 1);
    s[0] = 0;
    size_t k = 0;
    for (int i = 0; i < m->ndescr; i++) { size_t l = strlen(m->descrambled[i]); memcpy(s + k, m->descrambled[i], l); k += l; }
    s[k] = 0;
    return s;
}

/* bitsparser.IridiumMessage.pretty (the TL <i> <q> variant needs STL: later) */
static void pretty_iridium(msg_t *m, sb_t *b)
{
    sb_puts(b, "IRI: ");
    pretty_header_iridium(m, b);
    sb_printf(b, " %2s", m->msgtype);
    const char *d = descr_joined(m);
    if (*d) {
        sb_puts(b, " [");
        size_t len = strlen(d);
        for (size_t x = 0; x < len; x += 8) {
            unsigned v = 0;
            for (size_t i = x; i < x + 8 && i < len; i++) v = (v << 1) | (unsigned)(d[i] == '1');
            sb_printf(b, "%s%02x", x ? "." : "", v);
        }
        sb_puts(b, "]");
    }
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumMessage.__init__ (default options). Returns 0, or 1 with
 * *pe set for a ParserError. */
static int iridium_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_IRIDIUM;
    const char *data;
    if (m->next) {
        data = sub(a, m->bitstream_raw, strlen(next_access_dl), strlen(m->bitstream_raw));
        strcpy(m->msgtype, "NX");
        m->header = sub(a, m->bitstream_raw, 0, strlen(next_access_dl));
        descr_add(m, data);
        return 0;
    } else if (m->uplink)
        data = sub(a, m->bitstream_raw, strlen(uplink_access), strlen(m->bitstream_raw));
    else
        data = sub(a, m->bitstream_raw, ACCESS_LEN, strlen(m->bitstream_raw));
    size_t dlen = strlen(data);
    const int freqclass = 1;
    double fr = (double)m->frequency;

    if (!m->msgtype[0] && (!freqclass || fr > f_simplex()) && !(freqclass && m->uplink))
        if (dlen >= 32 && strncmp(data, header_messaging, 32) == 0) strcpy(m->msgtype, "MS");

    if (!m->msgtype[0] && (!freqclass || fr > f_simplex()) && !(freqclass && m->uplink))
        if (is_header_time_location(data) && dlen >= 96) strcpy(m->msgtype, "TL");

    if (!m->msgtype[0] && (!freqclass || fr < f_duplex()) && !(freqclass && m->uplink)) {
        int hdrlen = 6, blocklen = 64;
        if ((int)dlen > hdrlen + blocklen && ndivide((uint64_t)hdr_poly, sub(a, data, 0, (size_t)hdrlen)) == 0) {
            char *bc1, *bc2;
            de_interleave(a, sub(a, data, (size_t)hdrlen, (size_t)(hdrlen + blocklen)), &bc1, &bc2);
            if (ndivide((uint64_t)ringalert_bch_poly, sub(a, bc1, 0, 31)) == 0 &&
                ndivide((uint64_t)ringalert_bch_poly, sub(a, bc2, 0, 31)) == 0)
                strcpy(m->msgtype, "BC");
        }
    }

    if (!m->msgtype[0] && (!freqclass || fr < f_duplex())) {
        if (dlen > 64) {
            char *l1, *l2, *l3, *d2, *bch;
            de_interleave_lcw(a, sub(a, data, 0, 46), &l1, &l2, &l3);
            if (ndivide(29, l1) == 0 && ndivide(41, l3) == 0) {
                int e2 = bch_repair(a, 465, cat(a, l2, "0"), &d2, &bch);
                if (e2 == 1) e2 = bch_repair(a, 465, cat(a, l2, "1"), &d2, &bch);
                if (e2 == 0) strcpy(m->msgtype, "LW");
            }
        }
    }

    if (!m->msgtype[0] && (!freqclass || fr > f_simplex()) && !(freqclass && m->uplink)) {
        size_t firstlen = 3 * 32;
        if (dlen >= firstlen) {
            char *r1, *r2, *r3;
            de_interleave3(a, sub(a, data, 0, firstlen), &r1, &r2, &r3);
            if (ndivide((uint64_t)ringalert_bch_poly, sub(a, r1, 0, 31)) == 0 &&
                ndivide((uint64_t)ringalert_bch_poly, sub(a, r2, 0, 31)) == 0 &&
                ndivide((uint64_t)ringalert_bch_poly, sub(a, r3, 0, 31)) == 0)
                strcpy(m->msgtype, "RA");
        }
    }

    if (!m->msgtype[0] && (!freqclass || fr < f_duplex()) && m->uplink)
        if (dlen >= 2 * 26 && dlen < 2 * 50) strcpy(m->msgtype, "AQ");

    /* (--harder detection not ported) */

    if (!m->msgtype[0] && dlen < 64) { pe->msg = "Iridium message too short"; pe->cls = C_IRIDIUM; return 1; }
    if (!m->msgtype[0]) { pe->msg = "unknown Iridium message type"; pe->cls = C_IRIDIUM; return 1; }

    if (strcmp(m->msgtype, "MS") == 0) {
        size_t hdrlen = 32;
        m->header = sub(a, data, 0, hdrlen);
        if (strcmp(m->header, header_messaging) == 0) m->header = "";
        char **blocks, *extra;
        int nb = slice_extra(a, sub(a, data, hdrlen, dlen), 64, &blocks, &extra);
        m->descramble_extra = extra;
        for (int i = 0; i < nb; i++) { char *o, *e; de_interleave(a, blocks[i], &o, &e); descr_add(m, o); descr_add(m, e); }
    } else if (strcmp(m->msgtype, "AQ") == 0) {
        size_t datalen = 2 * 26;
        m->header = "";
        descr_add(m, sub(a, data, 0, datalen));
        m->descramble_extra = sub(a, data, datalen, dlen);
    } else if (strcmp(m->msgtype, "TL") == 0) {
        size_t hdrlen = 96;
        m->header = sub(a, data, 0, hdrlen);
        descr_add(m, sub(a, data, hdrlen, hdrlen + 256 * 3));
        m->descramble_extra = sub(a, data, hdrlen + 256 * 3, dlen);
    } else if (strcmp(m->msgtype, "RA") == 0) {
        size_t firstlen = 3 * 32;
        if (dlen < firstlen) new_error(m, "No data to descramble", NULL);
        m->header = "";
        char *r1, *r2, *r3;
        de_interleave3(a, sub(a, data, 0, firstlen), &r1, &r2, &r3);
        descr_add(m, r1); descr_add(m, r2); descr_add(m, r3);
        char **blocks, *extra;
        int nb = slice_extra(a, sub(a, data, firstlen, dlen), 64, &blocks, &extra);
        m->descramble_extra = extra;
        for (int i = 0; i < nb; i++) { char *o, *e; de_interleave(a, blocks[i], &o, &e); descr_add(m, o); descr_add(m, e); }
    } else if (strcmp(m->msgtype, "BC") == 0) {
        size_t hdrlen = 6;
        m->header = sub(a, data, 0, hdrlen);
        char *d;
        int e = bch_repair1(a, hdr_poly, m->header, &d, NULL);
        m->bc_type = (int)bits_to_int(d);
        if (e < 0) {
            size_t n = strlen(m->header) + 16;
            char *h = ar_alloc(a, n);
            snprintf(h, n, "%s/E%d", m->header, e);
            m->header = h;
            new_error(m, "IBC header error", NULL);
        } else m->header = "";
        size_t ibclen = 131 * 2;
        char **blocks, *extra;
        int nb = slice_extra(a, sub(a, data, hdrlen, ibclen), 64, &blocks, &extra);
        int ndescr0 = m->ndescr;
        for (int i = 0; i < nb; i++) { char *o, *e2; de_interleave(a, blocks[i], &o, &e2); descr_add(m, o); descr_add(m, e2); }
        m->descramble_extra = cat(a, extra, sub(a, data, ibclen, dlen));
        if (m->ndescr - ndescr0 == 8) {
            m->symbols -= (int)(strlen(m->descramble_extra) / 2);
            m->descramble_extra = "";
        }
    } else if (strcmp(m->msgtype, "LW") == 0) {
        size_t lcwlen = 46;
        char *l1, *l2, *l3, *lcw2b, *bch;
        de_interleave_lcw(a, sub(a, data, 0, lcwlen), &l1, &l2, &l3);
        char *lcw1, *lcw2, *lcw3;
        int e1 = bch_repair(a, 29, l1, &lcw1, &bch);
        int e2 = bch_repair(a, 465, cat(a, l2, "0"), &lcw2, &bch);
        int e2b = bch_repair(a, 465, cat(a, l2, "1"), &lcw2b, &bch);
        int e3 = bch_repair(a, 41, l3, &lcw3, &bch);
        if ((e2b >= 0 && e2b <= e2) || e2 < 0) { e2 = e2b; lcw2 = lcw2b; }
        m->lcw1 = lcw1; m->lcw2 = lcw2; m->lcw3 = lcw3;
        m->ft = (int)bits_to_int(lcw1);
        if (e1 < 0 || e2 < 0 || e3 < 0) {
            new_error(m, "LCW decode failed", NULL);
            size_t n = 256;
            char *h = ar_alloc(a, n);
            snprintf(h, n, "LCW(%s_%s/%01dE%d,%s_%sx/%03dE%d,%s_%s/%06dE%d)",
                     sub(a, l1, 0, 3), sub(a, l1, 3, strlen(l1)), (int)bits_to_int(lcw1), e1,
                     sub(a, l2, 0, 6), sub(a, l2, 6, strlen(l2)), (int)bits_to_int(lcw2), e2,
                     sub(a, l3, 0, 21), sub(a, l3, 21, strlen(l3)), (int)bits_to_int(lcw3), e3);
            m->header = h;
        }
        const char *rest = sub(a, data, lcwlen, dlen);
        m->descramble_extra = sub(a, rest, 312, strlen(rest));
        descr_add(m, sub(a, rest, 0, 312));
    }
    return 0;
}


/* ------------------------------------------------------------ LCW family */

static int bin_int(const char *s, size_t from, size_t to)   /* int(s[from:to], 2) */
{
    int v = 0;
    size_t n = strlen(s);
    for (size_t i = from; i < to && i < n; i++) v = (v << 1) | (s[i] == '1');
    return v;
}

/* util.group(string, n): a space after every n characters, then rstrip */
static void put_group(sb_t *b, const char *s, size_t n)
{
    size_t len = strlen(s);
    for (size_t x = 0; x < len; x += n) {
        if (x) sb_puts(b, " ");
        char tmp[64];
        size_t k = len - x < n ? len - x : n;
        memcpy(tmp, s + x, k); tmp[k] = 0;
        sb_puts(b, tmp);
    }
}

/* util.myhex(data, sep) */
static void put_hex(sb_t *b, const int *d, int n, const char *sep)
{
    for (int i = 0; i < n; i++) sb_printf(b, "%s%02x", i ? sep : "", d[i]);
}

/* util.to_ascii(data, dot=True) */
static void put_ascii_dot(sb_t *b, const int *d, int n)
{
    char tmp[2] = { 0, 0 };
    for (int i = 0; i < n; i++) { tmp[0] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.'; sb_puts(b, tmp); }
}

/* bitsparser.IridiumLCWMessage.pretty_lcw: the LCW(...) header */
static void pretty_lcw(msg_t *m)
{
    arena_t *a = m->a;
    const char *l3 = m->lcw3;
    m->lcw_ft = bin_int(m->lcw2, 0, 2);
    m->lcw_code = bin_int(m->lcw2, 2, strlen(m->lcw2));
    char code[200], bits3[64], ty[8];
    snprintf(bits3, sizeof(bits3), "%s", l3);
    code[0] = 0;
    int c = m->lcw_code;
    if (m->lcw_ft == 0) {
        strcpy(ty, "maint");
        if (c == 6) strcpy(code, "geoloc");
        else if (c == 15) strcpy(code, "<silent>");
        else if (c == 12) {
            snprintf(code, sizeof(code), "maint[1][lqi:%d,power:%d]", bin_int(l3, 19, 21), bin_int(l3, 16, 19));
            snprintf(bits3, sizeof(bits3), "%s", sub(a, l3, 0, 16));
        } else if (c == 0) {
            snprintf(code, sizeof(code), "sync[status:%d,dtoa:%d,dfoa:%d]", bin_int(l3, 1, 2), bin_int(l3, 3, 13), bin_int(l3, 13, 21));
            snprintf(bits3, sizeof(bits3), "%s|%s", sub(a, l3, 0, 1), sub(a, l3, 2, 3));
        } else if (c == 3) {
            snprintf(code, sizeof(code), "maint[2][lqi:%d,power:%d,f_dtoa:%d,f_dfoa:%d]", bin_int(l3, 1, 3), bin_int(l3, 3, 6), bin_int(l3, 6, 13), bin_int(l3, 13, 20));
            snprintf(bits3, sizeof(bits3), "%s|%s", sub(a, l3, 0, 1), sub(a, l3, 20, strlen(l3)));
        } else if (c == 1) {
            snprintf(code, sizeof(code), "switch[dtoa:%d,dfoa:%d]", bin_int(l3, 3, 13), bin_int(l3, 13, 21));
            snprintf(bits3, sizeof(bits3), "%s", sub(a, l3, 0, 3));
        } else snprintf(code, sizeof(code), "rsrvd(%d)", c);
    } else if (m->lcw_ft == 1) {
        strcpy(ty, "acchl");
        if (c == 1) {
            snprintf(code, sizeof(code), "acchl[msg_type:%01x,bloc_num:%01x,sapi_code:%01x,segm_list:%8s]",
                     bin_int(l3, 1, 4), bin_int(l3, 4, 5), bin_int(l3, 5, 8), sub(a, l3, 8, 16));
            snprintf(bits3, sizeof(bits3), "%s,%02x", sub(a, l3, 0, 1), bin_int(l3, 16, strlen(l3)));
        } else snprintf(code, sizeof(code), "rsrvd(%d)", c);
    } else if (m->lcw_ft == 2) {
        strcpy(ty, "hndof");
        if (c == 12) {
            strcpy(code, "handoff_cand");
            snprintf(bits3, sizeof(bits3), "%s,%s", sub(a, l3, 0, 11), sub(a, l3, 11, strlen(l3)));
        } else if (c == 3) {
            snprintf(code, sizeof(code), "handoff_resp[cand:%s,denied:%d,ref:%d,slot:%d,sband_up:%d,sband_dn:%d,access:%d]",
                     bin_int(l3, 2, 3) ? "S" : "P", bin_int(l3, 3, 4), bin_int(l3, 4, 5), 1 + bin_int(l3, 6, 8),
                     bin_int(l3, 8, 13), bin_int(l3, 13, 18), 1 + bin_int(l3, 18, 21));
            snprintf(bits3, sizeof(bits3), "%s,%s", sub(a, l3, 0, 2), sub(a, l3, 5, 6));
        } else if (c == 15) strcpy(code, "<silent>");
        else snprintf(code, sizeof(code), "rsrvd(%d)", c);
    } else {
        strcpy(ty, "rsrvd");
        snprintf(code, sizeof(code), "<%d>", c);
    }
    char h[400], hp[512];
    snprintf(h, sizeof(h), "LCW(%d,T:%s,C:%s,%s)", m->ft, ty, code, bits3);
    snprintf(hp, sizeof(hp), "%-110s ", h);
    m->header = ar_strdup(a, hp);
}

/* slice(data, 8) as ints; also bit-reversed */
static int bytes_of(const char *d, int *fwd, int *rev, int max)
{
    size_t n = strlen(d);
    int k = 0;
    for (size_t x = 0; x < n && k < max; x += 8) {
        size_t e = x + 8 < n ? x + 8 : n;
        int f = 0, r = 0;
        for (size_t i = x; i < e; i++) f = (f << 1) | (d[i] == '1');
        for (size_t i = e; i-- > x;) r = (r << 1) | (d[i] == '1');
        fwd[k] = f;
        if (rev) rev[k] = r;
        k++;
    }
    return k;
}

static int sixes_of(const char *d, size_t len, int *out, int max)   /* slice(d[:len], 6) */
{
    size_t n = strlen(d);
    if (len < n) n = len;
    int k = 0;
    for (size_t x = 0; x < n && k < max; x += 6) out[k++] = bin_int(d, x, x + 6 < n ? x + 6 : n);
    return k;
}

/* bitsparser.IridiumLCWMessage.__init__ */
static void lcw_init(msg_t *m)
{
    arena_t *a = m->a;
    m->cls = C_LCW;
    const char *data = descr_joined(m);
    pretty_lcw(m);
    size_t dlen = strlen(data);
    if (m->ft <= 3 && dlen < 312) new_error(m, "Not enough data in data packet", NULL);
    m->ndescr = 0;
    int empty_list = 0;
    if (m->ft == 0) {
        strcpy(m->msgtype, "VO");
        for (size_t x = 0; x < dlen; x += 8) descr_add(m, sub(a, data, x, x + 8));
        m->npay = bytes_of(data, m->payload_f, m->payload_r, 64);
        m->npay6 = sixes_of(data, 312, m->payload_6, 64);
    } else if (m->ft == 1) {
        strcpy(m->msgtype, "IP");
        for (size_t x = 0; x < dlen; x += 8) {
            char *byte = sub(a, data, x, x + 8);
            size_t l = strlen(byte);
            char *r = ar_alloc(a, l + 1);
            for (size_t i = 0; i < l; i++) r[i] = byte[l - 1 - i];
            r[l] = 0;
            descr_add(m, r);
        }
        m->npay = bytes_of(data, m->payload_f, m->payload_r, 64);
    } else if (m->ft == 2) {
        strcpy(m->msgtype, "DA");
        /* blocks = slice(data, 124); end = blocks.pop() */
        int nb = 0;
        const char *blk[8];
        for (size_t x = 0; x < dlen && nb < 8; x += 124) blk[nb++] = sub(a, data, x, x + 124);
        if (nb == 0) empty_list = 1;       /* pop() from an empty list: IndexError */
        else {
            for (int i = 0; i < nb - 1; i++) {
                char *b1, *b2;
                de_interleave(a, blk[i], &b1, &b2);
                char *j = cat(a, b1, b2);
                descr_add(m, sub(a, j, 93, 124));
                descr_add(m, sub(a, j, 31, 62));
                descr_add(m, sub(a, j, 62, 93));
                descr_add(m, sub(a, j, 0, 31));
            }
            char *b1, *b2;
            de_interleave(a, blk[nb - 1], &b1, &b2);
            descr_add(m, sub(a, b2, 1, strlen(b2)));
            descr_add(m, sub(a, b1, 1, strlen(b1)));
        }
    } else if (m->ft == 7) {
        strcpy(m->msgtype, "SY");
        descr_add(m, data);
    } else if (m->ft == 3) {
        strcpy(m->msgtype, "U3");
        descr_add(m, data);
        m->npayload6 = sixes_of(data, dlen, m->payload6, 64);
        m->npayload8 = bytes_of(data, m->payload8, NULL, 64);
    } else {
        snprintf(m->msgtype, sizeof(m->msgtype), "U%d", m->ft);
        descr_add(m, data);
    }
    (void)empty_list;
    if (strcmp(m->msgtype, "VO") != 0 && strcmp(m->msgtype, "IP") != 0 && *descr_joined(m) == 0 &&
        (m->ndescr <= 1))
        new_error(m, "No data to descramble", NULL);
}

/* bitsparser.IridiumSYMessage.upgrade */
static void sy_upgrade(msg_t *m)
{
    m->cls = C_SY;
    const char *d = descr_joined(m);
    size_t n = strlen(d);
    char *pat = ar_alloc(m->a, n + 2);
    for (size_t i = 0; i < n / 2; i++) { pat[2 * i] = '1'; pat[2 * i + 1] = '0'; }
    pat[(n / 2) * 2] = 0;
    m->has_fixederrs = 1;
    m->fixederrs = (1 + bitdiff(d, pat)) / 2;
    if (m->uplink) {
        for (size_t i = 0; i < n / 2; i++) { pat[2 * i] = '1'; pat[2 * i + 1] = '1'; }
        int fe = (1 + bitdiff(d, pat)) / 2;
        if (fe + 5 < m->fixederrs) { m->fixederrs = fe; strcpy(m->pattern, "11"); }
        else strcpy(m->pattern, "10");
    }
}

static void pretty_sy(msg_t *m, sb_t *b)
{
    sb_puts(b, "ISY: ");
    pretty_header_iridium(m, b);
    if (m->fixederrs == 0) sb_puts(b, " Sync=OK");
    else {
        sb_printf(b, " Sync=no errs=%d", m->fixederrs);
        size_t n = strlen(descr_joined(m));
        if (n < 312) sb_printf(b, " short:%zu ", n);
    }
    if (m->uplink) sb_printf(b, " pattern=%s", m->pattern);
    pretty_trailer_iridium(m, b);
}

static int ints_eq(const int *x, const int *y, int n) { for (int i = 0; i < n; i++) if (x[i] != y[i]) return 0; return 1; }

/* bitsparser.IridiumLCW3Message.__init__ */
static void lcw3_init(msg_t *m)
{
    m->cls = C_LCW3;
    strcpy(m->utype, "IU3");
    m->rs8p = m->rs6p = 0;
    m->has_fixederrs = 1;
    m->fixederrs = 0;
    int msg[64], cs[16];
    int ml = tk_rs8_fix(m->payload8, m->npayload8, msg, cs);
    if (ml >= 0) {
        strcpy(m->utype, "I38");
        int both[64]; memcpy(both, msg, sizeof(int) * (size_t)ml); memcpy(both + ml, cs, sizeof(int) * 8);
        if (m->npayload8 == ml + 8 && ints_eq(m->payload8, both, ml + 8)) m->rs8p = 1;
        else m->fixederrs += 1;
        memcpy(m->rs8m, msg, sizeof(int) * (size_t)ml); m->nrs8m = ml;
        m->csum = tk_checksum_16(m->rs8m);
    } else {
        ml = tk_rs6_fix(m->payload6, m->npayload6, msg, cs);
        if (ml >= 0) {
            strcpy(m->utype, "I36");
            int both[64]; memcpy(both, msg, sizeof(int) * (size_t)ml); memcpy(both + ml, cs, sizeof(int) * 10);
            if (m->npayload6 == ml + 10 && ints_eq(m->payload6, both, ml + 10)) m->rs6p = 1;
            else m->fixederrs += 1;
            memcpy(m->rs6m, msg, sizeof(int) * (size_t)ml); m->nrs6m = ml;
        }
    }
}

static int remove_zeros(const int *l, int n) { while (n > 0 && l[n - 1] == 0) n--; return n; }

/* bitsparser.IridiumLCW3Message.pretty */
static void pretty_lcw3(msg_t *m, sb_t *b)
{
    arena_t *a = m->a;
    sb_printf(b, "%s: ", m->utype);
    pretty_header_iridium(m, b);
    if (strcmp(m->utype, "I38") == 0) {
        sb_puts(b, m->rs8p ? " RS8=OK" : " RS8=ok");
        int n = m->nrs8m;
        if (m->csum == 0) { sb_puts(b, " CS=OK"); n = remove_zeros(m->rs8m, n >= 3 ? n - 3 : 0); }
        else sb_puts(b, " CS=no");
        sb_puts(b, " [");
        put_hex(b, m->rs8m, n, " ");
        sb_puts(b, "]");
    } else if (strcmp(m->utype, "I36") == 0) {
        sb_puts(b, m->rs6p ? " RS6=OK" : " RS6=ok");
        sb_printf(b, " {%02d}", m->rs6m[0]);
        sb_puts(b, " [");
        size_t vn = (size_t)(m->nrs6m - 1) * 6;
        char *v = ar_alloc(a, vn + 1);
        for (int i = 1; i < m->nrs6m; i++)
            for (int k = 0; k < 6; k++) v[(size_t)(i - 1) * 6 + (size_t)k] = (char)('0' + ((m->rs6m[i] >> (5 - k)) & 1));
        v[vn] = 0;
        int num[64], nnum = -1;
        int t = m->rs6m[0];
        if (t == 0) { sb_puts(b, sub(a, v, 0, 2)); sb_puts(b, "| "); put_group(b, sub(a, v, 2, vn), 10); }
        else if (t == 6) {
            sb_puts(b, sub(a, v, 0, 2)); sb_puts(b, "| "); put_group(b, sub(a, v, 2, vn), 24);
            nnum = 0;
            for (size_t x = 2; x < vn; x += 24) num[nnum++] = bin_int(v, x, x + 24 < vn ? x + 24 : vn);
            nnum = remove_zeros(num, nnum);
        } else if (t == 32 || t == 34) {
            sb_puts(b, sub(a, v, 0, 2)); sb_puts(b, "| "); put_group(b, sub(a, v, 2, vn), 24);
            nnum = 0;
            size_t e = vn >= 4 ? vn - 4 : 0;
            for (size_t x = 2; x < e; x += 24) num[nnum++] = bin_int(v, x, x + 24 < e ? x + 24 : e);
            int last = bin_int(v, e, vn);
            if (last != 0) num[nnum++] = last;
            while (nnum > 0 && num[nnum - 1] == 0x7ffff) nnum--;
        } else put_group(b, v, 6);
        sb_puts(b, "]");
        if (nnum >= 0) {
            sb_puts(b, " <");
            for (int i = 0; i < nnum; i++) sb_printf(b, "%s%06x", i ? " " : "", num[i]);
            sb_puts(b, ">");
        }
    } else {
        sb_puts(b, " RS=no [");
        put_group(b, descr_joined(m), 8);
        sb_puts(b, "]");
    }
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumIPMessage.__init__ */
static void ip_init(msg_t *m)
{
    m->cls = C_IP;
    unsigned char pr[64];
    for (int i = 0; i < m->npay; i++) pr[i] = (unsigned char)m->payload_r[i];
    if (tk_iip_crc24(pr, m->npay) == 0) {
        strcpy(m->itype, "IIP");
        m->ip_hdr = m->payload_r[0];
        m->ip_seq = m->payload_r[1];
        m->ip_ack = m->payload_r[2];
        m->ip_cs = m->payload_r[3];
        m->ip_cs_ok = m->ip_hdr + m->ip_seq + m->ip_ack + m->ip_cs;
        while (m->ip_cs_ok > 255) m->ip_cs_ok -= 255;
        m->nip_data = 0;
        for (int i = 4; i < 36 && i < m->npay; i++) m->ip_data[m->nip_data++] = m->payload_r[i];
        m->ip_cksum = 0;
        for (int i = 36; i < m->npay; i++) m->ip_cksum = (m->ip_cksum << 8) | (unsigned)m->payload_r[i];
    } else {
        int msg[64], rsc[16];
        int ml = tk_rs8_fix(m->payload_f, m->npay, msg, rsc);
        if (ml >= 0) {
            int both[64]; memcpy(both, msg, sizeof(int) * (size_t)ml); memcpy(both + ml, rsc, sizeof(int) * 8);
            if (!(m->npay == ml + 8 && ints_eq(m->payload_f, both, ml + 8))) { m->has_fixederrs = 1; m->fixederrs = 1; }
            unsigned iiq = tk_checksum_16(msg);
            if (iiq == 0) {
                strcpy(m->itype, "IIR");
                m->nidata = ml - 2;
                memcpy(m->idata, msg, sizeof(int) * (size_t)m->nidata);
            } else {
                strcpy(m->itype, "IIQ");
                int val = msg[0] | (msg[1] << 8);
                m->flags = val & 7;
                m->counter = val >> 3;
                m->nidata = ml - 2;
                memcpy(m->idata, msg + 2, sizeof(int) * (size_t)m->nidata);
            }
        } else strcpy(m->itype, "IIU");
    }
}

/* bitsparser.IridiumIPMessage.pretty */
static void pretty_ip(msg_t *m, sb_t *b)
{
    arena_t *a = m->a;
    sb_printf(b, "%s: ", m->itype);
    pretty_header_iridium(m, b);
    if (strcmp(m->itype, "IIP") == 0 || strcmp(m->itype, "VDA") == 0) {
        sb_printf(b, " type:%02x seq=%03d ack=%03d cs=%03d/%s ", m->ip_hdr, m->ip_seq, m->ip_ack, m->ip_cs,
                  m->ip_cs_ok == 255 ? "OK" : "no");
        const int *data = m->ip_data;
        int nd = m->nip_data;
        if (m->ip_hdr == 4) {
            int ip_len = m->ip_data[0];
            const int *ipd = m->ip_data + 1;
            int nipd = m->nip_data - 1;
            sb_printf(b, " len=%03d", ip_len);
            int allz = 1;
            for (int i = ip_len + 1; i < nipd; i++) if (ipd[i]) allz = 0;
            sb_t ms = { 0 };
            if (allz) { data = ipd; nd = ip_len < nipd ? ip_len : nipd; }
            else { data = ipd; nd = nipd; }
            sb_puts(&ms, " [");
            put_hex(&ms, data, nd, ".");
            sb_puts(&ms, "]");
            char *mstr = ms.s ? ms.s : ar_strdup(a, "");
            if (!allz && ip_len > 0 && ip_len < 31) {
                size_t L = strlen(mstr), cut = (size_t)(3 * ip_len - 1), from = (size_t)(3 * ip_len);
                sb_t t = { 0 };
                sb_puts(&t, sub(a, mstr, 0, cut < L ? cut : L));
                sb_puts(&t, "!");
                sb_puts(&t, sub(a, mstr, from < L ? from : L, L));
                free(ms.s);
                ms = t;
                mstr = ms.s;
            }
            sb_printf(b, "%-95s", mstr);
            free(ms.s);
        } else if (m->ip_hdr == 1) {
            sb_puts(b, "      ");
            while (nd > 0 && data[nd - 1] == 0) nd--;
            sb_t t = { 0 };
            put_hex(&t, data, nd, ".");
            sb_printf(b, "%-97s", t.s ? t.s : "");
            free(t.s);
        } else {
            sb_puts(b, "      [");
            put_hex(b, data, nd, ".");
            sb_puts(b, "]");
        }
        sb_printf(b, " FCS:OK/%06x", m->ip_cksum);
        if (nd > 0 && m->ip_hdr != 1) { sb_puts(b, " IP: "); put_ascii_dot(b, data, nd); }
    } else if (strcmp(m->itype, "IIQ") == 0) {
        sb_printf(b, " f:%d c:%04x", m->flags, m->counter);
        sb_puts(b, " ["); put_hex(b, m->idata, m->nidata, " "); sb_puts(b, "]");
        sb_puts(b, " IP: "); put_ascii_dot(b, m->idata, m->nidata);
    } else if (strcmp(m->itype, "IIR") == 0) {
        sb_puts(b, " ["); put_hex(b, m->idata, m->nidata, " "); sb_puts(b, "]");
    } else {
        sb_puts(b, " [");
        for (int i = 0; i < m->ndescr; i++) { if (i) sb_puts(b, " "); sb_puts(b, m->descrambled[i]); }
        sb_puts(b, "]");
    }
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumVOMessage.__init__ + upgrade */
static void vo_init(msg_t *m)
{
    m->cls = C_VO;
    unsigned char pr[64];
    for (int i = 0; i < m->npay; i++) pr[i] = (unsigned char)m->payload_r[i];
    if (tk_iip_crc24(pr, m->npay) == 0) {
        strcpy(m->vtype, "VDA");
        /* upgrade: IridiumIPMessage(self).upgrade(), itype = "VDA" */
        ip_init(m);
        strcpy(m->itype, "VDA");
        return;
    }
    int msg[64], cs[16];
    int ml = tk_rs6_fix(m->payload_6, m->npay6, msg, cs);
    m->rs6p = 0;
    if (ml >= 0) {
        strcpy(m->vtype, "VO6");
        int both[64]; memcpy(both, msg, sizeof(int) * (size_t)ml); memcpy(both + ml, cs, sizeof(int) * 10);
        if (m->npay6 == ml + 10 && ints_eq(m->payload_6, both, ml + 10)) m->rs6p = 1;
        memcpy(m->rs6m, msg, sizeof(int) * (size_t)ml); m->nrs6m = ml;
        return;
    }
    ml = tk_rs8_fix(m->payload_f, m->npay, msg, cs);
    if (ml >= 0) {
        strcpy(m->vtype, "VOD");
        memcpy(m->vdata, msg, sizeof(int) * (size_t)ml); m->nvdata = ml;
        return;
    }
    /* payload_f[-4:-1] all zero, sum % 256 == 0 -> VOZ */
    int n = m->npay, allz = 1;
    for (int i = n - 4; i < n - 1; i++) if (i >= 0 && m->payload_f[i] != 0) allz = 0;
    if (allz) {
        long vsum = 0;
        for (int i = 0; i < n; i++) vsum += m->payload_f[i];
        if (vsum % 0x100 == 0) {
            strcpy(m->vtype, "VOZ");
            int i;
            for (i = n - 2; i > 0; i--) if (m->payload_f[i] != 0) break;
            if (n - 2 < 0) i = 0;
            m->nvdata = i + 1;
            memcpy(m->vdata, m->payload_f, sizeof(int) * (size_t)m->nvdata);
            return;
        }
    }
    strcpy(m->vtype, "VOC");
    memcpy(m->vdata, m->payload_f, sizeof(int) * (size_t)n); m->nvdata = n;
}

/* bitsparser.IridiumVOMessage.pretty */
static void pretty_vo(msg_t *m, sb_t *b)
{
    sb_printf(b, "%s: ", m->vtype);
    pretty_header_iridium(m, b);
    if (strcmp(m->vtype, "VO6") == 0) {
        sb_puts(b, m->rs6p ? " RS=OK" : " RS=ok");
        size_t vn = (size_t)m->nrs6m * 6;
        char *v = ar_alloc(m->a, vn + 1);
        for (int i = 0; i < m->nrs6m; i++)
            for (int k = 0; k < 6; k++) v[(size_t)i * 6 + (size_t)k] = (char)('0' + ((m->rs6m[i] >> (5 - k)) & 1));
        v[vn] = 0;
        sb_puts(b, " ");
        put_group(b, v, 6);
    } else {
        sb_puts(b, " [");
        put_hex(b, m->vdata, m->nvdata, ".");
        sb_puts(b, "]");
    }
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumLCWMessage.upgrade (DA is the ECC path: not yet) */
static void lcw_upgrade(msg_t *m)
{
    if (m->error) return;
    if (strcmp(m->msgtype, "VO") == 0) vo_init(m);
    else if (strcmp(m->msgtype, "IP") == 0) ip_init(m);
    else if (strcmp(m->msgtype, "SY") == 0) sy_upgrade(m);
    else if (strcmp(m->msgtype, "U3") == 0) lcw3_init(m);
    else if (strcmp(m->msgtype, "DA") == 0) new_error(m, "not ported (DA)", "native");
    /* other U*: stays an LCW message */
}

/* bitsparser.Message.upgrade (default options) */
static void message_upgrade(msg_t *m)
{
    if (m->error) return;
    const char *bs = m->bitstream_raw;
    if (!m->next && startswith(bs, iridium_access)) { m->has_uplink = 1; m->uplink = 0; }
    else if (!m->next && startswith(bs, uplink_access)) { m->has_uplink = 1; m->uplink = 1; }
    else if (startswith(bs, next_access_dl)) { m->has_uplink = 1; m->uplink = 0; m->next = 1; }
    else if (startswith(bs, next_access_ul)) { m->has_uplink = 1; m->uplink = 1; m->next = 1; }
    else {
        /* (--uw-ec not ported) */
        new_error(m, "Access code missing", NULL);
        return;
    }
    perr_t pe;
    if (iridium_init(m, &pe)) {
        m->cls = C_MESSAGE;                  /* upgrade returns the Message */
        new_error(m, pe.msg, cls_name(pe.cls));
        return;
    }
    /* bitsparser.IridiumMessage.upgrade */
    if (m->error) return;
    if (strcmp(m->msgtype, "LW") == 0) {
        lcw_init(m);
        lcw_upgrade(m);
        return;
    }
    /* other families: not ported yet - say so, so the comparison counts it */
    char t[64];
    snprintf(t, sizeof(t), "not ported (msgtype %s)", m->msgtype);
    new_error(m, t, "native");
}

int bp_parse_line(const char *raw_line, char *out, size_t outsz)
{
    arena_t a = { 0 };
    msg_t *m = ar_alloc(&a, sizeof(*m));
    memset(m, 0, sizeof(*m));
    m->a = &a;
    m->cls = C_MESSAGE;
    sb_t b = { 0 };
    if (message_init(m, raw_line) != 0) {
        m->parse_error = 1;
        new_error(m, cat(&a, "Couldn't parse: ", raw_line), NULL);
    } else
        message_upgrade(m);

    switch (m->cls) {
    case C_MESSAGE: pretty_message(m, &b); break;
    case C_IRIDIUM:
    case C_LCW:     pretty_iridium(m, &b); break;
    case C_SY:      pretty_sy(m, &b); break;
    case C_LCW3:    pretty_lcw3(m, &b); break;
    case C_VO:      if (strcmp(m->vtype, "VDA") == 0) pretty_ip(m, &b); else pretty_vo(m, &b); break;
    case C_IP:      pretty_ip(m, &b); break;
    }

    if (m->error) {
        sb_puts(&b, " ERR:");
        for (int i = 0; i < m->nerr; i++) { if (i) sb_puts(&b, ", "); sb_puts(&b, m->error_msg[i]); }
    }
    int n = snprintf(out, outsz, "%s", b.s ? b.s : "");
    free(b.s);
    ar_free(&a);
    return n;
}
