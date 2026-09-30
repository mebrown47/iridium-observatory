/*
 * Native port of iridium-toolkit's reassembler.py - see tk_reassembler.h.
 *
 * Ported from iridium-toolkit (https://github.com/muccc/iridium-toolkit)
 * commit 8888124: reassembler.py, iridiumtk/reassembler/base.py (Reassemble,
 * MyObject), ida.py (ReassembleIDA), sbd.py (ReassembleIDASBD,
 * ReassembleIDASBDACARS), util.py (to_ascii, channelize_str, dt). Each
 * function names the Python one it follows.
 * (c) Sec & schneider, 2-Clause BSD License - see LICENSES/BSD-2-Clause-iridium-toolkit.txt
 *
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tk_reassembler.h"

#include <ctype.h>
#include <math.h>
#include <regex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_LIBACARS
#include <sys/time.h>
#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/reassembly.h>
#include <libacars/vstring.h>
#include <libacars/version.h>
#endif

/* ------------------------------------------------------------ bytes, text */

typedef struct { unsigned char *d; size_t n; } bytes_t;

static bytes_t b_new(const unsigned char *d, size_t n)
{
    bytes_t b = { malloc(n ? n : 1), n };
    if (!b.d) abort();
    if (n) memcpy(b.d, d, n);
    return b;
}
static bytes_t b_slice(bytes_t b, long from, long to)     /* b[from:to], Python clamping */
{
    long n = (long)b.n;
    if (from < 0) from += n;
    if (to < 0) to += n;
    if (from < 0) from = 0;
    if (to > n) to = n;
    if (to < from) to = from;
    return b_new(b.d + from, (size_t)(to - from));
}
static void b_free(bytes_t *b) { free(b->d); b->d = NULL; b->n = 0; }
static void b_append(bytes_t *b, bytes_t x)
{
    b->d = realloc(b->d, b->n + x.n + 1);
    if (!b->d) abort();
    memcpy(b->d + b->n, x.d, x.n);
    b->n += x.n;
}

typedef struct { char *s; size_t n, cap; } sb_t;
static void sb_grow(sb_t *b, size_t k)
{
    if (b->n + k + 1 <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (b->n + k + 1 > c) c *= 2;
    b->s = realloc(b->s, c);
    if (!b->s) abort();
    b->cap = c;
}
static void sb_puts(sb_t *b, const char *s) { size_t k = strlen(s); sb_grow(b, k); memcpy(b->s + b->n, s, k + 1); b->n += k; }
__attribute__((format(printf, 2, 3)))
static void sb_printf(sb_t *b, const char *f, ...)
{
    va_list ap;
    va_start(ap, f); int k = vsnprintf(NULL, 0, f, ap); va_end(ap);
    if (k <= 0) { if (!b->s) { sb_grow(b, 0); b->s[0] = 0; } return; }
    sb_grow(b, (size_t)k);
    va_start(ap, f); vsnprintf(b->s + b->n, (size_t)k + 1, f, ap); va_end(ap);
    b->n += (size_t)k;
}
static const char *sb_str(sb_t *b) { if (!b->s) { sb_grow(b, 0); b->s[0] = 0; } return b->s; }
/* raw bytes, NULs included (Python's bytes.decode('latin-1') in a str) */
static void sb_putb(sb_t *b, const unsigned char *d, size_t k) { sb_grow(b, k); memcpy(b->s + b->n, d, k); b->n += k; b->s[b->n] = 0; }
/* the line, byte for byte, and a newline */
static void sb_writeln(sb_t *b, FILE *f) { if (b->n) fwrite(b->s, 1, b->n, f); fputc('\n', f); }

/* bytes.hex(sep) */
static void put_hex(sb_t *o, bytes_t b, const char *sep)
{
    for (size_t i = 0; i < b.n; i++) sb_printf(o, "%s%02x", i && sep ? sep : "", b.d[i]);
    if (!b.n) sb_puts(o, "");
}

/* util.to_ascii(data, dot, escape) */
static void put_ascii(sb_t *o, bytes_t b, int dot, int escape)
{
    for (size_t i = 0; i < b.n; i++) {
        int c = b.d[i];
        if (c >= 32 && c < 127) { char t[2] = { (char)c, 0 }; sb_puts(o, t); }
        else if (dot) sb_puts(o, ".");
        else if (escape) {
            if (c == 0x0d) sb_puts(o, "\\r");
            else if (c == 0x0a) sb_puts(o, "\\n");
            else sb_printf(o, "\\x{%02x}", c);
        } else sb_printf(o, "[%02x]", c);
    }
    if (!b.n) sb_puts(o, "");
}

/* util.channelize_str(freq) */
static void channelize_str(long freq, char *out, size_t n)
{
    const double channel_width = 1e7 / (30 * 8);
    long fbase = freq - 1616000000L;
    long freq_chan = (long)((double)fbase / channel_width);     /* int(): toward zero */
    long sb = (long)((double)freq_chan / 8) + 1;
    long fa = ((freq_chan % 8) + 8) % 8 + 1;                    /* Python % */
    long sx = freq_chan - 30 * 8 + 1;
    double foff = fmod((double)fbase, channel_width);
    if (foff < 0) foff += channel_width;                        /* Python float % */
    double freq_off = foff - channel_width / 2;
    if (sb > 30) snprintf(out, n, "S.%02ld|%+06.0f", sx, freq_off);
    else snprintf(out, n, "%02ld.%1ld|%+06.0f", sb, fa, freq_off);
}

/* datetime.fromtimestamp(t, utc): seconds, microseconds rounded half-even */
static time_t ts_seconds(double t)
{
    double s = floor(t), us = nearbyint((t - s) * 1e6);
    if (us >= 1000000) s += 1;
    return (time_t)s;
}

/* util.dt.epoch(t).isoformat(timespec='seconds') */
static void iso_utc_seconds(double t, char *out, size_t n)
{
    time_t tt = ts_seconds(t);
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* util.dt.epoch_local(int(t)).isoformat(): local time, offset +HHMM, or Z */
static void iso_local(long t, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    char base[40];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    long off = tm.tm_gmtoff;
    if (off == 0) snprintf(out, n, "%sZ", base);
    else {
        long a = off < 0 ? -off : off;
        snprintf(out, n, "%s%c%02d%02d", base, off < 0 ? '-' : '+', (int)((a / 3600) % 100), (int)((a / 60) % 60));
    }
}

/* ---------------------------------------------------------------- state */

static tkr_mode_t mode;
static FILE *out;
static long stat_line, stat_filter;
/* ReassembleIDA */
typedef struct { long freq; double *time; int ntime; int ctr; char *dat; int cont, ul; } idabuf_t;
static idabuf_t *buf; static int nbuf, capbuf;
static long stat_broken, stat_ok, stat_fragments, stat_dupes;
static double otime; static char *odata; static long ofreq; static double olevel;
/* ReassembleIDASBD */
typedef struct { char *typ; double time; int ul; bytes_t prehdr, data; } sbdobj_t;
typedef struct { int no, cnt; sbdobj_t *p; double t; } multi_t;
static multi_t *multi; static int nmulti, capmulti;
static long sbd_short, sbd_single, sbd_cnt, sbd_multi, sbd_assembled, sbd_broken;
static regex_t re_ida;
static int arg_json, arg_showerrs, arg_nopings;

int tkr_set_arg(const char *a)
{
    if (!strcmp(a, "json")) arg_json = 1;
    else if (!strcmp(a, "showerrs")) arg_showerrs = 1;
    else if (!strcmp(a, "nopings")) arg_nopings = 1;
    else if (!strcmp(a, "perfect")) ;
    else return -1;
    return 0;
}

/* json.dumps of a str (ensure_ascii) */
static void put_json_str(sb_t *o, const unsigned char *s, size_t n)
{
    sb_puts(o, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c == '"') sb_puts(o, "\\\"");
        else if (c == '\\') sb_puts(o, "\\\\");
        else if (c == '\n') sb_puts(o, "\\n");
        else if (c == '\r') sb_puts(o, "\\r");
        else if (c == '\t') sb_puts(o, "\\t");
        else if (c == '\b') sb_puts(o, "\\b");
        else if (c == '\f') sb_puts(o, "\\f");
        else if (c < 0x20 || c >= 0x7f) sb_printf(o, "\\u%04x", c);
        else { char t[2] = { (char)c, 0 }; sb_puts(o, t); }
    }
    sb_puts(o, "\"");
}

/* repr(float) as json.dumps prints it: the shortest text that reads back */
static void put_json_float(sb_t *o, double v)
{
    char t[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(t, sizeof(t), "%.*e", p - 1, v);
        if (strtod(t, NULL) == v) {
            int e = atoi(strchr(t, 'e') + 1);
            if (e < -4 || e >= 16) {                  /* scientific, as repr */
                char mant[32]; int ex;
                sscanf(t, "%31[^e]e%d", mant, &ex);
                size_t ml = strlen(mant);
                while (ml && mant[ml - 1] == '0' && strchr(mant, '.')) mant[--ml] = 0;
                if (ml && mant[ml - 1] == '.') mant[--ml] = 0;
                sb_printf(o, "%se%c%02d", mant, ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
            } else {
                int dec = p - 1 - e;
                if (dec < 1) dec = 1;
                snprintf(t, sizeof(t), "%.*f", dec, v);
                size_t l = strlen(t);
                while (l > 2 && t[l - 1] == '0' && t[l - 2] != '.') t[--l] = 0;
                sb_puts(o, t);
            }
            return;
        }
    }
    sb_printf(o, "%.17g", v);
}

tkr_mode_t tkr_mode_from_name(const char *name)
{
    if (!strcmp(name, "ida")) return TKR_IDA;
    if (!strcmp(name, "sbd")) return TKR_SBD;
    if (!strcmp(name, "acars")) return TKR_ACARS;
#ifdef HAVE_LIBACARS
    if (!strcmp(name, "libacars")) return TKR_LIBACARS;
#endif
    return TKR_OFF;
}

void tkr_init(tkr_mode_t m, FILE *o)
{
    mode = m; out = o;

    /* ida.py filter regex (one " cont=" per IDA line, so leftmost-longest
     * POSIX matching picks the same groups as Python's) */
    if (regcomp(&re_ida, "^.* cont=([0-9]) ([0-9]) ctr=([0-9]+) [0-9]+ len=([0-9]+) 0:.000 "
                         "\\[([0-9a-f.!]*)\\][[:space:]]+..../.... CRC:OK", REG_EXTENDED) != 0)
        abort();
}

/* ------------------------------------------------------------ SBD / ACARS */

static const struct { const char *k; int n; const char *v; } acars_labels[] = {
    { "_\x7f", 2, "Demand mode" }, { "H1", 2, "Message to/from terminal" }, { "52", 2, "Ground UTC request" },
    { "C1", 2, "Uplink to cockpit printer No.1" }, { "C2", 2, "Uplink to cockpit printer No.2" },
    { "C3", 2, "Uplink to cockpit printer No.3" }, { "Q0", 2, "Link Test" },
};

/* crcmod.predefined "kermit": CRC-16, poly 0x1021 reflected, init 0 */
static unsigned crc_kermit(bytes_t b)
{
    unsigned crc = 0;
    for (size_t i = 0; i < b.n; i++) {
        crc ^= b.d[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc & 0xffff;
}

/* sbd.ReassembleIDASBDACARS.consume_l2 (plain output; no json/showerrs/nopings) */
static void acars_consume_l2(sbdobj_t *q)
{
    if (q->data.n == 0) return;
    if (q->data.d[0] != 1) return;
    if (q->data.n <= 2) return;
    int e_crc_fail = 0, e_crc_missing = 0, e_parity = 0, e_etx = 0;
    bytes_t data = b_slice(q->data, 1, (long)q->data.n), csum = { NULL, 0 }, hdr = { NULL, 0 };
    if (data.n && data.d[data.n - 1] == 0x7f) {
        bytes_t c = b_slice(data, -3, -1), d2 = b_slice(data, 0, -3);
        b_free(&data); data = d2; csum = c;
    }
    if (data.n && data.d[0] == 0x3) {
        bytes_t h = b_slice(data, 0, 8), d2 = b_slice(data, 8, (long)data.n);
        b_free(&data); data = d2; hdr = h;
    }
    if (csum.n > 0) {
        bytes_t all = b_new(data.d, data.n); b_append(&all, csum);
        if (crc_kermit(all) != 0) e_crc_fail = 1;
        b_free(&all);
    } else e_crc_missing = 1;
    if (data.n < 13) goto done;           /* TRUNCATED: thrown away */
    for (size_t i = 0; i < data.n; i++) {
        if (__builtin_popcount(data.d[i]) % 2 == 0) e_parity = 1;
        data.d[i] &= 0x7f;
    }
    bytes_t mode_ = b_slice(data, 0, 1), f_reg = b_slice(data, 1, 8), ack = b_slice(data, 8, 9),
            label = b_slice(data, 9, 11), b_id = b_slice(data, 11, 12), rest = b_slice(data, 12, (long)data.n);
    int cont = 0;
    if (rest.n && rest.d[rest.n - 1] == 0x03) rest.n--;
    else if (rest.n && rest.d[rest.n - 1] == 0x17) { cont = 1; rest.n--; }
    else e_etx = 1;
    bytes_t seqn = { NULL, 0 }, f_no = { NULL, 0 }, txt = { NULL, 0 };
    int have_seq = 0;
    if (rest.n > 0 && rest.d[0] == 2) {
        if (q->ul) { seqn = b_slice(rest, 1, 5); f_no = b_slice(rest, 5, 11); txt = b_slice(rest, 11, (long)rest.n); have_seq = 1; }
        else txt = b_slice(rest, 1, (long)rest.n);
    }
    int nerr = e_crc_fail + e_crc_missing + e_parity + e_etx;
    int ping = label.n == 2 && label.d[0] == '_' && label.d[1] == 0x7f;
    if ((nerr == 0 || arg_showerrs) && !(ping && arg_nopings)) {
        char ts[40];
        iso_utc_seconds(q->time, ts, sizeof(ts));
        size_t k = 0;
        while (k < f_reg.n && f_reg.d[k] == '.') k++;
        sb_t o = { 0 };
        if (arg_json) {
            sb_puts(&o, "{\"app\": {\"name\": \"iridium-toolkit\", \"version\": \"0.0.1\"}, "
                        "\"source\": {\"transport\": \"iridium\", \"protocol\": \"acars\"}, \"acars\": {");
            sb_printf(&o, "\"timestamp\": \"%s\", \"errors\": %d, \"link_direction\": \"%s\", \"block_end\": %s",
                      ts, nerr, q->ul ? "uplink" : "downlink", cont ? "false" : "true");
            sb_puts(&o, ", \"mode\": "); put_json_str(&o, mode_.d, mode_.n);
            sb_puts(&o, ", \"tail\": "); put_json_str(&o, f_reg.d + k, f_reg.n - k);
            if (have_seq) { sb_puts(&o, ", \"flight\": "); put_json_str(&o, f_no.d, f_no.n); }
            sb_puts(&o, ", \"label\": ");
            if (ping) put_json_str(&o, (const unsigned char *)"_d", 2);
            else put_json_str(&o, label.d, label.n);
            sb_puts(&o, ", \"block_id\": "); put_json_str(&o, b_id.d, b_id.n);
            if (have_seq) { sb_puts(&o, ", \"message_number\": "); put_json_str(&o, seqn.d, seqn.n); }
            sb_puts(&o, ", \"ack\": ");
            if (ack.n == 1 && ack.d[0] == 0x15) put_json_str(&o, (const unsigned char *)"!", 1);
            else put_json_str(&o, ack.d, ack.n);
            sb_puts(&o, ", \"text\": "); put_json_str(&o, txt.d, txt.n);
            sb_printf(&o, "}, \"freq\": %ld, \"level\": ", ofreq);
            put_json_float(&o, olevel);
            sb_puts(&o, ", \"header\": \"");
            put_hex(&o, hdr, NULL);
            sb_puts(&o, "\"}");
            fprintf(out, "%s\n", sb_str(&o));
            free(o.s);
            goto freed;
        }
        sb_printf(&o, "%s ", ts);
        if (hdr.n > 0) { sb_puts(&o, "[hdr: "); put_hex(&o, hdr, NULL); sb_puts(&o, "]"); }
        else sb_printf(&o, "%-23s", "");
        sb_puts(&o, " ");
        sb_printf(&o, "Dir:%s ", q->ul ? "UL" : "DL");
        sb_puts(&o, "Mode:"); sb_putb(&o, mode_.d, mode_.n); sb_puts(&o, " ");
        sb_puts(&o, "REG:"); sb_putb(&o, f_reg.d + k, f_reg.n - k);         /* "%-7s" */
        for (size_t pad = f_reg.n - k; pad < 7; pad++) sb_puts(&o, " ");
        sb_puts(&o, " ");
        if (ack.n && ack.d[0] == 21) sb_puts(&o, "NAK  ");
        else { sb_puts(&o, "ACK:"); sb_putb(&o, ack.d, ack.n); }
        sb_puts(&o, " ");
        sb_puts(&o, "Label:");
        if (ping) sb_puts(&o, "_?");
        else put_ascii(&o, label, 0, 1);
        sb_puts(&o, " ");
        const char *lname = NULL;
        for (size_t i = 0; i < sizeof(acars_labels) / sizeof(acars_labels[0]); i++)
            if (label.n == 2 && memcmp(label.d, acars_labels[i].k, 2) == 0) lname = acars_labels[i].v;
        sb_printf(&o, "(%s) ", lname ? lname : "?");
        sb_puts(&o, "bID:"); put_ascii(&o, b_id, 0, 1); sb_puts(&o, " ");
        if (q->ul && have_seq) {
            sb_puts(&o, "SEQ: "); put_ascii(&o, seqn, 0, 1);
            sb_puts(&o, ", FNO: "); put_ascii(&o, f_no, 0, 1); sb_puts(&o, " ");
        }
        if (txt.n > 0) { sb_puts(&o, "["); put_ascii(&o, txt, 0, 1); sb_puts(&o, "]"); }
        if (cont) sb_puts(&o, " CONT'd");
        if (nerr) {
            /* q.errors in order: CRC_FAIL / CRC_MISSING, PARITY_FAIL, ETX incorrect */
            sb_puts(&o, " ");
            int first = 1;
            if (e_crc_fail) { sb_puts(&o, "CRC_FAIL"); first = 0; }
            if (e_crc_missing) { sb_puts(&o, first ? "CRC_MISSING" : " CRC_MISSING"); first = 0; }
            if (e_parity) { sb_puts(&o, first ? "PARITY_FAIL" : " PARITY_FAIL"); first = 0; }
            if (e_etx) sb_puts(&o, first ? "ETX incorrect" : " ETX incorrect");
        }
        sb_writeln(&o, out);
        free(o.s);
    }
freed:
    b_free(&mode_); b_free(&f_reg); b_free(&ack); b_free(&label); b_free(&b_id); b_free(&rest);
    b_free(&seqn); b_free(&f_no); b_free(&txt);
done:
    b_free(&data); b_free(&csum); b_free(&hdr);
}

/* sbd.ReassembleIDASBD.consume_l2 */
static void sbd_consume_l2(sbdobj_t *q)
{
    char ts[48], h[64];
    iso_local((long)q->time, ts, sizeof(ts));
    size_t tl = strlen(q->typ);
    char tsel[32]; int k = 0;
    for (size_t i = 3; i < tl && k < 31; i += 4) tsel[k++] = q->typ[i];   /* typ[3::4] */
    tsel[k] = 0;
    snprintf(h, sizeof(h), "%s[%s]", q->ul ? "UL" : "DL", tsel);
    sb_t hdr = { 0 };
    sb_printf(&hdr, "%-8s <", h);
    put_hex(&hdr, q->prehdr, ":");
    sb_puts(&hdr, ">");
    if (q->data.n > 0) {
        sb_t d = { 0 }, a = { 0 };
        put_hex(&d, q->data, " ");
        put_ascii(&a, q->data, 1, 0);
        fprintf(out, "%s %-99s %s | %s\n", ts, sb_str(&hdr), sb_str(&d), sb_str(&a));
        free(d.s); free(a.s);
    } else fprintf(out, "%s %s\n", ts, sb_str(&hdr));
    free(hdr.s);
}

#ifdef HAVE_LIBACARS
/* ---- json.loads / json.dumps (Python's defaults) for libacars' JSON ---- */

typedef struct jv jv_t;
struct jv {
    enum { J_NULL, J_TRUE, J_FALSE, J_INT, J_FLOAT, J_STR, J_ARR, J_OBJ } t;
    char *num;                 /* J_INT: the digits as read (Python int) */
    double f;                  /* J_FLOAT */
    unsigned *cp; size_t ncp;  /* J_STR: code points */
    jv_t **items; char ***keys; size_t *kn; size_t n;   /* J_ARR / J_OBJ */
    unsigned **kcp;            /* J_OBJ: keys as code points */
    size_t *kcpn;
};

static void jv_free(jv_t *v)
{
    if (!v) return;
    free(v->num); free(v->cp);
    for (size_t i = 0; i < v->n; i++) { jv_free(v->items[i]); if (v->kcp) free(v->kcp[i]); }
    free(v->items); free(v->kcp); free(v->kcpn); free(v);
}

static void js_ws(const char **p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++; }

static int js_hex4(const char *p, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return -1;
        *v = *v * 16 + d;
    }
    return 0;
}

/* a JSON string -> code points (UTF-8 decoded, escapes resolved) */
static int js_str(const char **p, unsigned **cp, size_t *ncp)
{
    if (**p != '"') return -1;
    (*p)++;
    size_t cap = 16, n = 0;
    unsigned *o = malloc(cap * sizeof(unsigned));
    while (**p && **p != '"') {
        unsigned c;
        if (**p == '\\') {
            (*p)++;
            char e = **p; (*p)++;
            if (e == 'n') c = '\n'; else if (e == 'r') c = '\r'; else if (e == 't') c = '\t';
            else if (e == 'b') c = '\b'; else if (e == 'f') c = '\f';
            else if (e == 'u') {
                if (js_hex4(*p, &c)) { free(o); return -1; }
                *p += 4;
                if (c >= 0xd800 && c < 0xdc00 && (*p)[0] == '\\' && (*p)[1] == 'u') {
                    unsigned lo;
                    if (!js_hex4(*p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); *p += 6; }
                }
            } else c = (unsigned char)e;          /* " \ / */
        } else {
            unsigned char b = (unsigned char)**p;
            int extra = b < 0x80 ? 0 : b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
            c = extra == 0 ? b : extra == 1 ? (b & 0x1f) : extra == 2 ? (b & 0x0f) : (b & 0x07);
            (*p)++;
            for (int i = 0; i < extra && **p; i++, (*p)++) c = (c << 6) | ((unsigned char)**p & 0x3f);
        }
        if (n == cap) { cap *= 2; o = realloc(o, cap * sizeof(unsigned)); }
        o[n++] = c;
    }
    if (**p != '"') { free(o); return -1; }
    (*p)++;
    *cp = o; *ncp = n;
    return 0;
}

static int cp_eq(const unsigned *a, size_t na, const unsigned *b, size_t nb)
{
    return na == nb && (na == 0 || memcmp(a, b, na * sizeof(unsigned)) == 0);
}

static jv_t *js_value(const char **p)
{
    js_ws(p);
    jv_t *v = calloc(1, sizeof(*v));
    if (**p == '{' || **p == '[') {
        int obj = **p == '{';
        v->t = obj ? J_OBJ : J_ARR;
        (*p)++;
        size_t cap = 0;
        js_ws(p);
        if (**p == (obj ? '}' : ']')) { (*p)++; return v; }
        for (;;) {
            unsigned *kc = NULL; size_t kn = 0;
            if (obj) {
                js_ws(p);
                if (js_str(p, &kc, &kn)) { jv_free(v); return NULL; }
                js_ws(p);
                if (**p != ':') { free(kc); jv_free(v); return NULL; }
                (*p)++;
            }
            jv_t *item = js_value(p);
            if (!item) { free(kc); jv_free(v); return NULL; }
            size_t at = v->n;
            if (obj)                                    /* dict: a repeated key keeps its place */
                for (size_t i = 0; i < v->n; i++)
                    if (cp_eq(v->kcp[i], v->kcpn[i], kc, kn)) { at = i; break; }
            if (at < v->n) { jv_free(v->items[at]); v->items[at] = item; free(kc); }
            else {
                if (v->n == cap) {
                    cap = cap ? cap * 2 : 8;
                    v->items = realloc(v->items, cap * sizeof(jv_t *));
                    if (obj) { v->kcp = realloc(v->kcp, cap * sizeof(unsigned *)); v->kcpn = realloc(v->kcpn, cap * sizeof(size_t)); }
                }
                v->items[v->n] = item;
                if (obj) { v->kcp[v->n] = kc; v->kcpn[v->n] = kn; }
                v->n++;
            }
            js_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == (obj ? '}' : ']')) { (*p)++; return v; }
            jv_free(v); return NULL;
        }
    }
    if (**p == '"') { v->t = J_STR; if (js_str(p, &v->cp, &v->ncp)) { jv_free(v); return NULL; } return v; }
    if (!strncmp(*p, "true", 4)) { v->t = J_TRUE; *p += 4; return v; }
    if (!strncmp(*p, "false", 5)) { v->t = J_FALSE; *p += 5; return v; }
    if (!strncmp(*p, "null", 4)) { v->t = J_NULL; *p += 4; return v; }
    const char *q = *p;
    if (*q == '-') q++;
    while (*q >= '0' && *q <= '9') q++;
    int isf = 0;
    if (*q == '.') { isf = 1; q++; while (*q >= '0' && *q <= '9') q++; }
    if (*q == 'e' || *q == 'E') { isf = 1; q++; if (*q == '+' || *q == '-') q++; while (*q >= '0' && *q <= '9') q++; }
    if (q == *p) { jv_free(v); return NULL; }
    char *t = strndup(*p, (size_t)(q - *p));
    if (isf) { v->t = J_FLOAT; v->f = strtod(t, NULL); free(t); }
    else {
        /* Python int(): "-0" is 0; leading zeros are not valid JSON anyway */
        v->t = J_INT;
        if (!strcmp(t, "-0")) { free(t); t = strdup("0"); }
        v->num = t;
    }
    *p = q;
    return v;
}

/* json.dumps(str) with ensure_ascii */
static void put_json_cp(sb_t *o, const unsigned *cp, size_t n)
{
    sb_puts(o, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned c = cp[i];
        if (c == '"') sb_puts(o, "\\\"");
        else if (c == '\\') sb_puts(o, "\\\\");
        else if (c == '\n') sb_puts(o, "\\n");
        else if (c == '\r') sb_puts(o, "\\r");
        else if (c == '\t') sb_puts(o, "\\t");
        else if (c == '\b') sb_puts(o, "\\b");
        else if (c == '\f') sb_puts(o, "\\f");
        else if (c < 0x20 || (c >= 0x7f && c < 0x10000)) sb_printf(o, "\\u%04x", c);
        else if (c >= 0x10000) {
            unsigned v = c - 0x10000;
            sb_printf(o, "\\u%04x\\u%04x", 0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
        } else { char t[2] = { (char)c, 0 }; sb_puts(o, t); }
    }
    sb_puts(o, "\"");
}

static void put_json_float(sb_t *o, double v);

static void jv_dump(sb_t *o, const jv_t *v)
{
    switch (v->t) {
    case J_NULL: sb_puts(o, "null"); break;
    case J_TRUE: sb_puts(o, "true"); break;
    case J_FALSE: sb_puts(o, "false"); break;
    case J_INT: sb_puts(o, v->num); break;
    case J_FLOAT: put_json_float(o, v->f); break;
    case J_STR: put_json_cp(o, v->cp, v->ncp); break;
    case J_ARR:
        sb_puts(o, "[");
        for (size_t i = 0; i < v->n; i++) { if (i) sb_puts(o, ", "); jv_dump(o, v->items[i]); }
        sb_puts(o, "]");
        break;
    case J_OBJ:
        sb_puts(o, "{");
        for (size_t i = 0; i < v->n; i++) {
            if (i) sb_puts(o, ", ");
            put_json_cp(o, v->kcp[i], v->kcpn[i]);
            sb_puts(o, ": ");
            jv_dump(o, v->items[i]);
        }
        sb_puts(o, "}");
        break;
    }
}

/* sbd.ReassembleIDASBDlibACARS.consume_l2 with the toolkit's libacars.py
 * wrapper */
static la_reasm_ctx *la_ctx;
static void libacars_consume_l2(sbdobj_t *q)
{
    if (q->data.n <= 2) return;
    if (q->data.d[0] != 1) return;
    la_msg_dir d = q->ul ? LA_MSG_DIR_AIR2GND : LA_MSG_DIR_GND2AIR;
    bytes_t x = b_slice(q->data, 1, (long)q->data.n);
    if (x.n && x.d[0] == 3) { bytes_t y = b_slice(x, 8, (long)x.n); b_free(&x); x = y; }
    if (!la_ctx) la_ctx = la_reasm_ctx_new();
    /* libacars.py: timeval(int(time), int(time-int(time))*1000000) - the
     * microseconds are always 0 (int() of the fraction first) */
    struct timeval tv = { (time_t)(long)q->time, 0 };
    la_proto_node *p = la_acars_parse_and_reassemble(x.d, (int)x.n, d, la_ctx, tv);
    b_free(&x);
    if (!p) return;                     /* Python: NULL pointer access (raises) */
    int is_acars = p->td && p->td->json_key && !strcmp(p->td->json_key, "acars");
    la_acars_msg *am = is_acars ? (la_acars_msg *)p->data : NULL;
    int is_err = am ? am->err : 0;
    int is_ping = am && !am->err && (!strcmp(am->label, "_d") || !strcmp(am->label, "Q0"));
    int interesting = !is_acars || p->next != NULL;
    if ((is_err && !arg_showerrs) || (is_ping && arg_nopings)) { la_proto_tree_destroy(p); return; }
    char ts[40];
    iso_utc_seconds(q->time, ts, sizeof(ts));
    if (arg_json) {
        /* {app, source, timestamp, link_direction,
         *  acars: json.loads(o.json())['acars']} through json.dumps */
        la_vstring *v = la_proto_tree_format_json(NULL, p);
        const char *jp = v && v->str ? v->str : "";
        jv_t *root = js_value(&jp);
        const jv_t *acars = NULL;
        if (root && root->t == J_OBJ) {
            static const unsigned k[] = { 'a', 'c', 'a', 'r', 's' };
            for (size_t i = 0; i < root->n; i++) if (cp_eq(root->kcp[i], root->kcpn[i], k, 5)) acars = root->items[i];
        }
        if (acars) {                     /* (no "acars" key: KeyError in Python) */
            sb_t o = { 0 };
            sb_puts(&o, "{\"app\": {\"name\": \"iridium-toolkit\", \"version\": \"0.0.2\"}, "
                        "\"source\": {\"transport\": \"iridium\", \"parser\": \"libacars\", \"version\": ");
            put_json_str(&o, (const unsigned char *)LA_VERSION, strlen(LA_VERSION));
            sb_printf(&o, "}, \"timestamp\": \"%s\", \"link_direction\": \"%s\", \"acars\": ", ts, q->ul ? "uplink" : "downlink");
            jv_dump(&o, acars);
            sb_puts(&o, "}");
            sb_writeln(&o, out);
            free(o.s);
        }
        jv_free(root);
        if (v) la_vstring_destroy(v, true);
        la_proto_tree_destroy(p);
        return;
    }
    fprintf(out, "%s %s ", ts, q->ul ? "UL" : "DL");
    if (q->data.d[1] == 0x3) {
        sb_t h = { 0 };
        bytes_t hh = b_slice(q->data, 1, 9);
        put_hex(&h, hh, NULL);
        fprintf(out, "[hdr: %s] ", sb_str(&h));
        free(h.s); b_free(&hh);
    }
    if (interesting) fputs("INTERESTING ", out);
    la_vstring *v = la_proto_tree_format_text(NULL, p);
    fprintf(out, "%s\n", v && v->str ? v->str : "");
    if (v) la_vstring_destroy(v, true);
    la_proto_tree_destroy(p);
}
#endif

static void consume_l2(sbdobj_t *p)
{
#ifdef HAVE_LIBACARS
    if (mode == TKR_LIBACARS) { libacars_consume_l2(p); return; }
#endif
    if (mode == TKR_ACARS) acars_consume_l2(p);
    else sbd_consume_l2(p);
}

static void sbd_free(sbdobj_t *p) { if (!p) return; free(p->typ); b_free(&p->prehdr); b_free(&p->data); free(p); }

/* sbd.ReassembleIDASBD.process_l2; returns a packet to consume, or NULL */
static sbdobj_t *process_l2(bytes_t data, double time, int ul)
{
    if (data.n < 5) return NULL;
    if (data.d[0] == 0x76 && data.d[1] != 5) ;
    else if (data.d[0] == 0x06 && data.d[1] == 0) ;
    else return NULL;
    if (data.d[0] == 0x76) {
        if (ul) { if (data.d[1] < 0x0c || data.d[1] > 0x0e) { fprintf(stderr, "WARN: SBD: ul pkt with unclear type\n"); return NULL; } }
        else if (data.d[1] < 0x08 || data.d[1] > 0x0b) { fprintf(stderr, "WARN: SBD: dl pkt with unclear type\n"); return NULL; }
    }
    if (data.d[0] == 0x06) {
        int s = data.d[2];
        if (!(s == 0x00 || s == 0x10 || s == 0x20 || s == 0x40 || s == 0x50 || s == 0x70)) {
            fprintf(stderr, "WARN: SBD: HELLO pkt with unknown sub-type\n");
            return NULL;
        }
    }
    sbd_cnt++;
    char typ[8];
    snprintf(typ, sizeof(typ), "%02x%02x", data.d[0], data.d[1]);
    bytes_t d = b_slice(data, 2, (long)data.n), prehdr = { NULL, 0 }, hdr = { NULL, 0 };
    int msgcnt, msgno;
    if (!strcmp(typ, "0600")) {
        if (d.d[0] != 0x20) { b_free(&d); return NULL; }
        prehdr = b_slice(d, 0, 29);
        bytes_t d2 = b_slice(d, 29, (long)d.n); b_free(&d); d = d2;
        msgcnt = prehdr.n > 15 ? prehdr.d[15] : 0;    /* IndexError in Python if short */
        msgno = msgcnt == 0 ? 0 : 1;
    } else {
        if (!strcmp(typ, "7608")) {
            long cut = d.d[0] == 0x26 ? 7 : d.d[0] == 0x20 ? 5 : 7;
            if (d.d[0] != 0x26 && d.d[0] != 0x20) fprintf(stderr, "WARN: SBD: DL pkt with unclear header\n");
            prehdr = b_slice(d, 0, cut);
            bytes_t d2 = b_slice(d, cut, (long)d.n); b_free(&d); d = d2;
            msgcnt = prehdr.n > 3 ? prehdr.d[3] : 0;
        } else { prehdr = b_new(NULL, 0); msgcnt = -1; }
        if (ul && d.n >= 3 && (d.d[0] == 0x50 || d.d[0] == 0x51)) {
            b_free(&prehdr);
            prehdr = b_slice(d, 0, 3);
            bytes_t d2 = b_slice(d, 3, (long)d.n); b_free(&d); d = d2;
        }
        if (d.n == 0) { hdr = b_new(NULL, 0); msgno = 0; }
        else if (d.n > 3 && d.d[0] == 0x10) {
            hdr = b_slice(d, 0, 3);
            bytes_t d2 = b_slice(d, 3, (long)d.n); b_free(&d); d = d2;
            msgno = hdr.d[2];
            if (d.n < hdr.d[1]) { b_free(&d); b_free(&prehdr); b_free(&hdr); return NULL; }
            else if (d.n > hdr.d[1]) d.n = hdr.d[1];
        } else {
            hdr = b_new(NULL, 0); msgno = 0;
            fprintf(stderr, "WARN: SBD: Data packet without header?\n");
        }
    }
    b_free(&hdr);
    sbdobj_t *pkt = calloc(1, sizeof(*pkt));
    pkt->typ = strdup(typ); pkt->time = time; pkt->ul = ul; pkt->prehdr = prehdr; pkt->data = d;

    for (int i = nmulti - 1; i >= 0; i--)
        if (multi[i].t + 5 < time) {
            sbd_broken++;
            sbd_free(multi[i].p);
            memmove(&multi[i], &multi[i + 1], sizeof(multi_t) * (size_t)(nmulti - i - 1));
            nmulti--;
        }
    if (msgno == 0) { sbd_short++; return pkt; }
    else if (msgcnt == 1 && msgno == 1) { sbd_single++; return pkt; }
    else if (msgcnt > 1) {
        if (nmulti == capmulti) { capmulti = capmulti ? capmulti * 2 : 16; multi = realloc(multi, sizeof(multi_t) * (size_t)capmulti); }
        multi[nmulti++] = (multi_t){ msgno, msgcnt, pkt, time };
        sbd_assembled++;
        return NULL;
    } else if (msgno > 1) {
        for (int i = nmulti - 1; i >= 0; i--) {
            multi_t *mm = &multi[i];
            if (msgno == mm->no + 1 && msgno < mm->cnt && mm->p->ul == ul) {
                b_append(&mm->p->data, d);
                size_t l = strlen(mm->p->typ);
                mm->p->typ = realloc(mm->p->typ, l + strlen(typ) + 1);
                strcpy(mm->p->typ + l, typ);
                mm->no++;
                sbd_assembled++;
                sbd_free(pkt);
                return NULL;
            } else if (msgno == mm->no + 1 && msgno == mm->cnt && mm->p->ul == ul) {
                sbdobj_t *p = mm->p;
                b_append(&p->data, d);
                size_t l = strlen(p->typ);
                p->typ = realloc(p->typ, l + strlen(typ) + 1);
                strcpy(p->typ + l, typ);
                memmove(&multi[i], &multi[i + 1], sizeof(multi_t) * (size_t)(nmulti - i - 1));
                nmulti--;
                sbd_assembled++;
                sbd_multi++;
                sbd_free(pkt);
                return p;
            }
        }
        sbd_broken++;
        sbd_free(pkt);
        return NULL;
    }
    sbd_free(pkt);                        /* "Shouldn't happen" raises in Python */
    return NULL;
}

/* ------------------------------------------------------------------ IDA */

/* ReassembleIDA.consume (mode "ida") */
static void ida_consume(bytes_t data, double time, int ul, long freq)
{
    char ch[32];
    channelize_str(freq, ch, sizeof(ch));
    sb_t h = { 0 }, a = { 0 };
    put_hex(&h, data, " ");
    put_ascii(&a, data, 1, 0);
    fprintf(out, "%15.6f %s %s %s | %s\n", time, ch, ul ? "UL" : "DL", sb_str(&h), sb_str(&a));
    free(h.s); free(a.s);
}

static void consume(bytes_t data, double time, int ul, double level, long freq)
{
    (void)level;
    if (mode == TKR_IDA) { ida_consume(data, time, ul, freq); return; }
    sbdobj_t *p = process_l2(data, time, ul);
    if (p) { consume_l2(p); sbd_free(p); }
}

/* bytes().fromhex(dat.replace('.',' ').replace('!',' ')) */
static bytes_t from_dotted_hex(const char *s)
{
    size_t n = strlen(s);
    unsigned char *d = malloc(n / 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i + 1 < n + 1;) {
        if (s[i] == '.' || s[i] == '!' || s[i] == ' ') { i++; continue; }
        if (!s[i] || !s[i + 1]) break;
        unsigned v;
        if (sscanf(s + i, "%2x", &v) != 1) break;
        d[k++] = (unsigned char)v;
        i += 2;
    }
    bytes_t b = { d, k };
    return b;
}

/* ReassembleIDA.process for one filtered, enriched line */
static void ida_process(double time, long freq, double level, int ul, int cont, int ctr, const char *data)
{
    if (otime - 1 <= time && time <= otime + 1 && odata && !strcmp(odata, data) && ofreq - 200 < freq && freq < ofreq + 200) {
        stat_dupes++;
        return;
    }
    otime = time; free(odata); odata = strdup(data); ofreq = freq; olevel = level;

    int ok = 0;
    for (int i = 0; i < nbuf; i++) {
        idabuf_t *e = &buf[i];
        if (e->freq - 260 < freq && freq < e->freq + 260 && e->time[e->ntime - 1] <= time && time <= e->time[e->ntime - 1] + 280 &&
            (e->ctr + 1) % 8 == ctr && e->ul == ul) {
            idabuf_t x = *e;
            memmove(&buf[i], &buf[i + 1], sizeof(idabuf_t) * (size_t)(nbuf - i - 1));
            nbuf--;
            size_t l = strlen(x.dat);
            x.dat = realloc(x.dat, l + 1 + strlen(data) + 1);
            x.dat[l] = '.'; strcpy(x.dat + l + 1, data);
            x.time = realloc(x.time, sizeof(double) * (size_t)(x.ntime + 1));
            x.time[x.ntime++] = time;
            if (cont) {
                x.freq = freq; x.ctr = ctr; x.cont = cont; x.ul = ul;
                if (nbuf == capbuf) { capbuf = capbuf ? capbuf * 2 : 16; buf = realloc(buf, sizeof(idabuf_t) * (size_t)capbuf); }
                buf[nbuf++] = x;
            } else {
                stat_ok++;
                bytes_t b = from_dotted_hex(x.dat);
                consume(b, time, x.ul, level, x.freq);
                b_free(&b);
                free(x.dat); free(x.time);
                return;
            }
            stat_fragments++;
            ok = 1;
            break;
        }
    }
    if (ok) ;
    else if (ctr == 0 && !cont) {
        bytes_t b = from_dotted_hex(data);
        consume(b, time, ul, level, freq);
        b_free(&b);
        return;
    } else if (ctr == 0 && cont) {
        stat_fragments++;
        if (nbuf == capbuf) { capbuf = capbuf ? capbuf * 2 : 16; buf = realloc(buf, sizeof(idabuf_t) * (size_t)capbuf); }
        idabuf_t x = { freq, malloc(sizeof(double)), 1, ctr, strdup(data), cont, ul };
        x.time[0] = time;
        buf[nbuf++] = x;
    } else if (ctr > 0) {
        stat_broken++;
        stat_fragments++;
    }
    /* expire packets (the first one found) */
    for (int i = 0; i < nbuf; i++)
        if (buf[i].time[buf[i].ntime - 1] + 1000 <= time) {
            stat_broken++;
            free(buf[i].dat); free(buf[i].time);
            memmove(&buf[i], &buf[i + 1], sizeof(idabuf_t) * (size_t)(nbuf - i - 1));
            nbuf--;
            break;
        }
}

/* Reassemble.filter + ReassembleIDA.filter + MyObject.enrich, then process */
void tkr_line(const char *line_in)
{
    stat_line++;
    /* line.split(None, 8) */
    const char *f[9];
    size_t fl[9];
    const char *p = line_in;
    int nf = 0;
    while (nf < 9) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        f[nf] = p;
        if (nf == 8) { fl[nf] = strlen(p); nf++; break; }
        while (*p && !isspace((unsigned char)*p)) p++;
        fl[nf] = (size_t)(p - f[nf]);
        nf++;
    }
    if (nf != 9) { fprintf(stderr, "Couldn't parse input line: %s", line_in); return; }
    if (!(fl[0] == 4 && !strncmp(f[0], "IDA:", 4))) return;
    char *data = strndup(f[8], fl[8]);
    size_t dl = strlen(data);
    while (dl && (data[dl - 1] == '\n' || data[dl - 1] == '\r')) data[--dl] = 0;
    if (!strstr(data, " CRC:OK")) { free(data); return; }
    regmatch_t mt[6];
    if (regexec(&re_ida, data, 6, mt, 0) != 0) { fprintf(stderr, "Couldn't parse IDA: %s\n", data); free(data); return; }
    int f1 = data[mt[1].rm_so] == '1';
    int ctr = (int)strtol(data + mt[3].rm_so, NULL, 2);
    char *hex = strndup(data + mt[5].rm_so, (size_t)(mt[5].rm_eo - mt[5].rm_so));
    int ul = fl[7] == 2 && !strncmp(f[7], "UL", 2);
    /* enrich */
    char *freqs = strndup(f[3], fl[3]);
    long freq = strtol(freqs, NULL, 10);
    char *name = strndup(f[1], fl[1]);
    double starttime = 0;
    if (strlen(name) > 3 && name[1] == '-') starttime = strtod(name + 2, NULL);
    char *ms = strndup(f[2], fl[2]);
    double mstime = strtod(ms, NULL);
    char *lv = strndup(f[5], fl[5]);
    double level = strtod(lv, NULL);             /* "a|b|c": level = a */
    double time = starttime + mstime / 1000;
    stat_filter++;
    ida_process(time, freq, level, ul, f1, ctr, hex);
    free(freqs); free(name); free(ms); free(lv); free(hex); free(data);
}

void tkr_end(void)
{
    if (stat_line > 0) fprintf(out, "Kept %ld/%ld (%3.1f%%) lines\n", stat_filter, stat_line, 100.0 * stat_filter / stat_line);
    else fprintf(out, "No lines?\n");
    fprintf(out, "%ld valid packets assembled from %ld fragments (1:%1.2f).\n", stat_ok, stat_fragments,
            (double)stat_fragments / (stat_ok ? stat_ok : 1));
    fprintf(out, "%ld/%ld (%3.1f%%) broken fragments.\n", stat_broken, stat_fragments,
            100.0 * stat_broken / (stat_fragments ? stat_fragments : 1));
    fprintf(out, "%ld dupes removed.\n", stat_dupes);
    if (mode == TKR_SBD || mode == TKR_ACARS || mode == TKR_LIBACARS) {
        fprintf(out, "SBD: %ld short & %ld single messages. (%1.1f%%).\n", sbd_short, sbd_single,
                100 * (double)(sbd_short + sbd_single) / (sbd_cnt ? sbd_cnt : 1));
        fprintf(out, "SBD: %ld successful multi-pkt messages.\n", sbd_multi);
        fprintf(out, "SBD: %ld/%ld fragments could not be assembled. (%1.1f%%).\n", sbd_broken, sbd_assembled,
                100 * (double)sbd_broken / (sbd_assembled ? sbd_assembled : 1));
    }
    fflush(out);
}
