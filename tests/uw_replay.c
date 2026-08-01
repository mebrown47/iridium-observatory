/*
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * uw_replay.c - Offline UW-fail characterization harness
 *
 * Replays saved post-downmix burst frames (.cf32, 250 kHz / 10 sps, extracted
 * starting at the correlator's uw_start) through the exact qpsk_demod inner
 * logic, then sweeps one recovery knob at a time to answer: among bursts that
 * PASSED burst_downmix correlation but FAILED the qpsk_demod UW re-check, which
 * factor is responsible and what fraction is recoverable?
 *
 * The decimate/PLL/hard-decision/UW code below is copied verbatim from
 * qpsk_demod.c so the BASELINE reproduces production behaviour. Built-in oracle:
 * baseline must PASS the _DL files and FAIL the _UN files. If it reproduces
 * those ground-truth labels, the replay is faithful.
 *
 * Build:  gcc -O2 -o tests/uw_replay tests/uw_replay.c -lm
 * Run:    tests/uw_replay burst_sample
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "../iridium.h"

/* ---- constants copied from qpsk_demod.c ---- */
#define PLL_ALPHA    0.2f
#define M_SQRT1_2f   0.70710678118654752f
#define GARDNER_KP   0.02f
#define GARDNER_KI   0.0002f
#define UW_MAX_ERRORS 2

#define SPS          10.0f          /* 250 kHz / 25000 sym/s */
#define OUT_RATE     250000.0f
#define MAXSYM       4096

/* ---- cubic interpolation (verbatim) ---- */
static float complex cubic_interp(const float complex *in, int n_samples, float pos)
{
    int idx = (int)pos;
    float mu = pos - idx;
    if (idx < 1) idx = 1;
    if (idx >= n_samples - 2) idx = n_samples - 3;
    float complex s0 = in[idx-1], s1 = in[idx], s2 = in[idx+1], s3 = in[idx+2];
    float mu2 = mu*mu, mu3 = mu2*mu;
    float complex a = -0.5f*s0 + 1.5f*s1 - 1.5f*s2 + 0.5f*s3;
    float complex b =      s0 - 2.5f*s1 + 2.0f*s2 - 0.5f*s3;
    float complex c = -0.5f*s0           + 0.5f*s2;
    float complex d =                 s1;
    return a*mu3 + b*mu2 + c*mu + d;
}

/* ---- Gardner timing recovery (verbatim), with configurable start phase ---- */
static int decimate_gardner(const float complex *in, int n_samples, float sps,
                            float start_pos, float complex *out)
{
    int n = 0;
    float pos = start_pos;
    float timing_offset = 0.0f;
    float complex prev_sym = 0;
    while (pos < n_samples - 3) {
        float complex on_time = cubic_interp(in, n_samples, pos);
        out[n] = on_time;
        if (n > 0) {
            float mid_pos = pos - sps * 0.5f;
            if (mid_pos >= 1.0f) {
                float complex mid = cubic_interp(in, n_samples, mid_pos);
                float complex diff = prev_sym - on_time;
                float error = crealf(diff * conjf(mid));
                if (error > 1.0f) error = 1.0f;
                if (error < -1.0f) error = -1.0f;
                timing_offset += GARDNER_KI * error;
                float adjust = GARDNER_KP * error + timing_offset;
                if (adjust > 0.5f) adjust = 0.5f;
                if (adjust < -0.5f) adjust = -0.5f;
                pos += adjust;
            }
        }
        prev_sym = on_time;
        n++;
        pos += sps;
        if (n >= MAXSYM) break;
    }
    return n;
}

/* ---- simple decimation (verbatim), with configurable start offset ---- */
static int decimate_simple(const float complex *in, int n_samples, float sps,
                           int start, float complex *out)
{
    int n = 0;
    for (int i = start; i < n_samples && n < MAXSYM; i += (int)sps)
        out[n++] = in[i];
    return n;
}

/* ---- first-order PLL (verbatim) ---- */
static void qpsk_pll(const float complex *in, float complex *out, int n, float alpha)
{
    float complex phi_hat = 1.0f + 0.0f*I;
    for (int i = 0; i < n; i++) {
        out[i] = in[i] * phi_hat;
        float complex x_hat;
        float re = crealf(out[i]), im = cimagf(out[i]);
        if (re >= 0 && im >= 0)      x_hat =  M_SQRT1_2f + M_SQRT1_2f*I;
        else if (re >= 0)            x_hat =  M_SQRT1_2f - M_SQRT1_2f*I;
        else if (im < 0)             x_hat = -M_SQRT1_2f - M_SQRT1_2f*I;
        else                         x_hat = -M_SQRT1_2f + M_SQRT1_2f*I;
        float complex er = conjf(x_hat) * out[i];
        float er_mag = cabsf(er);
        if (er_mag < 1e-10f) continue;
        float complex phi_hat_t = er / er_mag;
        float angle = cargf(phi_hat_t);
        float scaled = alpha * angle;
        float complex corr = cosf(scaled) + sinf(scaled)*I;
        phi_hat = conjf(corr) * phi_hat;
        float m = cabsf(phi_hat);
        if (m > 0) phi_hat /= m;
    }
}

/* ---- hard-decision quadrant mapping (verbatim from demod_qpsk) ---- */
static void hard_decision(const float complex *pll, int n, int *symbols)
{
    for (int i = 0; i < n; i++) {
        float re = crealf(pll[i]), im = cimagf(pll[i]);
        if (re >= 0 && im >= 0)      symbols[i] = 0;
        else if (re < 0 && im >= 0)  symbols[i] = 1;
        else if (re < 0)             symbols[i] = 2;
        else                         symbols[i] = 3;
    }
}

/* ---- instrumented UW distance: like check_sync_word but returns Hamming ---- */
static int uw_hamming(const int *symbols, int n, const int *uw, int rot)
{
    if (n < IR_UW_LENGTH) return 999;
    int diffs = 0;
    for (int i = 0; i < IR_UW_LENGTH; i++) {
        int s = (symbols[i] + rot) & 3;
        int diff = abs(s - uw[i]);
        if (diff == 3) diff = 1;
        diffs += diff;
    }
    return diffs;
}

/* best UW distance over the two directions for a given rotation */
static int uw_best(const int *symbols, int n, int rot, int *dir_out)
{
    int dl = uw_hamming(symbols, n, IR_UW_DL, rot);
    int ul = uw_hamming(symbols, n, IR_UW_UL, rot);
    if (dl <= ul) { if (dir_out) *dir_out = 0; return dl; }
    if (dir_out) *dir_out = 1;
    return ul;
}

/* ---- normalized phase-coherent matched-filter sync score in [0,1] ----
   Mirrors what a correlate_sync acceptance threshold would see: slide the known
   UW over the (Gardner-decimated, PLL'd) frame, best of DL/UL, peak over offset,
   energy-normalized so it is scale-invariant. Real UW -> near 1; noise -> low. */
static float sync_score(const float complex *samples, int nsamp)
{
    static float complex dec[MAXSYM], pll[MAXSYM];
    int n = decimate_gardner(samples, nsamp, SPS, 0.0f, dec);
    qpsk_pll(dec, pll, n, PLL_ALPHA);
    if (n < IR_UW_LENGTH) return 0.0f;

    float complex refdl[IR_UW_LENGTH], reful[IR_UW_LENGTH];
    for (int i = 0; i < IR_UW_LENGTH; i++) {
        float pd = (float)M_PI*0.25f + IR_UW_DL[i]*(float)M_PI*0.5f;
        float pu = (float)M_PI*0.25f + IR_UW_UL[i]*(float)M_PI*0.5f;
        refdl[i] = cosf(pd) + sinf(pd)*I;
        reful[i] = cosf(pu) + sinf(pu)*I;
    }
    float best = 0.0f;
    for (int o = 0; o + IR_UW_LENGTH <= n; o++) {
        float complex accd = 0, accu = 0;
        float e = 0;
        for (int i = 0; i < IR_UW_LENGTH; i++) {
            float complex x = pll[o+i];
            accd += conjf(refdl[i]) * x;
            accu += conjf(reful[i]) * x;
            e += crealf(x)*crealf(x) + cimagf(x)*cimagf(x);
        }
        float den = sqrtf((float)IR_UW_LENGTH) * sqrtf(e);
        if (den < 1e-9f) continue;
        float sd = cabsf(accd) / den, su = cabsf(accu) / den;
        float s = sd > su ? sd : su;
        if (s > best) best = s;
    }
    return best;
}

/* ---- apply a constant CFO (Hz) at sample rate ---- */
static void apply_cfo(const float complex *in, float complex *out, int n, float df_hz)
{
    float w = 2.0f * (float)M_PI * df_hz / OUT_RATE;
    float complex rot = 1.0f, step = cosf(w) + sinf(w)*I;
    for (int i = 0; i < n; i++) {
        out[i] = in[i] * rot;
        rot *= step;
        rot /= cabsf(rot);
    }
}

/* ---- one demod pass -> returns best UW Hamming over allowed rotations ---- */
static int demod_pass(const float complex *samples, int nsamp,
                      int use_gardner, int start_off, float cfo_hz,
                      int allow_rot, int *dir_out, int *rot_out)
{
    static float complex tmp[8192];
    static float complex dec[MAXSYM];
    static float complex pll[MAXSYM];
    static int sym[MAXSYM];

    const float complex *src = samples;
    if (cfo_hz != 0.0f) {
        int m = nsamp < 8192 ? nsamp : 8192;
        apply_cfo(samples, tmp, m, cfo_hz);
        src = tmp;
        nsamp = m;
    }

    int n;
    if (use_gardner)
        n = decimate_gardner(src, nsamp, SPS, (float)start_off, dec);
    else
        n = decimate_simple(src, nsamp, SPS, start_off, dec);

    qpsk_pll(dec, pll, n, PLL_ALPHA);
    hard_decision(pll, n, sym);

    int best = 999, best_dir = 0, best_rot = 0;
    int rmax = allow_rot ? 4 : 1;
    for (int r = 0; r < rmax; r++) {
        int dir;
        int h = uw_best(sym, n, r, &dir);
        if (h < best) { best = h; best_dir = dir; best_rot = r; }
    }
    if (dir_out) *dir_out = best_dir;
    if (rot_out) *rot_out = best_rot;
    return best;
}

/* ---- definitive full-frame UW scan: slide UW over the whole burst, all
   rotations, both directions, simple 1-sps decimation from every start sample.
   Answers: is the DL/UL unique word present ANYWHERE, at any alignment? ---- */
static int full_frame_min_uw(const float complex *samples, int nsamp)
{
    static float complex dec[MAXSYM];
    static float complex pll[MAXSYM];
    static int sym[MAXSYM];
    int global_min = 999;
    /* try every decimation start phase/offset across the frame */
    for (int start = 0; start + (int)(IR_UW_LENGTH*SPS) < nsamp; start++) {
        int n = decimate_simple(samples, nsamp, SPS, start, dec);
        if (n < IR_UW_LENGTH) break;
        qpsk_pll(dec, pll, n, PLL_ALPHA);
        hard_decision(pll, n, sym);
        for (int r = 0; r < 4; r++) {
            int h = uw_best(sym, n, r, NULL);
            if (h < global_min) global_min = h;
        }
    }
    return global_min;
}

static int cmp_float(const void *a, const void *b) {
    float d = *(const float *)a - *(const float *)b;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* ---- file loading ---- */
static int load_cf32(const char *path, float complex *buf, int maxn)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = fread(buf, sizeof(float complex), maxn, f);
    fclose(f);
    return n;
}

typedef struct { char name[256]; int is_un; } entry_t;

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "burst_sample";
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "cannot open %s\n", dir); return 1; }

    /* Optional 2nd arg: process at most this many files (0/absent = all).
       Useful for a fast representative pass over a huge corpus.
       Env UW_NO_SCAN=1 skips the expensive full-frame UW slide. */
    long limit = (argc > 2) ? atol(argv[2]) : 0;
    int do_scan = (getenv("UW_NO_SCAN") == NULL);
    int verbose = (getenv("UW_VERBOSE") != NULL);

    int cap = 4096, nf = 0;
    entry_t *files = malloc(cap * sizeof(*files));
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        size_t L = strlen(nm);
        if (L < 8 || strcmp(nm + L - 5, ".cf32")) continue;
        int is_un;
        if (strstr(nm, "_UN.cf32")) is_un = 1;
        else if (strstr(nm, "_DL.cf32") || strstr(nm, "_UL.cf32")) is_un = 0;
        else continue;
        if (nf == cap) { cap *= 2; files = realloc(files, cap * sizeof(*files)); }
        files[nf].is_un = is_un;
        snprintf(files[nf].name, sizeof(files[nf].name), "%s/%s", dir, nm);
        nf++;
        if (limit && nf >= limit) break;
    }
    closedir(d);
    fprintf(stderr, "loaded %d files from %s%s%s\n", nf, dir,
            limit ? " (limited)" : "", do_scan ? "" : " [scan disabled]");

    static float complex samples[8192];

    int dl_total = 0, un_total = 0;
    int dl_base_pass = 0, un_base_pass = 0;
    int un_rot = 0, un_phase = 0, un_cfo = 0, un_any = 0;
    int dl_rot_falseflip = 0;
    int un_scan_found = 0, dl_scan_found = 0;
    float *dl_scores = malloc((nf ? nf : 1) * sizeof(float));
    float *un_scores = malloc((nf ? nf : 1) * sizeof(float));
    int ndl_s = 0, nun_s = 0;

    /* Per-file table is only useful for small corpora; suppress for scale runs
       unless UW_VERBOSE=1. Summary always prints. */
    int show_table = verbose || nf <= 200;
    if (show_table) {
        printf("=== per-UN-file recovery (baseline Hamming -> best achievable) ===\n");
        printf("%-46s %5s %5s %5s  %s\n", "file", "base", "best", "scan", "recovered-by");
    }

    for (int i = 0; i < nf; i++) {
        int n = load_cf32(files[i].name, samples, 8192);
        if (n < 100) continue;
        if (files[i].is_un) un_total++; else dl_total++;

        /* sync-strength discriminator (for the correlate_sync threshold) */
        float sc = sync_score(samples, n);
        if (files[i].is_un) un_scores[nun_s++] = sc; else dl_scores[ndl_s++] = sc;

        /* BASELINE: Gardner, start 0, no CFO, no rotation (production) */
        int base = demod_pass(samples, n, 1, 0, 0.0f, 0, NULL, NULL);
        int base_pass = base <= UW_MAX_ERRORS;

        if (files[i].is_un) { if (base_pass) un_base_pass++; }
        else                { if (base_pass) dl_base_pass++; }

        /* Experiment knobs (evaluated independently) */
        int h_rot   = demod_pass(samples, n, 1, 0, 0.0f, 1, NULL, NULL);
        /* start-phase sweep via simple decimator, 0..29 */
        int h_phase = 999;
        for (int s = 0; s < 30; s++) {
            int h = demod_pass(samples, n, 0, s, 0.0f, 0, NULL, NULL);
            if (h < h_phase) h_phase = h;
        }
        /* CFO sweep +/-40 kHz step 100 (covers full LEO Doppler), Gardner+rot */
        int h_cfo = 999, h_cfo_at = 0;
        for (int f = -40000; f <= 40000; f += 100) {
            int h = demod_pass(samples, n, 1, 0, (float)f, 1, NULL, NULL);
            if (h < h_cfo) { h_cfo = h; h_cfo_at = f; }
        }
        /* combined-best across all knobs (incl. rotation everywhere) */
        int best = base;
        if (h_rot < best) best = h_rot;
        if (h_phase < best) best = h_phase;
        if (h_cfo < best) best = h_cfo;
        /* full combined: cfo x phase x rotation */
        for (int f = -2500; f <= 2500; f += 500) {
            for (int s = 0; s < 20; s += 2) {
                int h = demod_pass(samples, n, 0, s, (float)f, 1, NULL, NULL);
                if (h < best) best = h;
            }
        }

        /* definitive: is the UW present anywhere in the frame? (slow; toggleable) */
        int scan = do_scan ? full_frame_min_uw(samples, n) : -1;
        if (do_scan) {
            if (files[i].is_un) { if (scan <= UW_MAX_ERRORS) un_scan_found++; }
            else                { if (scan <= UW_MAX_ERRORS) dl_scan_found++; }
        }

        if (files[i].is_un) {
            const char *by = "none";
            if (best <= UW_MAX_ERRORS) {
                un_any++;
                if (h_rot <= UW_MAX_ERRORS)        { by = "rotation"; un_rot++; }
                else if (h_phase <= UW_MAX_ERRORS) { by = "start-phase"; un_phase++; }
                else if (h_cfo <= UW_MAX_ERRORS)   { by = "cfo"; un_cfo++; }
                else                                by = "combined";
            }
            if (h_cfo <= UW_MAX_ERRORS && abs(h_cfo_at) > 3000)
                fprintf(stderr, "  [large-Doppler recovery] %s at %+d Hz\n",
                        files[i].name, h_cfo_at);
            if (h_phase <= UW_MAX_ERRORS && h_rot > UW_MAX_ERRORS) un_phase = un_phase; /* noop clarity */
            if (show_table) {
                const char *base_name = strrchr(files[i].name, '/');
                printf("%-46s %5d %5d %5d  %s\n", base_name ? base_name+1 : files[i].name,
                       base, best, scan, by);
            }
        } else {
            /* watch for DL files that only match under a nonzero rotation
               (would indicate the baseline absolute-phase assumption is fragile) */
            if (!base_pass && h_rot <= UW_MAX_ERRORS) dl_rot_falseflip++;
        }
    }

    printf("\n=== BASELINE fidelity (oracle check) ===\n");
    printf("DL files: %d, baseline PASS: %d  (expect ~all pass)\n", dl_total, dl_base_pass);
    printf("UN files: %d, baseline PASS: %d  (expect ~all fail = 0)\n", un_total, un_base_pass);

    printf("\n=== UN recovery by single knob (Hamming<=%d) ===\n", UW_MAX_ERRORS);
    printf("recovered by 4-quadrant rotation : %d / %d\n", un_rot, un_total);
    printf("recovered by start-phase sweep   : %d / %d\n", un_phase, un_total);
    printf("recovered by CFO sweep           : %d / %d\n", un_cfo, un_total);
    printf("recovered by ANY/combined        : %d / %d  (%.0f%%)\n",
           un_any, un_total, un_total ? 100.0*un_any/un_total : 0);

    if (do_scan) {
        printf("\n=== DEFINITIVE: is the UW present ANYWHERE in the burst? ===\n");
        printf("(full-frame slide, all start offsets x 4 rotations x DL/UL, Hamming<=%d)\n", UW_MAX_ERRORS);
        printf("DL files with UW found somewhere: %d / %d  (control, expect ~all)\n", dl_scan_found, dl_total);
        printf("UN files with UW found somewhere: %d / %d\n", un_scan_found, un_total);
    }

    printf("\n=== false-accept watch ===\n");
    printf("DL files needing a nonzero rotation to pass: %d (phase-ambiguity signal)\n",
           dl_rot_falseflip);

    /* ---- correlate_sync acceptance-threshold calibration ---- */
    printf("\n=== SYNC-STRENGTH SEPARABILITY (correlate_sync threshold) ===\n");
    printf("normalized matched-filter peak, [0,1]; DL=real, UN=false-positive candidate\n");
    if (ndl_s && nun_s) {
        /* percentiles via simple sort */
        qsort(dl_scores, ndl_s, sizeof(float), cmp_float);
        qsort(un_scores, nun_s, sizeof(float), cmp_float);
        float dl_p05 = dl_scores[ndl_s*5/100], dl_p50 = dl_scores[ndl_s/2];
        float un_p50 = un_scores[nun_s/2], un_p95 = un_scores[nun_s*95/100];
        printf("DL score: p05=%.3f  p50=%.3f  (want high)\n", dl_p05, dl_p50);
        printf("UN score: p50=%.3f  p95=%.3f  (want low)\n", un_p50, un_p95);
        printf("\n  thresh | DL kept (real retained) | UN kept (false pos left) \n");
        printf("  -------+-------------------------+--------------------------\n");
        for (float T = 0.35f; T <= 0.85f; T += 0.05f) {
            int dlk = 0, unk = 0;
            for (int i = 0; i < ndl_s; i++) if (dl_scores[i] >= T) dlk++;
            for (int i = 0; i < nun_s; i++) if (un_scores[i] >= T) unk++;
            printf("   %.2f  |  %5d/%5d  (%5.1f%%)   |  %5d/%5d  (%5.1f%%)\n",
                   T, dlk, ndl_s, 100.0*dlk/ndl_s, unk, nun_s, 100.0*unk/nun_s);
        }
        printf("\nPick the highest T where DL-kept stays ~>=99%% (protect real frames);\n");
        printf("UN-kept at that T = the false positives the threshold does NOT remove.\n");
    } else {
        printf("(need both DL and UN files to calibrate)\n");
    }

    return 0;
}
