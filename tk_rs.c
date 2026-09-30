/*
 * Reed-Solomon decoding as iridium-toolkit uses it (rs.py / rs6.py on its
 * bundled reedsolo.py / reedsolo6.py), and its CRC-24 and checksum_16.
 *
 * reedsolo.py: Copyright (c) 2012-2015 Tomer Filiba, (c) 2015 rotorgit,
 * (c) 2015 Stephen Larroque; https://github.com/tomerfiliba/reedsolomon/,
 * public domain. Ported from the copy in iridium-toolkit (commit 8888124),
 * (c) Sec & schneider, 2-Clause BSD License - see
 * LICENSES/BSD-2-Clause-iridium-toolkit.txt
 *
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tk_rs.h"

#include <pthread.h>
#include <string.h>

/* One Galois field: reedsolo.py keeps these as module globals, and rs.py /
 * rs6.py each have their own module, so there are two. */
typedef struct {
    int charac;                    /* field_charac = 2**c_exp - 1 */
    int exp[2 * 255];
    int log[256];
} gf_t;

static gf_t gf8, gf6;
static pthread_once_t gf_once = PTHREAD_ONCE_INIT;

/* reedsolo.gf_mult_noLUT (carryless) */
static int gf_mult_noLUT(int x, int y, int prim, int field_charac_full)
{
    int r = 0;
    while (y) {
        if (y & 1) r ^= x;
        y >>= 1;
        x <<= 1;
        if (prim > 0 && (x & field_charac_full)) x ^= prim;
    }
    return r;
}

/* reedsolo.init_tables */
static void init_tables(gf_t *g, int prim, int generator, int c_exp)
{
    g->charac = (1 << c_exp) - 1;
    memset(g->log, 0, sizeof(g->log));
    int x = 1;
    for (int i = 0; i < g->charac; i++) {
        g->exp[i] = x;
        g->log[x] = i;
        x = gf_mult_noLUT(x, generator, prim, g->charac + 1);
    }
    for (int i = g->charac; i < g->charac * 2; i++) g->exp[i] = g->exp[i - g->charac];
}

static void gf_init(void)
{
    init_tables(&gf8, 0x11d, 2, 8);   /* rs.py */
    init_tables(&gf6, 0x43, 2, 6);    /* rs6.py */
}

/* Python's % (never negative for a positive modulus) */
static int pmod(long a, int m) { long r = a % m; return (int)(r < 0 ? r + m : r); }

static int gf_inverse(const gf_t *g, int x) { return g->exp[g->charac - g->log[x]]; }
static int gf_mul(const gf_t *g, int x, int y)
{
    if (x == 0 || y == 0) return 0;
    return g->exp[(g->log[x] + g->log[y]) % g->charac];
}
/* returns -1 for ZeroDivisionError */
static int gf_div(const gf_t *g, int x, int y)
{
    if (y == 0) return -1;
    if (x == 0) return 0;
    return g->exp[(g->log[x] + g->charac - g->log[y]) % g->charac];
}
static int gf_pow(const gf_t *g, int x, long power) { return g->exp[pmod((long)g->log[x] * power, g->charac)]; }

/* Polynomials: arrays with a length, highest degree first (reedsolo's order). */
#define PMAX 512
typedef struct { int n; int c[PMAX]; } poly_t;

static void p_set(poly_t *p, const int *c, int n) { p->n = n; memcpy(p->c, c, sizeof(int) * (size_t)n); }

/* gf_poly_scale */
static void gf_poly_scale(const gf_t *g, const poly_t *p, int x, poly_t *r)
{
    poly_t t; t.n = p->n;
    for (int i = 0; i < p->n; i++) t.c[i] = gf_mul(g, p->c[i], x);
    *r = t;
}
/* gf_poly_add */
static void gf_poly_add(const poly_t *p, const poly_t *q, poly_t *r)
{
    poly_t t;
    t.n = p->n > q->n ? p->n : q->n;
    memset(t.c, 0, sizeof(int) * (size_t)t.n);
    for (int i = 0; i < p->n; i++) t.c[i + t.n - p->n] = p->c[i];
    for (int i = 0; i < q->n; i++) t.c[i + t.n - q->n] ^= q->c[i];
    *r = t;
}
/* gf_poly_mul */
static void gf_poly_mul(const gf_t *g, const poly_t *p, const poly_t *q, poly_t *r)
{
    poly_t t;
    t.n = p->n + q->n - 1;
    if (t.n < 0) t.n = 0;
    memset(t.c, 0, sizeof(int) * (size_t)t.n);
    for (int j = 0; j < q->n; j++) {
        int qj = q->c[j];
        if (qj == 0) continue;
        int lq = g->log[qj];
        for (int i = 0; i < p->n; i++)
            if (p->c[i] != 0) t.c[i + j] ^= g->exp[g->log[p->c[i]] + lq];
    }
    *r = t;
}
/* gf_poly_div -> remainder only (msg_out[separator:], Python slicing) */
static void gf_poly_div_rem(const gf_t *g, const poly_t *dividend, const poly_t *divisor, poly_t *rem)
{
    poly_t m = *dividend;
    for (int i = 0; i < dividend->n - (divisor->n - 1); i++) {
        int coef = m.c[i];
        if (coef != 0)
            for (int j = 1; j < divisor->n; j++)
                if (divisor->c[j] != 0) m.c[i + j] ^= gf_mul(g, divisor->c[j], coef);
    }
    int k = divisor->n - 1;               /* msg_out[-k:]; k == 0 -> msg_out[0:] */
    int from = k == 0 ? 0 : (k > m.n ? 0 : m.n - k);
    rem->n = m.n - from;
    memmove(rem->c, m.c + from, sizeof(int) * (size_t)rem->n);
}
/* gf_poly_eval; returns -1 for an empty polynomial (IndexError in Python) */
static int gf_poly_eval(const gf_t *g, const poly_t *p, int x)
{
    if (p->n == 0) return -1;
    int y = p->c[0];
    for (int i = 1; i < p->n; i++) y = gf_mul(g, y, x) ^ p->c[i];
    return y;
}
static void p_reverse(poly_t *p) { for (int i = 0, j = p->n - 1; i < j; i++, j--) { int t = p->c[i]; p->c[i] = p->c[j]; p->c[j] = t; } }

/* rs_calc_syndromes: [0] + evaluations */
static int rs_calc_syndromes(const gf_t *g, const poly_t *msg, int nsym, int fcr, int generator, poly_t *synd)
{
    synd->n = nsym + 1;
    synd->c[0] = 0;
    for (int i = 0; i < nsym; i++) {
        int v = gf_poly_eval(g, msg, gf_pow(g, generator, i + fcr));
        if (v < 0) return -1;
        synd->c[i + 1] = v;
    }
    return 0;
}

/* rs_find_errata_locator */
static void rs_find_errata_locator(const gf_t *g, const int *e_pos, int ne, int generator, poly_t *e_loc)
{
    e_loc->n = 1; e_loc->c[0] = 1;
    for (int i = 0; i < ne; i++) {
        poly_t one = { 1, { 1 } }, t, f;
        t.n = 2; t.c[0] = gf_pow(g, generator, e_pos[i]); t.c[1] = 0;
        gf_poly_add(&one, &t, &f);
        gf_poly_mul(g, e_loc, &f, e_loc);
    }
}

/* rs_find_error_evaluator */
static void rs_find_error_evaluator(const gf_t *g, const poly_t *synd, const poly_t *err_loc, int nsym, poly_t *rem)
{
    poly_t prod, div;
    gf_poly_mul(g, synd, err_loc, &prod);
    div.n = nsym + 2;
    memset(div.c, 0, sizeof(int) * (size_t)div.n);
    div.c[0] = 1;
    gf_poly_div_rem(g, &prod, &div, rem);
}

/* rs_correct_errata; returns -1 for ZeroDivisionError */
static int rs_correct_errata(const gf_t *g, poly_t *msg, const poly_t *synd, const int *err_pos, int npos, int fcr, int generator)
{
    int coef_pos[PMAX];
    for (int i = 0; i < npos; i++) coef_pos[i] = msg->n - 1 - err_pos[i];
    poly_t err_loc, sr, err_eval;
    rs_find_errata_locator(g, coef_pos, npos, generator, &err_loc);
    sr = *synd; p_reverse(&sr);
    rs_find_error_evaluator(g, &sr, &err_loc, err_loc.n - 1, &err_eval);
    p_reverse(&err_eval);

    int X[PMAX];
    for (int i = 0; i < npos; i++) {
        long l = g->charac - coef_pos[i];
        X[i] = gf_pow(g, generator, -l);
    }
    poly_t E; E.n = msg->n; memset(E.c, 0, sizeof(int) * (size_t)E.n);
    poly_t ee = err_eval; p_reverse(&ee);
    for (int i = 0; i < npos; i++) {
        int Xi_inv = gf_inverse(g, X[i]);
        int err_loc_prime = 1;
        for (int j = 0; j < npos; j++)
            if (j != i) err_loc_prime = gf_mul(g, err_loc_prime, 1 ^ gf_mul(g, Xi_inv, X[j]));
        int y = gf_poly_eval(g, &ee, Xi_inv);
        if (y < 0) return -2;
        y = gf_mul(g, gf_pow(g, X[i], 1 - fcr), y);
        int mag = gf_div(g, y, err_loc_prime);
        if (mag < 0) return -1;
        E.c[err_pos[i]] = mag;
    }
    gf_poly_add(msg, &E, msg);
    return 0;
}

/* rs_find_error_locator; returns -1 for "Too many errors to correct" */
static int rs_find_error_locator(const gf_t *g, const poly_t *synd, int nsym, int erase_count, poly_t *out)
{
    poly_t err_loc = { 1, { 1 } }, old_loc = { 1, { 1 } };
    int synd_shift = synd->n > nsym ? synd->n - nsym : 0;
    for (int i = 0; i < nsym - erase_count; i++) {
        int K = i + synd_shift;
        if (K >= synd->n) return -2;       /* IndexError in Python */
        int delta = synd->c[K];
        for (int j = 1; j < err_loc.n; j++) {
            if (K - j < 0) return -2;
            delta ^= gf_mul(g, err_loc.c[err_loc.n - 1 - j], synd->c[K - j]);
        }
        old_loc.c[old_loc.n++] = 0;
        if (delta != 0) {
            if (old_loc.n > err_loc.n) {
                poly_t new_loc;
                gf_poly_scale(g, &old_loc, delta, &new_loc);
                gf_poly_scale(g, &err_loc, gf_inverse(g, delta), &old_loc);
                err_loc = new_loc;
            }
            poly_t s;
            gf_poly_scale(g, &old_loc, delta, &s);
            gf_poly_add(&err_loc, &s, &err_loc);
        }
    }
    int k = 0;
    while (k < err_loc.n && err_loc.c[k] == 0) k++;
    out->n = err_loc.n - k;
    memmove(out->c, err_loc.c + k, sizeof(int) * (size_t)out->n);
    int errs = out->n - 1;
    if ((errs - erase_count) * 2 + erase_count > nsym) return -1;
    return 0;
}

/* rs_find_errors; returns -1 for the Chien search mismatch */
static int rs_find_errors(const gf_t *g, const poly_t *err_loc, int nmess, int generator, int *err_pos, int *npos)
{
    int errs = err_loc->n - 1, k = 0;
    for (int i = 0; i < nmess; i++) {
        int v = gf_poly_eval(g, err_loc, gf_pow(g, generator, i));
        if (v < 0) return -2;
        if (v == 0) err_pos[k++] = nmess - 1 - i;
    }
    *npos = k;
    return k == errs ? 0 : -1;
}

/* rs_forney_syndromes */
static void rs_forney_syndromes(const gf_t *g, const poly_t *synd, const int *pos, int npos, int nmess, int generator, poly_t *fsynd)
{
    fsynd->n = synd->n - 1;
    memcpy(fsynd->c, synd->c + 1, sizeof(int) * (size_t)fsynd->n);
    for (int i = 0; i < npos; i++) {
        int x = gf_pow(g, generator, nmess - 1 - pos[i]);
        for (int j = 0; j < fsynd->n - 1; j++) fsynd->c[j] = gf_mul(g, fsynd->c[j], x) ^ fsynd->c[j + 1];
    }
}

static int p_max(const poly_t *p) { int m = 0; for (int i = 0; i < p->n; i++) if (p->c[i] > m) m = p->c[i]; return m; }

/* rs_correct_msg: 0 = corrected (msg_out = message + ecc), -1 = not
 * correctable (ReedSolomonError / ZeroDivisionError, which rs_fix catches),
 * -2 = another exception (the toolkit would stop there) */
static int rs_correct_msg(const gf_t *g, const int *msg_in, int n, int nsym, int fcr, int generator,
                          const int *erase_pos, int nerase, int *msg_out)
{
    if (n > g->charac) return -2;
    poly_t m; p_set(&m, msg_in, n);
    for (int i = 0; i < nerase; i++) m.c[erase_pos[i]] = 0;
    if (nerase > nsym) return -1;
    poly_t synd;
    if (rs_calc_syndromes(g, &m, nsym, fcr, generator, &synd) < 0) return -2;
    if (p_max(&synd) == 0) { memcpy(msg_out, m.c, sizeof(int) * (size_t)n); return 0; }
    poly_t fsynd, err_loc;
    rs_forney_syndromes(g, &synd, erase_pos, nerase, m.n, generator, &fsynd);
    int r = rs_find_error_locator(g, &fsynd, nsym, nerase, &err_loc);
    if (r) return r;
    p_reverse(&err_loc);
    int err_pos[PMAX], npos;
    r = rs_find_errors(g, &err_loc, m.n, generator, err_pos, &npos);
    if (r) return r;
    int all_pos[PMAX], nall = 0;
    for (int i = 0; i < nerase; i++) all_pos[nall++] = erase_pos[i];
    for (int i = 0; i < npos; i++) all_pos[nall++] = err_pos[i];
    r = rs_correct_errata(g, &m, &synd, all_pos, nall, fcr, generator);
    if (r) return r;
    if (rs_calc_syndromes(g, &m, nsym, fcr, generator, &synd) < 0) return -2;
    if (p_max(&synd) > 0) return -1;
    memcpy(msg_out, m.c, sizeof(int) * (size_t)n);
    return 0;
}

/* rs.rs_fix: data = 39 bytes; 8 erased ecc symbols appended; RS(47,31) */
int tk_rs8_fix(const int *data, int n, int *msg, int *csum)
{
    pthread_once(&gf_once, gf_init);
    const int nsym = 8, elen = 8;
    int d[PMAX], out[PMAX], er[8];
    if (n + elen > PMAX) return -2;
    memcpy(d, data, sizeof(int) * (size_t)n);
    for (int i = 0; i < elen; i++) { d[n + i] = 0; er[i] = n + i; }
    int r = rs_correct_msg(&gf8, d, n + elen, nsym + elen, 0, 2, er, elen, out);
    if (r) return r;
    int mlen = n + elen - (nsym + elen);  /* msg_out[:-nsym] */
    memcpy(msg, out, sizeof(int) * (size_t)mlen);
    memcpy(csum, out + mlen, sizeof(int) * (size_t)nsym);   /* crs[:nsym] */
    return mlen;
}

/* rs6.rs_fix: 52 six-bit symbols, no erasures; RS_6(52,10), fcr 54 */
int tk_rs6_fix(const int *data, int n, int *msg, int *csum)
{
    pthread_once(&gf_once, gf_init);
    const int nsym = 10;
    int out[PMAX];
    int r = rs_correct_msg(&gf6, data, n, nsym, 54, 2, NULL, 0, out);
    if (r) return r;
    int mlen = n - nsym;
    memcpy(msg, out, sizeof(int) * (size_t)mlen);
    memcpy(csum, out + mlen, sizeof(int) * (size_t)nsym);
    return mlen;
}

/* bitsparser.iip_crc24: crcmod.mkCrcFun(poly=0x1BBA1B5,
 * initCrc=0xffffff^0x0c91b6, rev=True, xorOut=0x0c91b6). crcmod's initCrc
 * already has xorOut folded in: the register starts at 0xffffff. */
unsigned tk_iip_crc24(const unsigned char *d, int n)
{
    /* the reflected polynomial of 0xBBA1B5 (24 bits) */
    unsigned rpoly = 0;
    for (int i = 0; i < 24; i++) if (0xBBA1B5u & (1u << i)) rpoly |= 1u << (23 - i);
    unsigned crc = 0xffffff;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ rpoly : crc >> 1;
    }
    return (crc ^ 0x0c91b6) & 0xffffff;
}

/* bitsparser.checksum_16: sum of struct.unpack("<14HBH", msg) (31 bytes),
 * folded, ^0xffff */
unsigned tk_checksum_16(const int *m)
{
    unsigned long s = 0;
    for (int i = 0; i < 14; i++) s += (unsigned)(m[2 * i] | (m[2 * i + 1] << 8));
    s += (unsigned)m[28];
    s += (unsigned)(m[29] | (m[30] << 8));
    s = (s & 0xffff) + (s >> 16);
    return (unsigned)(s ^ 0xffff);
}
