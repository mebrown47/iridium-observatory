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
#include "itl_tables.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int count_ones(const char *s) { int n = 0; for (; *s; s++) n += *s == '1'; return n; }

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

/* iridium-parser.py --harder */
static int opt_harder;
void bp_set_harder(int on) { opt_harder = on; }

/* ------------------------------------------------------------- message */

typedef enum {
    C_MESSAGE, C_IRIDIUM, C_LCW, C_SY, C_LCW3, C_VO, C_IP,
    C_STL, C_AQ, C_NXT, C_ECC, C_LCWECC, C_DA, C_BC, C_RA, C_MS, C_MSBODY, C_MSASCII, C_MSBCD,
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
    case C_STL:     return "IridiumSTLMessage";
    case C_AQ:      return "IridiumAQMessage";
    case C_NXT:     return "IridiumNXTMessage";
    case C_ECC:     return "IridiumECCMessage";
    case C_LCWECC:  return "IridiumLCWECCMessage";
    case C_DA:      return "IridiumDAMessage";
    case C_BC:      return "IridiumBCMessage";
    case C_RA:      return "IridiumRAMessage";
    case C_MS:      return "IridiumMSMessage";
    case C_MSBODY:  return "IridiumMSMessageBody";
    case C_MSASCII: return "IridiumMessagingAscii";
    case C_MSBCD:   return "IridiumMessagingBCD";
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

    /* IridiumECCMessage / IridiumLCWECCMessage */
    int poly;
    const char *bitstream_bch;
    int fill, ecc_cut;
    const char *trailer;                      /* NULL = not set */
    /* IridiumDAMessage */
    int da_len, the_crc, crc_ok;
    int da_ta[64], nda_ta;
    /* IridiumBCMessage */
    int bc_blocks0;                           /* the first block was parsed */
    int sv_id, beam_id, slot, sv_blocking, acqu_subband, acqu_channels;
    const char *unknown01, *acqu_classes, *unknown02;
    int has_type, type;
    const char *unknown11, *unknown21, *unknown31, *type_data;
    int max_uplink_pwr;
    char iri_time_str[48], tmsi_expiry_str[48];
    struct { int type, empty, random_id, timeslot, ul_sb, dl_sb, access, dtoa, dfoa; const char *unknown4, *unknown; } asg[16];
    int nasg;
    /* IridiumRAMessage */
    int ra_sat, ra_cell, ra_pos_x, ra_pos_y, ra_pos_z, ra_int, ra_ts, ra_eip, ra_bc_sb;
    double ra_lat, ra_lon, ra_alt;
    char *page_str[16];
    int npaging;
    const char *ra_extra;                     /* NULL = not set */
    /* IridiumMSMessage and subclasses */
    int block, frame, bch_blocks, group_is_a, group, secondary, msg_trailer;
    const char *unknown1;
    const char *ablocks[4]; int nablocks;
    const char *bch_extra;
    const char *msblocks[64]; int nmsblocks;
    int msg_ric, msg_format, has_msg_seq, msg_seq;
    const char *pkt_cs1, *msg_data;           /* msg_data NULL = not set */
    int pkt_csum_ok, pkt_csum, msg_ctr, msg_ctr_max, msg_checksum;
    const char *msg_msgdata, *msg_rest;
    char *msg_ascii;
    const char *msg_unknown2;
    char *bcd;

    /* IridiumSTLMessage */
    int has_iq;                               /* "i" in __dict__ */
    const char *itl_i[8]; int nitl_i;
    const char *itl_q[8]; int nitl_q;
    int itl_version, plane;
    int msg_int[8]; const char *msg_str[8]; int nmsg;   /* ints, or a hex string */
    const char *sat, *mt;

    /* IridiumAQMessage */
    char aq_sym[128]; int rid, ridcrc, aq_crcval;
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
    if (strcmp(m->msgtype, "TL") == 0 && m->has_iq) {
        sb_puts(b, " <");
        for (int k = 0; k < m->nitl_i; k++) { if (k) sb_puts(b, " "); sb_puts(b, m->itl_i[k]); }
        sb_puts(b, "> <");
        for (int k = 0; k < m->nitl_q; k++) { if (k) sb_puts(b, " "); sb_puts(b, m->itl_q[k]); }
        sb_puts(b, ">");
    } else if (*d) {
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

    if (!m->msgtype[0] && opt_harder) {
        /* try IBC */
        if (dlen >= 70 && !(freqclass && m->uplink)) {
            int hdrlen = 6, blocklen = 64;
            char *bc1, *bc2, *d2, *b2, *d3, *b3;
            int e1 = bch_repair1(a, hdr_poly, sub(a, data, 0, (size_t)hdrlen), NULL, NULL);
            de_interleave(a, sub(a, data, (size_t)hdrlen, (size_t)(hdrlen + blocklen)), &bc1, &bc2);
            int e2 = bch_repair(a, ringalert_bch_poly, sub(a, bc1, 0, 31), &d2, &b2);
            int e3 = bch_repair(a, ringalert_bch_poly, sub(a, bc2, 0, 31), &d3, &b3);
            if (e1 >= 0 && e2 >= 0 && e3 >= 0 &&
                (count_ones(d2) + count_ones(b2) + count_ones(sub(a, bc1, 31, 32))) % 2 == 0 &&
                (count_ones(d3) + count_ones(b3) + count_ones(sub(a, bc2, 31, 32))) % 2 == 0) {
                strcpy(m->msgtype, "BC");
                m->has_ec_lcw = 1; m->ec_lcw = e1;
            }
        }
        /* try for LCW */
        if (!m->msgtype[0] && dlen >= 64) {
            char *l1, *l2, *l3, *x;
            de_interleave_lcw(a, sub(a, data, 0, 46), &l1, &l2, &l3);
            int e1 = bch_repair(a, 29, l1, &x, NULL);
            int e2a = bch_repair(a, 465, cat(a, l2, "0"), &x, NULL);
            int e2b = bch_repair(a, 465, cat(a, l2, "1"), &x, NULL);
            int e3 = bch_repair(a, 41, l3, &x, NULL);
            int e2 = e2a;
            if ((e2b >= 0 && e2b < e2a) || e2a < 0) e2 = e2b;
            if (e1 >= 0 && e2 >= 0 && e3 >= 0) {
                strcpy(m->msgtype, "LW");
                m->has_ec_lcw = 1; m->ec_lcw = e1 + e2 + e3;
            }
        }
        /* try for IRA */
        size_t firstlen = 3 * 32;
        if (!m->msgtype[0] && dlen >= firstlen && !(freqclass && m->uplink)) {
            char *r1, *r2, *r3, *d1, *b1, *d2, *b2, *d3, *b3;
            de_interleave3(a, sub(a, data, 0, firstlen), &r1, &r2, &r3);
            int e1 = bch_repair(a, ringalert_bch_poly, sub(a, r1, 0, 31), &d1, &b1);
            int e2 = bch_repair(a, ringalert_bch_poly, sub(a, r2, 0, 31), &d2, &b2);
            int e3 = bch_repair(a, ringalert_bch_poly, sub(a, r3, 0, 31), &d3, &b3);
            if (e1 >= 0 && e2 >= 0 && e3 >= 0 &&
                (count_ones(d1) + count_ones(b1) + count_ones(sub(a, r1, 31, 32))) % 2 == 0 &&
                (count_ones(d2) + count_ones(b2) + count_ones(sub(a, r2, 31, 32))) % 2 == 0 &&
                (count_ones(d3) + count_ones(b3) + count_ones(sub(a, r3, 31, 32))) % 2 == 0)
                strcpy(m->msgtype, "RA");
        }
        /* try ITL */
        if (!m->msgtype[0] && dlen >= 96 + (8 * 8 * 12) && !(freqclass && m->uplink)) {
            static const char *tl = NULL;
            if (!tl) {
                static char h[97];
                h[0] = h[1] = '1';
                for (int i = 2; i < 96; i++) h[i] = '0';
                h[96] = 0;
                tl = h;
            }
            if (bitdiff(sub(a, data, 0, 96), tl) < 4) { m->has_ec_lcw = 1; m->ec_lcw = 1; strcpy(m->msgtype, "TL"); }
        }
        /* try IMS */
        if (!m->msgtype[0] && dlen >= 32 && !(freqclass && m->uplink))
            if (bitdiff(sub(a, data, 0, 32), header_messaging) < 2) { m->has_ec_lcw = 1; m->ec_lcw = 1; strcpy(m->msgtype, "MS"); }
    }

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


/* ------------------------------------------------------------ ECC family */

static const char *errs_ch(int e) { return e == 0 ? "0" : e == 1 ? "1" : e == 2 ? "2" : "-"; }

static int count1(const char *s) { int n = 0; for (; *s; s++) n += *s == '1'; return n; }

/* crcmod.predefined "crc-ccitt-false" */
static unsigned crc_ccitt_false(const unsigned char *d, int n)
{
    unsigned crc = 0xffff;
    for (int i = 0; i < n; i++) {
        crc ^= (unsigned)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
    return crc;
}

/* util.fmt_iritime -> the string */
static void fmt_iritime(unsigned long iritime, char *out, size_t n)
{
    double uxtime = (double)iritime * 90 / 1000 + 1739556857;
    /* datetime.fromtimestamp: seconds, with microseconds rounded half-even */
    double t = floor(uxtime), frac = uxtime - t;
    double us = nearbyint(frac * 1e6);        /* default rounding mode: half-even */
    if (us >= 1000000) t += 1;
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    char hund[16];
    snprintf(hund, sizeof(hund), "%02.0f", fmod(uxtime, 1.0) * 100);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%sZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, hund);
}

/* bitsparser.IridiumECCMessage.__init__; returns 1 with *pe for ParserError */
static int ecc_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_ECC;
    if (strcmp(m->msgtype, "MS") == 0) m->poly = messaging_bch_poly;
    else m->poly = ringalert_bch_poly;        /* RA, BC */
    m->bitstream_bch = "";
    m->has_fixederrs = 1; m->fixederrs = 0;
    m->fill = 0; m->ecc_cut = 0;
    if (m->ndescr == 0) { pe->msg = "No data to ECC"; pe->cls = C_ECC; return 1; }
    if (strcmp(m->msgtype, "MS") == 0 || strcmp(m->msgtype, "RA") == 0) {
        while (m->ndescr >= 2) {
            const char *first = m->descrambled[m->ndescr - 2], *second = m->descrambled[m->ndescr - 1];
            if (strcmp(cat(a, first, second), "1010001001110011101111110110110101010100010001011100001011100110") == 0 ||
                (bitdiff(first, "10100010011100111011111101101101") <= 2 &&
                 bitdiff(second, "01010100010001011100001011100110") <= 2)) {
                m->fill++;
                m->ndescr -= 2;
            } else break;
        }
        if (m->fill > 0) m->descramble_extra = "";
    }
    sb_t bch = { 0 };
    for (int i = 0; i < m->ndescr; i++) {
        const char *blk = m->descrambled[i];
        const char *parity = sub(a, blk, 31, strlen(blk));
        char *data, *bb;
        int errs = bch_repair(a, m->poly, sub(a, blk, 0, 31), &data, &bb);
        if (errs < 0) { m->ecc_cut = 1; m->fill = 0; m->descramble_extra = ""; break; }
        if ((count1(data) + count1(bb) + count1(parity)) % 2 == 1) {
            if (opt_harder) errs += 1;
            else if (errs > 0) {
                new_error(m, "Parity error", NULL);
                m->ecc_cut = 1; m->fill = 0; m->descramble_extra = "";
                break;
            }
        }
        if (errs > 0) m->fixederrs++;
        sb_puts(&bch, data);
    }
    m->bitstream_bch = ar_strdup(a, bch.s ? bch.s : "");
    free(bch.s);
    if (!*m->bitstream_bch) new_error(m, "BCH decode failed", NULL);
    return 0;
}

/* bitsparser.IridiumECCMessage.pretty (IME) */
static void pretty_ecc(msg_t *m, sb_t *b)
{
    arena_t *a = m->a;
    sb_puts(b, "IME: ");
    pretty_header_iridium(m, b);
    sb_printf(b, " %s ", m->msgtype);
    for (int i = 0; i < m->ndescr; i++) {
        const char *bk = m->descrambled[i];
        const char *b31 = sub(a, bk, 0, 31);
        char *foo;
        int errs = nrepair(a, m->poly, b31, &foo);
        uint64_t res = ndivide((uint64_t)m->poly, b31);
        const char *p31 = sub(a, bk, 31, 32);
        int parity = (count1(foo) + count1(p31)) % 2;
        sb_printf(b, "{%s %s %s/%04d E%s P%d}", sub(a, bk, 0, 21), sub(a, bk, 21, 31), p31, (int)res, errs_ch(errs), parity);
    }
    if (m->fill > 0) sb_printf(b, " FILL=%02d", m->fill);
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumLCWECCMessage.__init__ */
static void lcwecc_init(msg_t *m)
{
    arena_t *a = m->a;
    m->cls = C_LCWECC;
    m->poly = acch_bch_poly;
    m->has_fixederrs = 1; m->fixederrs = 0;
    sb_t bch = { 0 };
    for (int i = 0; i < m->ndescr; i++) {
        char *data, *bb;
        int errs = bch_repair(a, m->poly, m->descrambled[i], &data, &bb);
        if (errs < 0) { m->descramble_extra = ""; break; }
        if (errs > 0) m->fixederrs++;
        sb_puts(&bch, data);
    }
    m->bitstream_bch = ar_strdup(a, bch.s ? bch.s : "");
    free(bch.s);
    if (!*m->bitstream_bch) new_error(m, "BCH decode failed", NULL);
}

/* bitsparser.IridiumLCWECCMessage.pretty (IME for DA) */
static void pretty_lcwecc(msg_t *m, sb_t *b)
{
    arena_t *a = m->a;
    sb_puts(b, "IME: ");
    pretty_header_iridium(m, b);
    sb_printf(b, " %s ", m->msgtype);
    for (int i = 0; i < m->ndescr; i++) {
        const char *bk = m->descrambled[i];
        char *foo;
        int errs = nrepair(a, m->poly, bk, &foo);
        uint64_t res = ndivide((uint64_t)m->poly, bk);
        int parity = count1(foo) % 2;
        sb_printf(b, "{%s %s/%04d E%s P%d}", sub(a, bk, 0, 21), sub(a, bk, 21, 31), (int)res, errs_ch(errs), parity);
    }
    pretty_trailer_iridium(m, b);
}

/* ints of slice(bits, 8) (a short last piece counts too) */
static int bytes_list(const char *d, int *out, int max) { return bytes_of(d, out, NULL, max); }

/* bitsparser.IridiumDAMessage.__init__; returns 1 with *pe for ParserError */
static int da_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_DA;
    const char *bs = m->bitstream_bch;
    size_t n = strlen(bs);
    m->da_len = bin_int(bs, 11, 16);
    if (bin_int(bs, 17, 20) != 0) new_error(m, "zero1 not 0", NULL);
    if (n < 9 * 20 + 16) { pe->msg = "Not enough data in data packet"; pe->cls = C_DA; return 1; }
    if (m->da_len > 0) {
        m->nda_ta = bytes_list(sub(a, bs, 20, 9 * 20), m->da_ta, 64);
        const char *crcstream = cat(a, cat(a, sub(a, bs, 0, 20), "000000000000"), sub(a, bs, 20, n >= 4 ? n - 4 : 0));
        int by[64];
        int nb = bytes_list(crcstream, by, 64);
        unsigned char ub[64];
        for (int i = 0; i < nb; i++) ub[i] = (unsigned char)by[i];
        m->the_crc = (int)crc_ccitt_false(ub, nb);
        m->crc_ok = m->the_crc == 0;
    } else {
        m->crc_ok = 0;
        m->nda_ta = bytes_list(sub(a, bs, 20, 11 * 20), m->da_ta, 64);
    }
    if (bin_int(bs, 9 * 20 + 16, n) != 0) new_error(m, "zero2 not 0", NULL);
    return 0;
}

/* bitsparser.IridiumDAMessage.pretty */
static void pretty_da(msg_t *m, sb_t *b)
{
    arena_t *a = m->a;
    const char *bs = m->bitstream_bch;
    size_t n = strlen(bs);
    sb_puts(b, "IDA: ");
    pretty_header_iridium(m, b);
    sb_printf(b, " %s cont=%s %s ctr=%s %s len=%02d 0:%s [", sub(a, bs, 0, 3), sub(a, bs, 3, 4), sub(a, bs, 4, 5),
              sub(a, bs, 5, 8), sub(a, bs, 8, 11), m->da_len, sub(a, bs, 16, 20));
    sb_t ms = { 0 };
    if (m->da_len > 0) {
        int allz = 1;
        for (int i = m->da_len + 1; i < m->nda_ta; i++) if (m->da_ta[i]) allz = 0;
        if (allz) put_hex(&ms, m->da_ta, m->da_len < m->nda_ta ? m->da_len : m->nda_ta, ".");
        else {
            put_hex(&ms, m->da_ta, m->nda_ta, ".");
            if (m->da_len > 0 && m->da_len < 20) {
                const char *s0 = ms.s ? ms.s : "";
                size_t L = strlen(s0), cut = (size_t)(3 * m->da_len - 1), from = (size_t)(3 * m->da_len);
                sb_t t = { 0 };
                sb_puts(&t, sub(a, s0, 0, cut < L ? cut : L));
                sb_puts(&t, "!");
                sb_puts(&t, sub(a, s0, from < L ? from : L, L));
                free(ms.s);
                ms = t;
            }
        }
    } else put_hex(&ms, m->da_ta, m->nda_ta, ".");
    sb_puts(&ms, "]");
    sb_printf(b, "%-60s", ms.s);
    free(ms.s);
    if (m->da_len > 0) {
        sb_printf(b, " %04x/%04x", bin_int(bs, 9 * 20, 9 * 20 + 16), m->the_crc);
        sb_puts(b, m->crc_ok ? " CRC:OK" : " CRC:no");
        sb_printf(b, " %s", sub(a, bs, 9 * 20 + 16, n));
        sb_puts(b, " SBD: ");
        int sbd[64];
        int k = bytes_list(sub(a, bs, 20, 9 * 20), sbd, 64);
        put_ascii_dot(b, sbd, k);
    } else {
        sb_puts(b, "  ---   ");
        sb_printf(b, " %s", sub(a, bs, 9 * 20 + 16, n));
    }
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumBCMessage.__init__ */
static void bc_init(msg_t *m)
{
    arena_t *a = m->a;
    m->cls = C_BC;
    char **blocks, *extra;
    int nb = slice_extra(a, m->bitstream_bch, 42, &blocks, &extra);
    if (nb > 4) { nb = 4; m->trailer = "{LONG}"; }
    else if (nb < 4) m->trailer = "{SHORT}";
    m->descramble_extra = "";
    int k = 0;
    if (k < nb && m->bc_type == 0) {
        const char *d = blocks[k++];
        m->bc_blocks0 = 1;
        m->sv_id = bin_int(d, 0, 7); m->beam_id = bin_int(d, 7, 13);
        m->unknown01 = sub(a, d, 13, 14);
        m->slot = bin_int(d, 14, 15); m->sv_blocking = bin_int(d, 15, 16);
        m->acqu_classes = sub(a, d, 16, 32);
        m->acqu_subband = bin_int(d, 32, 37); m->acqu_channels = bin_int(d, 37, 40);
        m->unknown02 = sub(a, d, 40, 42);
    }
    if (k < nb && m->bc_type == 0) {
        const char *d = blocks[k++];
        m->has_type = 1;
        m->type = bin_int(d, 0, 6);
        if (m->type == 0) { m->unknown11 = sub(a, d, 6, 36); m->max_uplink_pwr = bin_int(d, 36, 42); }
        else if (m->type == 1) { m->unknown21 = sub(a, d, 6, 10); fmt_iritime(bits_to_int(sub(a, d, 10, 42)), m->iri_time_str, sizeof(m->iri_time_str)); }
        else if (m->type == 2) { m->unknown31 = sub(a, d, 6, 10); fmt_iritime(bits_to_int(sub(a, d, 10, 42)), m->tmsi_expiry_str, sizeof(m->tmsi_expiry_str)); }
        else m->type_data = d;
    }
    m->nasg = 0;
    for (; k < nb && m->nasg < 16; k++) {
        const char *d = blocks[k];
        typeof(m->asg[0]) *as = &m->asg[m->nasg++];
        memset(as, 0, sizeof(*as));
        as->type = bin_int(d, 0, 3);
        if (as->type == 0) {
            as->random_id = bin_int(d, 3, 11); as->timeslot = 1 + bin_int(d, 11, 13);
            as->ul_sb = bin_int(d, 13, 18); as->dl_sb = bin_int(d, 18, 23);
            as->access = 1 + bin_int(d, 23, 26); as->dtoa = bin_int(d, 26, 34);
            as->dfoa = bin_int(d, 34, 40); as->unknown4 = sub(a, d, 40, 42);
            if (as->dtoa > 128) as->dtoa -= 256;
        } else if (strcmp(d, "111000000000000000000000000000000000000000") == 0) as->empty = 1;
        else as->unknown = sub(a, d, 3, 42);
    }
}

/* bitsparser.IridiumBCMessage.pretty */
static void pretty_bc(msg_t *m, sb_t *b)
{
    sb_t s = { 0 };
    sb_puts(&s, "IBC: ");
    pretty_header_iridium(m, &s);
    sb_printf(&s, " bc:%d", m->bc_type);
    if (m->bc_type == 0) {
        sb_printf(&s, " sat:%03d cell:%02d %s slot:%d sv_blkn:%d aq_cl:%s aq_sb:%02d aq_ch:%d %s", m->sv_id, m->beam_id,
                  m->unknown01, m->slot, m->sv_blocking, m->acqu_classes, m->acqu_subband, m->acqu_channels, m->unknown02);
        if (m->has_type) {
            if (m->type == 0) sb_printf(&s, " %s max_uplink_pwr:%02d", m->unknown11, m->max_uplink_pwr);
            else if (m->type == 1) sb_printf(&s, " %s time:%s", m->unknown21, m->iri_time_str);
            else if (m->type == 2) sb_printf(&s, " %s tmsi_expiry:%s", m->unknown31, m->tmsi_expiry_str);
            else if (m->type == 4) {
                sb_printf(&s, " st:%02d ", m->type);
                sb_puts(&s, strcmp(m->type_data, "000100000000100001110000110000110011110000") == 0 ? "DFLT" : m->type_data);
            } else { sb_printf(&s, " st:%02d ", m->type); sb_puts(&s, m->type_data); }
        }
    }
    sb_printf(b, "%-214s", s.s);
    free(s.s);
    for (int i = 0; i < m->nasg; i++) {
        typeof(m->asg[0]) *as = &m->asg[i];
        if (as->empty) sb_puts(b, " []");
        else if (as->type == 0)
            sb_printf(b, " [%d Rid:%03d ts:%d ul_sb:%02d dl_sb:%02d access:%d dtoa:%+04d dfoa:%02d %s]", as->type, as->random_id,
                      as->timeslot, as->ul_sb, as->dl_sb, as->access, as->dtoa, as->dfoa, as->unknown4);
        else sb_printf(b, " [%d %s]                     ", as->type, as->unknown);
    }
    if (m->trailer) { sb_puts(b, " "); sb_puts(b, m->trailer); }
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumRAMessage.__init__; returns 1 with *pe for ParserError */
static int ra_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_RA;
    const char *bs = m->bitstream_bch;
    if (strlen(bs) < 63) { pe->msg = "RA content too short"; pe->cls = C_RA; return 1; }
    m->ra_sat = bin_int(bs, 0, 7);
    m->ra_cell = bin_int(bs, 7, 13);
    m->ra_pos_x = bin_int(bs, 14, 25) - bin_int(bs, 13, 14) * (1 << 11);
    m->ra_pos_y = bin_int(bs, 26, 37) - bin_int(bs, 25, 26) * (1 << 11);
    m->ra_pos_z = bin_int(bs, 38, 49) - bin_int(bs, 37, 38) * (1 << 11);
    m->ra_int = bin_int(bs, 49, 56);
    m->ra_ts = bin_int(bs, 56, 57);
    m->ra_eip = bin_int(bs, 57, 58);
    m->ra_bc_sb = bin_int(bs, 58, 63);
    double x = m->ra_pos_x, y = m->ra_pos_y, z = m->ra_pos_z;
    m->ra_lat = atan2(z, sqrt(x * x + y * y)) * 180 / M_PI;
    m->ra_lon = atan2(y, x) * 180 / M_PI;
    m->ra_alt = sqrt(x * x + y * y + z * z) * 4;
    const char *ra_msg = sub(a, bs, 63, strlen(bs));
    size_t rl = strlen(ra_msg);
    m->npaging = 0;
    int page_cnt = (int)(rl / 42);
    if (page_cnt == 0) { m->trailer = "{TRUNCATED}"; return 0; }
    if (rl % 42 > 0) page_cnt++;
    char **blocks, *extra;
    int nb = slice_extra(a, ra_msg, 42, &blocks, &extra);
    int ended = 0;
    for (int i = 0; i < nb && m->npaging < 16; i++) {
        const char *pg = blocks[i];
        unsigned long tmsi = (unsigned long)bits_to_int(sub(a, pg, 0, 32));
        int zero1 = bin_int(pg, 32, 34), msc_id = bin_int(pg, 34, 39), zero2 = bin_int(pg, 39, 42);
        char str[96];
        int k = snprintf(str, sizeof(str), "tmsi:%08lx", tmsi);
        if (zero1) k += snprintf(str + k, sizeof(str) - (size_t)k, " 0:%d", zero1);
        k += snprintf(str + k, sizeof(str) - (size_t)k, " msc_id:%02d", msc_id);
        if (zero2) snprintf(str + k, sizeof(str) - (size_t)k, " 0:%d", zero2);
        int all1 = strlen(pg) == 42;
        for (const char *c = pg; *c; c++) if (*c != '1') all1 = 0;
        m->page_str[m->npaging++] = ar_strdup(a, all1 ? "END" : str);
        if (all1) { ended = 1; break; }
    }
    if (m->npaging > 0 && ended) {
        if (m->npaging < page_cnt) {
            const char *ex = sub(a, ra_msg, (size_t)(42 * m->npaging), rl);
            if (startswith(ex, "101000100111001110111")) m->trailer = "{OK:UNCLEAN}";
            else { m->ra_extra = ex; m->trailer = "{EXTRA_BITS}"; }
        } else {
            if (m->descramble_extra && startswith(m->descramble_extra, "011010110")) m->descramble_extra = "";
            m->trailer = "{OK}";
        }
        m->npaging--;
    } else if (m->npaging < 12) m->trailer = "{TRUNCATED}";
    else m->trailer = "{OK}";
    return 0;
}

/* bitsparser.IridiumRAMessage.pretty */
static void pretty_ra(msg_t *m, sb_t *b)
{
    sb_puts(b, "IRA: ");
    pretty_header_iridium(m, b);
    sb_printf(b, " sat:%03d beam:%02d xyz=(%+05d,%+05d,%+05d) pos=(%+06.2f/%+07.2f) alt=%03ld RAI:%02d ?%d%d bc_sb:%02d",
              m->ra_sat, m->ra_cell, m->ra_pos_x, m->ra_pos_y, m->ra_pos_z, m->ra_lat, m->ra_lon,
              (long)(m->ra_alt - 6378 + 23), m->ra_int, m->ra_ts, m->ra_eip, m->ra_bc_sb);
    sb_printf(b, " P%02d:", m->npaging);
    for (int i = 0; i < m->npaging; i++) sb_printf(b, " PAGE(%s)", m->page_str[i]);
    if (m->trailer) { sb_puts(b, " "); sb_puts(b, m->trailer); }
    if (m->fill > 0) sb_printf(b, " FILL=%d", m->fill);
    if (m->ra_extra) { sb_puts(b, " +"); put_sliced(b, m->ra_extra, 42); }
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumMSMessage.__init__; returns 1 with *pe for ParserError */
static int ms_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_MS;
    const char *bs = m->bitstream_bch;
    size_t n = strlen(bs);
    const char *blocks[64];
    int nb = 0;
    for (size_t x = 0; x < n && nb < 64; x += 21) blocks[nb++] = sub(a, bs, x, x + 21);
    const char *b0 = blocks[0];
    int ms_type = bin_int(b0, 0, 1);
    const char *zero1 = sub(a, b0, 1, 5);
    m->block = bin_int(b0, 5, 9);
    m->frame = bin_int(b0, 9, 15);
    m->bch_blocks = bin_int(b0, 15, 19);
    if (ms_type == 1) {
        m->group_is_a = 1;
        m->unknown1 = sub(a, b0, 19, 20);
        m->secondary = bin_int(b0, 20, 21);
    } else m->group = bin_int(b0, 19, 21);
    if (strcmp(zero1, "0000") != 0) new_error(m, "zero1 not 0000", NULL);
    if (m->bch_blocks < 2) { pe->msg = "length field in header too small"; pe->cls = C_MS; return 1; }
    m->bch_extra = "";
    if (m->bch_blocks * 2 > nb) {
        new_error(m, "Not enough data received", NULL);
        char t[64];
        snprintf(t, sizeof(t), "Need %d, got %d", m->bch_blocks * 2, nb);
        new_error(m, ar_strdup(a, t), NULL);
    } else if (m->bch_blocks * 2 < nb) {
        m->trailer = "{EXTRA}";
        m->bch_extra = sub(a, bs, (size_t)(m->bch_blocks * 42), n);
        nb = 2 * m->bch_blocks;
    }
    /* blocks.pop(0) */
    int k = 1;
    if (m->group_is_a) {
        if (nb - k < 2) { pe->msg = "not enough data in acquisition group message"; pe->cls = C_MS; return 1; }
        m->nablocks = nb - k >= 4 ? 4 : 2;
        for (int i = 0; i < m->nablocks; i++) m->ablocks[i] = blocks[k++];
    }
    m->msg_trailer = 0;
    const char *all1 = "111111111111111111111";
    if (nb > k && blocks[nb - 1][0] == '1') {
        const char *t = blocks[--nb];
        if (strcmp(t, all1) != 0) new_error(m, "trailer exists, but not all-1", NULL);
        m->msg_trailer++;
        if (nb > k && blocks[nb - 1][0] == '1') {
            t = blocks[--nb];
            if (strcmp(t, all1) != 0) new_error(m, "second trailer exists, but not all-1", NULL);
            m->msg_trailer++;
        }
    }
    m->nmsblocks = 0;
    for (int i = k; i < nb; i++) m->msblocks[m->nmsblocks++] = blocks[i];
    return 0;
}

/* bitsparser.IridiumMSMessage._pretty_header */
static void pretty_header_ms(msg_t *m, sb_t *b)
{
    pretty_header_iridium(m, b);
    if (m->group_is_a) sb_printf(b, " %1d:A:%02d", m->block, m->frame);
    else sb_printf(b, " %1d:%d:%02d", m->block, m->group, m->frame);
    sb_printf(b, " len:%02d/T%d/F%02d", m->bch_blocks, m->msg_trailer, m->fill);
    if (m->group_is_a) {
        sb_t j = { 0 };
        for (int i = 0; i < m->nablocks; i++) { if (i) sb_puts(&j, " "); sb_puts(&j, m->ablocks[i]); }
        sb_printf(b, " %s %d %-87s", m->unknown1, m->secondary, j.s ? j.s : "");
        free(j.s);
    } else sb_printf(b, " %s %s %-87s", " ", " ", " ");
}

static void pretty_trailer_ms(msg_t *m, sb_t *b)
{
    pretty_trailer_iridium(m, b);
    if (m->bch_extra && *m->bch_extra) { sb_puts(b, " bch_extra:"); sb_puts(b, m->bch_extra); }
}

static void pretty_ms(msg_t *m, sb_t *b)
{
    sb_puts(b, "IMS: ");
    pretty_header_ms(m, b);
    pretty_trailer_ms(m, b);
}

/* bitsparser.IridiumMSMessageBody.__init__; returns 1 with *pe for ParserError */
static int msbody_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_MSBODY;
    sb_t r = { 0 };
    for (int i = 0; i < m->nmsblocks; i++) sb_puts(&r, m->msblocks[i] + (m->msblocks[i][0] ? 1 : 0));
    const char *rest = ar_strdup(a, r.s ? r.s : "");
    free(r.s);
    size_t n = strlen(rest);
    if (n <= 27) { pe->msg = "message too short(body)"; pe->cls = C_MSBODY; return 1; }
    int ric = 0;
    for (int i = 21; i >= 0; i--) ric = (ric << 1) | (rest[i] == '1');     /* int(rest[0:22][::-1], 2) */
    m->msg_ric = ric;
    m->msg_format = bin_int(rest, 22, 27);
    rest = rest + 27; n -= 27;
    if (n <= 16) { new_error(m, "incomplete MSG body", NULL); return 0; }
    m->has_msg_seq = 1;
    m->msg_seq = bin_int(rest, 0, 6);
    if (bin_int(rest, 6, 10) != 0) new_error(m, "zero1 is not all-zero", NULL);
    m->pkt_cs1 = sub(a, rest, 10, 16);
    m->msg_data = sub(a, rest, 16, n);
    return 0;
}

static void pretty_header_msbody(msg_t *m, sb_t *b)
{
    pretty_header_ms(m, b);
    sb_printf(b, " ric:%07d fmt:%02d", m->msg_ric, m->msg_format);
    if (m->has_msg_seq) sb_printf(b, " seq:%02d", m->msg_seq);
}

static void pretty_msbody(msg_t *m, sb_t *b)
{
    sb_puts(b, "MSG: ");
    pretty_header_msbody(m, b);
    if (m->msg_data) { sb_puts(b, " "); put_group(b, m->msg_data, 20); }
    pretty_trailer_ms(m, b);
}

/* bitsparser.msg_checksum(blocks) */
static void msg_checksum(msg_t *m, const char **blocks, int n, int *ok, int *val)
{
    arena_t *a = m->a;
    const char *c0 = blocks[0], *c1 = n > 1 ? blocks[1] : "";
    size_t l0 = strlen(c0);
    const char *s = cat(a, sub(a, c0, l0 >= 3 ? l0 - 3 : 0, l0), sub(a, c1, 1, 8));
    int v = 0;
    for (size_t i = strlen(s); i-- > 0;) v = (v << 1) | (s[i] == '1');
    long csum = 0;
    for (int idx = 0; idx < n; idx++) {
        const char *c = blocks[idx];
        if (idx != 1) csum += bin_int(c, 0, 8);
        csum += bin_int(c, 8, 16);
        if (idx != 0) csum += bin_int(c, 16, strlen(c));
    }
    *ok = (v + csum) % 1024 == 1023;
    *val = v;
}

/* bitsparser.IridiumMessagingAscii.__init__; returns 1 with *pe for ParserError */
static int msascii_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_MSASCII;
    const char *rest = m->msg_data;
    if (m->nmsblocks >= 2) msg_checksum(m, m->msblocks + 1, m->nmsblocks - 1, &m->pkt_csum_ok, &m->pkt_csum);
    const char *len_bit = sub(a, rest, 4, 5);
    rest = sub(a, rest, 5, strlen(rest));
    if (strcmp(len_bit, "1") == 0) {
        int lfl = bin_int(rest, 0, 4);
        if (lfl == 0) { pe->msg = "len_field_len unexpectedly 0"; pe->cls = C_MSASCII; return 1; }
        m->msg_ctr = bin_int(rest, 4, (size_t)(4 + lfl));
        if (strlen(sub(a, rest, (size_t)(4 + lfl), (size_t)(4 + lfl * 2))) == 0) { pe->msg = "message too short(lfl)"; pe->cls = C_MSASCII; return 1; }
        m->msg_ctr_max = bin_int(rest, (size_t)(4 + lfl), (size_t)(4 + lfl * 2));
        rest = sub(a, rest, (size_t)(4 + lfl * 2), strlen(rest));
        if (lfl < 1 || lfl > 2) new_error(m, "len_field_len not 1 or 2", NULL);
    } else { m->msg_ctr = 0; m->msg_ctr_max = 0; }
    if (strlen(rest) < 8) { pe->msg = "message too short(ascii)"; pe->cls = C_MSASCII; return 1; }
    if (rest[0] != '0') new_error(m, "zero2 is not zero", NULL);
    m->msg_checksum = bin_int(rest, 1, 8);
    m->msg_msgdata = sub(a, rest, 8, strlen(rest));
    char **chars, *mrest;
    int nc = slice_extra(a, m->msg_msgdata, 7, &chars, &mrest);
    m->msg_rest = mrest;
    sb_t asc = { 0 };
    int end = 0, etx_errs = 0;
    for (int i = 0; i < nc; i++) {
        int c = (int)bits_to_int(chars[i]);
        if (c == 3) end = 1;
        else if (end == 1) {
            if (opt_harder) etx_errs++;
            else new_error(m, "ETX inside ascii", NULL);
        }
        if (c < 32 || c == 127) sb_printf(&asc, "[%d]", c);
        else { char t[2] = { (char)c, 0 }; sb_puts(&asc, t); }
    }
    m->msg_ascii = ar_strdup(a, asc.s ? asc.s : "");
    free(asc.s);
    if (etx_errs > 0) m->fixederrs += 1;
    return 0;
}

static void pretty_msascii(msg_t *m, sb_t *b)
{
    arena_t *a = m->a;
    sb_puts(b, "MSG: ");
    pretty_header_msbody(m, b);
    sb_printf(b, " %s/%04d", m->pkt_csum_ok ? "C:OK" : "C:no", m->pkt_csum);
    sb_printf(b, " %1d/%1d", m->msg_ctr, m->msg_ctr_max);
    char **full, *rest;
    int nf = slice_extra(a, m->msg_msgdata, 8, &full, &rest);
    sb_printf(b, " csum:%02x msg:", m->msg_checksum);
    for (int i = 0; i < nf; i++) sb_printf(b, "%02x", (int)bits_to_int(full[i]));
    sb_printf(b, ".%s", rest);
    sb_printf(b, " TXT: %-65s +%-6s", m->msg_ascii, m->msg_rest);
    pretty_trailer_ms(m, b);
}

/* bitsparser.IridiumMessagingBCD.__init__ */
static void msbcd_init(msg_t *m)
{
    arena_t *a = m->a;
    m->cls = C_MSBCD;
    m->msg_unknown2 = sub(a, m->msg_data, 0, 1);
    m->msg_msgdata = sub(a, m->msg_data, 1, strlen(m->msg_data));
    size_t n = strlen(m->msg_msgdata);
    char *bcd = ar_alloc(a, n / 4 + 2);
    size_t k = 0;
    for (size_t x = 0; x < n; x += 4) { char t[4]; snprintf(t, sizeof(t), "%01x", bin_int(m->msg_msgdata, x, x + 4)); bcd[k++] = t[0]; }
    bcd[k] = 0;
    m->bcd = bcd;
}

static void pretty_msbcd(msg_t *m, sb_t *b)
{
    sb_puts(b, "MS3: ");
    pretty_header_msbody(m, b);
    sb_printf(b, " %6s %s", m->pkt_cs1, m->msg_unknown2);
    sb_printf(b, " BCD: %-65s", m->bcd);
    pretty_trailer_ms(m, b);
}

/* IridiumMSMessage.upgrade -> Body -> Ascii / BCD */
static void ms_upgrade(msg_t *m)
{
    if (m->nmsblocks <= 0) return;
    perr_t pe;
    if (msbody_init(m, &pe)) { m->cls = C_MS; new_error(m, pe.msg, cls_name(pe.cls)); return; }
    /* IridiumMSMessageBody.upgrade */
    if (m->msg_data && strlen(m->msg_data) >= 5) {
        if (m->msg_format == 5) {
            if (msascii_init(m, &pe)) { m->cls = C_MSBODY; new_error(m, pe.msg, cls_name(pe.cls)); }
            return;
        } else if (m->msg_format == 3) { msbcd_init(m); return; }
        new_error(m, "unknown msg_format", NULL);
    }
}

/* IridiumMessage.upgrade for MS/RA/BC: IridiumECCMessage(self).upgrade() */
static void ecc_family(msg_t *m)
{
    perr_t pe;
    if (ecc_init(m, &pe)) { m->cls = C_IRIDIUM; new_error(m, pe.msg, cls_name(pe.cls)); return; }
    if (m->error) return;
    if (strcmp(m->msgtype, "MS") == 0) {
        if (ms_init(m, &pe)) { m->cls = C_ECC; new_error(m, pe.msg, cls_name(pe.cls)); return; }
        ms_upgrade(m);
    } else if (strcmp(m->msgtype, "RA") == 0) {
        if (ra_init(m, &pe)) { m->cls = C_ECC; new_error(m, pe.msg, cls_name(pe.cls)); return; }
    } else {
        bc_init(m);
    }
}

/* IridiumLCWMessage.upgrade for DA: IridiumLCWECCMessage(self).upgrade() */
static void da_family(msg_t *m)
{
    lcwecc_init(m);
    if (m->error) return;
    perr_t pe;
    if (da_init(m, &pe)) { m->cls = C_LCWECC; new_error(m, pe.msg, cls_name(pe.cls)); }
}

/* bitsparser.IridiumLCWMessage.upgrade (DA is the ECC path: not yet) */
static void lcw_upgrade(msg_t *m)
{
    if (m->error) return;
    if (strcmp(m->msgtype, "VO") == 0) vo_init(m);
    else if (strcmp(m->msgtype, "IP") == 0) ip_init(m);
    else if (strcmp(m->msgtype, "SY") == 0) sy_upgrade(m);
    else if (strcmp(m->msgtype, "U3") == 0) lcw3_init(m);
    else if (strcmp(m->msgtype, "DA") == 0) da_family(m);
    /* other U*: stays an LCW message */
}


/* ------------------------------------------------------------------- ITL */

/* bitsparser.de_dqpsk */
static int de_dqpsk(const char *bits, int *sym, int max)
{
    static const int imap[4] = { 0, 1, 3, 2 };
    size_t n = strlen(bits);
    int k = 0;
    for (size_t x = 0; x + 1 < n && k < max; x += 2) sym[k++] = imap[(bits[x] == '1') * 2 + (bits[x + 1] == '1')];
    for (int c = 1; c < k; c++) sym[c] = (sym[c - 1] + sym[c]) % 4;
    return k;
}

static const itl_entry_t *itl_find(const itl_entry_t *t, int n, const char *key)
{
    for (int i = 0; i < n; i++) if (strcmp(t[i].key, key) == 0) return &t[i];
    return NULL;
}

/* util.hex2bin: "{0:0%db}".format(int(h, 16)) with 4 bits per digit */
static const char *hex2bin(arena_t *a, const char *h)
{
    size_t n = strlen(h);
    char *o = ar_alloc(a, n * 4 + 1);
    for (size_t i = 0; i < n; i++) {
        int v = isdigit((unsigned char)h[i]) ? h[i] - '0' : (tolower((unsigned char)h[i]) - 'a' + 10);
        for (int k = 0; k < 4; k++) o[i * 4 + (size_t)k] = (char)('0' + ((v >> (3 - k)) & 1));
    }
    o[n * 4] = 0;
    return o;
}

/* itl.map_sat; returns -1 for ValueError */
static int itl_map_sat(arena_t *a, int num, int version, const char **sat, const char **mt)
{
    char s1[16], s2[16];
    if (version == 2) {
        if (num == 77) { strcpy(s1, "---"); strcpy(s2, "M08"); }
        else if (num < 66) { snprintf(s1, sizeof(s1), "S%02d", num % 11 + 1); snprintf(s2, sizeof(s2), "M%02d", num / 11 + 1); }
        else if (num >= 82 && num <= 84) { snprintf(s1, sizeof(s1), "R%02d", ((num - 82) % 3) + 1); strcpy(s2, "N01"); }
        else if (num >= 85 && num <= 95) { snprintf(s1, sizeof(s1), "S%02d", num - 84); strcpy(s2, "N02"); }
        else if (num >= 96 && num <= 107) { snprintf(s1, sizeof(s1), "R%02d", ((num - 96) % 3) + 1); snprintf(s2, sizeof(s2), "N%02d", ((num - 96) / 3) + 3); }
        else if (num == 108) { strcpy(s1, "---"); strcpy(s2, "SSS"); }
        else if (num == 111) { strcpy(s1, "---"); strcpy(s2, "N08"); }
        else { strcpy(s1, "---"); snprintf(s2, sizeof(s2), "%03d", num); }
    } else {
        if (num >= 88) return -1;
        snprintf(s1, sizeof(s1), "S%02d", num % 11 + 1); snprintf(s2, sizeof(s2), "M%02d", num / 11 + 1);
    }
    *sat = ar_strdup(a, s1); *mt = ar_strdup(a, s2);
    return 0;
}

/* bitsparser.IridiumSTLMessage.__init__ (default options); 1 = ParserError */
static int stl_init(msg_t *m, perr_t *pe)
{
    arena_t *a = m->a;
    m->cls = C_STL;
    pe->cls = C_STL;
    m->header = "<11>";
    m->has_fixederrs = 1; m->fixederrs = 0;
    const char *d = descr_joined(m);
    if (strlen(d) < 8 * 8 * 12) { pe->msg = "ITL content too short"; return 1; }
    static __thread int sym[1024];
    int ns = de_dqpsk(d, sym, 1024);
    /* split_qpsk -> i, q bit strings */
    char *ib = ar_alloc(a, (size_t)ns + 1), *qb = ar_alloc(a, (size_t)ns + 1);
    for (int k = 0; k < ns; k++) {
        int v = sym[k];
        ib[k] = (v == 1 || v == 2) ? '1' : '0';
        qb[k] = (v == 2 || v == 3) ? '1' : '0';
    }
    ib[ns] = 0; qb[ns] = 0;
    /* ["%02x" % int(x,2) for x in slice(bits, 8)] */
    int nbi = (ns + 7) / 8;
    char *ih = ar_alloc(a, (size_t)nbi * 2 + 1), *qh = ar_alloc(a, (size_t)nbi * 2 + 1);
    for (int k = 0; k < nbi; k++) {
        snprintf(ih + 2 * k, 3, "%02x", bin_int(ib, (size_t)k * 8, (size_t)k * 8 + 8));
        snprintf(qh + 2 * k, 3, "%02x", bin_int(qb, (size_t)k * 8, (size_t)k * 8 + 8));
    }
    size_t lih = strlen(ih), lqh = strlen(qh);
    m->has_iq = 1;
    m->nitl_i = 2;
    m->itl_i[0] = sub(a, ih, 0, 32);
    m->itl_i[1] = sub(a, ih, 32, lih);
    m->nitl_q = 0;
    for (size_t x = 0; x < lqh && m->nitl_q < 8; x += 24) m->itl_q[m->nitl_q++] = sub(a, qh, x, x + 24);

    const int MAX_DIFF = 10;
    m->itl_version = -1;
    for (int k = 0; k < ITL_N_PRS_HDR; k++) if (strcmp(ITL_PRS_HDR[k], m->itl_i[0]) == 0) { m->itl_version = k; break; }
    if (m->itl_version < 0) {
        if (opt_harder && bitdiff(hex2bin(a, m->itl_i[0]), hex2bin(a, ITL_PRS_HDR[2])) < MAX_DIFF) {
            m->fixederrs += 1;
            m->itl_version = 2;
        } else { pe->msg = "ITL PRS I#0 (version) unknown"; return 1; }
    }
    char h[8];
    snprintf(h, sizeof(h), "V%d", m->itl_version);
    m->header = ar_strdup(a, h);
    m->nmsg = 0;
    static __thread char em[64];
    if (m->itl_version == 0) {
        m->nitl_i = 0;
        for (size_t x = 0; x < lih && m->nitl_i < 8; x += 32) m->itl_i[m->nitl_i++] = sub(a, ih, x, x + 32);
        m->nitl_q = 0;
        for (size_t x = 0; x < lqh && m->nitl_q < 8; x += 32) m->itl_q[m->nitl_q++] = sub(a, qh, x, x + 32);
        return 0;
    } else if (m->itl_version == 1) {
        const itl_entry_t *e = itl_find(ITL_MAP_PLANE_V1, ITL_MAP_PLANE_V1_N, m->itl_i[1]);
        if (!e) { pe->msg = "ITL V1 PRS I#1 (plane) unknown"; return 1; }
        m->plane = e->val;
        for (int k = 0; k < m->nitl_q; k++) {
            const itl_entry_t *q = itl_find(ITL_MAP_PRS_V1, ITL_MAP_PRS_V1_N, m->itl_q[k]);
            if (q) { m->msg_int[m->nmsg] = q->val; m->msg_str[m->nmsg++] = NULL; }
            else {
                if (k == 0 || m->msg_int[0] < 77) { snprintf(em, sizeof(em), "ITL V1 PRS Q#%d unknown", k); pe->msg = ar_strdup(a, em); return 1; }
                m->msg_str[m->nmsg++] = m->itl_q[k];
            }
        }
    } else if (opt_harder) {
        /* IridiumSTLMessage.__init__, itl_version == 2 and args.harder */
        m->plane = -1;
        const itl_entry_t *e = itl_find(ITL_MAP_PLANE, ITL_MAP_PLANE_N, m->itl_i[1]);
        if (e) m->plane = e->val;
        else {
            const char *ib1 = hex2bin(a, m->itl_i[1]);
            for (int k = 0; k < ITL_MAP_PLANE_N; k++)
                if (bitdiff(ib1, hex2bin(a, ITL_MAP_PLANE[k].key)) < MAX_DIFF * 2) { m->plane = k + 1; m->fixederrs += 1; break; }
            if (m->plane < 0) { pe->msg = "ITL V2 PRS I#1 (plane) unknown"; return 1; }
        }
        int have[8] = { 0 };
        int cat = -1;                        /* None */
        m->nmsg = m->nitl_q;
        for (int k = 0; k < m->nitl_q; k++) { m->msg_str[k] = NULL; m->msg_int[k] = 0; }
        for (int qidx = 0; qidx < m->nitl_q; qidx++) {
            int mindist = 999;
            if (qidx > 0 && have[0] && !m->msg_str[0] && m->msg_int[0] == 108) {
                m->msg_str[qidx] = m->itl_q[qidx];   /* (then falls through: `next` is a no-op) */
                have[qidx] = 1;
            }
            const itl_entry_t *q = itl_find(ITL_MAP_PRS, ITL_MAP_PRS_N, m->itl_q[qidx]);
            if (q) { m->msg_int[qidx] = q->val; m->msg_str[qidx] = NULL; have[qidx] = 1; cat = q->type; }
            else {
                const char *qb = hex2bin(a, m->itl_q[qidx]);
                int st, en;
                if (qidx == 0) { if (m->plane % 2 == 0) { st = 0; en = 256; } else { st = 256; en = 512; } }
                else if (qidx == 1 || qidx == 3) { if (cat < 0) { pe->msg = "ITL category error"; return 1; } cat ^= 1; st = 128 * cat; en = 128 * (cat + 1); }
                else if (qidx == 2) { if (cat < 0) { pe->msg = "ITL category error"; return 1; } cat ^= 3; st = 128 * cat; en = 128 * (cat + 1); }
                else { pe->msg = "ITL category error"; return 1; }
                for (int i = 0; st + i < en && st + i < ITL_MAP_PRS_N; i++) {
                    int dist = bitdiff(qb, hex2bin(a, ITL_MAP_PRS[st + i].key));
                    if (dist < mindist) mindist = dist;
                    if (dist < MAX_DIFF) {
                        m->msg_int[qidx] = i % 128; m->msg_str[qidx] = NULL; have[qidx] = 1;
                        if (cat < 0) cat = (i + st) / 128;
                        m->fixederrs += 1;
                        break;
                    }
                }
            }
            if (!have[qidx]) {
                snprintf(em, sizeof(em), "ITL V2 PRS Q#%d unknown", qidx);
                new_error(m, ar_strdup(a, em), NULL);
                snprintf(em, sizeof(em), "ITL PRS dist=%d", mindist);
                pe->msg = ar_strdup(a, em);
                return 1;
            }
        }
    } else {
        const itl_entry_t *e = itl_find(ITL_MAP_PLANE, ITL_MAP_PLANE_N, m->itl_i[1]);
        if (!e) { pe->msg = "ITL V2 PRS I#1 (plane) unknown"; return 1; }
        m->plane = e->val;
        for (int k = 0; k < m->nitl_q; k++) {
            const itl_entry_t *q = itl_find(ITL_MAP_PRS, ITL_MAP_PRS_N, m->itl_q[k]);
            if (q) { m->msg_int[m->nmsg] = q->val; m->msg_str[m->nmsg++] = NULL; }
            else {
                if (k == 0 || m->msg_int[0] != 108) { snprintf(em, sizeof(em), "ITL V2 PRS Q#%d unknown", k); pe->msg = ar_strdup(a, em); return 1; }
                m->msg_str[m->nmsg++] = m->itl_q[k];
            }
        }
        if (m->msg_int[0] != 108) {
            char san[16]; int ks = 0;
            for (int k = 0; k < m->nitl_q && ks < 15; k++) {
                const itl_entry_t *q = itl_find(ITL_MAP_PRS, ITL_MAP_PRS_N, m->itl_q[k]);
                san[ks++] = (char)('0' + (q ? q->type : 0));
            }
            san[ks] = 0;
            int ok = (m->plane % 2 == 0) ? (strcmp(san, "0123") == 0 || strcmp(san, "1032") == 0)
                                         : (strcmp(san, "2301") == 0 || strcmp(san, "3210") == 0);
            if (!ok) { pe->msg = "ITL V2 PRS from unexpected set"; return 1; }
        }
    }
    if (itl_map_sat(a, m->msg_int[0], m->itl_version, &m->sat, &m->mt) < 0) { pe->msg = "ITL invalid sat ID"; return 1; }
    /* self.msg = self.msg[1:] */
    for (int k = 1; k < m->nmsg; k++) { m->msg_int[k - 1] = m->msg_int[k]; m->msg_str[k - 1] = m->msg_str[k]; }
    m->nmsg--;
    return 0;
}

/* bitsparser.IridiumSTLMessage.pretty */
static void pretty_stl(msg_t *m, sb_t *b)
{
    sb_puts(b, "ITL: ");
    pretty_header_iridium(m, b);
    if (m->itl_version == 0) {
        sb_puts(b, " - <");
        for (int k = 0; k < m->nitl_i; k++) { if (k) sb_puts(b, " "); sb_puts(b, m->itl_i[k]); }
        sb_puts(b, "> <");
        for (int k = 0; k < m->nitl_q; k++) { if (k) sb_puts(b, " "); sb_puts(b, m->itl_q[k]); }
        sb_puts(b, ">");
    } else {
        sb_printf(b, " OK P%d %s %s", m->plane, m->sat, m->mt);
        for (int k = 0; k < m->nmsg; k++) {
            if (m->msg_str[k]) { sb_puts(b, " "); sb_puts(b, m->msg_str[k]); }
            else {
                char t[40]; int v = m->msg_int[k], n = 0;
                for (int i = 31; i >= 0; i--) if ((v >> i) & 1) { n = i + 1; break; }
                if (n < 7) n = 7;
                for (int i = 0; i < n; i++) t[i] = (char)('0' + ((v >> (n - 1 - i)) & 1));
                t[n] = 0;
                sb_puts(b, " "); sb_puts(b, t);
            }
        }
    }
    pretty_trailer_iridium(m, b);
}


/* ------------------------------------------------------------- IAQ, NXT */

/* bitsparser.iaq_crc16: crcmod.mkCrcFun(poly=0x15101, initCrc=0, rev=False) */
static unsigned iaq_crc16(const unsigned char *d, int n)
{
    unsigned crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (unsigned)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x5101) & 0xffff : (crc << 1) & 0xffff;
    }
    return crc;
}

/* bitsparser.IridiumAQMessage.__init__; 1 = ParserError */
static int aq_init(msg_t *m, perr_t *pe)
{
    m->cls = C_AQ;
    m->has_fixederrs = 1; m->fixederrs = 0;
    const char *bits = descr_joined(m);
    size_t n = strlen(bits);
    int k = 0;
    static const char imap[4] = { '0', 'e', 'e', '1' };
    for (size_t x = 0; x + 1 < n && k < 127; x += 2) m->aq_sym[k++] = imap[(bits[x] == '1') * 2 + (bits[x + 1] == '1')];
    m->aq_sym[k] = 0;
    if (strchr(m->aq_sym, 'e')) { pe->msg = "IAQ content not BPSK"; pe->cls = C_AQ; return 1; }
    const char *sy = m->aq_sym;
    int rid = 0;
    static const int order[8] = { 4, 6, 8, 10, 5, 7, 9, 11 };
    for (int i = 0; i < 8; i++) rid = (rid << 1) | (sy[order[i]] == '1');
    m->rid = rid;
    unsigned char val[2];
    val[0] = (unsigned char)bin_int(sy, 0, 4);
    val[1] = (unsigned char)bin_int(sy, 4, 12);
    m->ridcrc = bin_int(sy, 12, (size_t)k);
    m->aq_crcval = (int)(iaq_crc16(val, 2) >> 2);
    return 0;
}

static void pretty_aq(msg_t *m, sb_t *b)
{
    sb_puts(b, "IAQ: ");
    pretty_header_iridium(m, b);
    sb_printf(b, " %.4s Rid:%03d", m->aq_sym, m->rid);
    if (m->ridcrc == m->aq_crcval) sb_puts(b, " CRC:OK");
    else sb_printf(b, " CRC:no[%04x]", m->ridcrc);
    pretty_trailer_iridium(m, b);
}

/* bitsparser.IridiumNXTMessage.__init__ */
static void nxt_init(msg_t *m)
{
    m->cls = C_NXT;
    m->has_fixederrs = 1; m->fixederrs = 0;
    const char *d = descr_joined(m);
    if (strlen(d) < 233) new_error(m, "next frame too short", NULL);
    const char *ones = sub(m->a, d, 176, 206);
    if (strchr(ones, '0')) m->fixederrs += 1;
}

static void pretty_nxt(msg_t *m, sb_t *b)
{
    arena_t *a = m->a;
    const char *d = descr_joined(m);
    sb_puts(b, "NXT: ");
    pretty_header_iridium(m, b);
    sb_puts(b, " "); put_group(b, sub(a, d, 0, 32), 8);
    sb_puts(b, " | "); put_group(b, sub(a, d, 32, 36), 2);
    sb_puts(b, " > "); put_group(b, sub(a, d, 36, strlen(d)), 10);
    pretty_trailer_iridium(m, b);
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
    if (strcmp(m->msgtype, "MS") == 0 || strcmp(m->msgtype, "RA") == 0 || strcmp(m->msgtype, "BC") == 0) {
        ecc_family(m);
        return;
    }
    if (strcmp(m->msgtype, "AQ") == 0) {
        perr_t ape;
        if (aq_init(m, &ape)) { m->cls = C_IRIDIUM; new_error(m, ape.msg, cls_name(ape.cls)); }
        return;
    }
    if (strcmp(m->msgtype, "NX") == 0) { nxt_init(m); return; }
    if (strcmp(m->msgtype, "TL") == 0) {
        perr_t tpe;
        if (stl_init(m, &tpe)) { m->cls = C_IRIDIUM; new_error(m, tpe.msg, cls_name(tpe.cls)); }
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
    case C_STL:     pretty_stl(m, &b); break;
    case C_AQ:      pretty_aq(m, &b); break;
    case C_NXT:     pretty_nxt(m, &b); break;
    case C_ECC:     pretty_ecc(m, &b); break;
    case C_LCWECC:  pretty_lcwecc(m, &b); break;
    case C_DA:      pretty_da(m, &b); break;
    case C_BC:      pretty_bc(m, &b); break;
    case C_RA:      pretty_ra(m, &b); break;
    case C_MS:      pretty_ms(m, &b); break;
    case C_MSBODY:  pretty_msbody(m, &b); break;
    case C_MSASCII: pretty_msascii(m, &b); break;
    case C_MSBCD:   pretty_msbcd(m, &b); break;
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
