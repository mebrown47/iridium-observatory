/*
 * JSONL archive of classified frames and decoded messages
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "archive.h"

/* Frame-type labels tracked in the per-minute rollup. Must cover every
 * label classify_frame_label() can return; unknown labels fall into
 * the last ("RAW") bucket rather than being dropped. */
static const char *type_names[] = {
    "IRA", "IBC", "MSG", "IDA", "VOC", "IIP", "IU3", "ISY", "IU6",
    "IDA_UL_FAIL", "ITL", "RAW"
};
#define N_TYPES ((int)(sizeof(type_names) / sizeof(type_names[0])))

#define MAX_ROLLUP_SATS 128

static struct {
    pthread_mutex_t lock;
    FILE *fp;
    char dir[512];
    int file_day;               /* YYYYMMDD of the open file */
    /* Current minute's rollup */
    time_t minute;              /* epoch seconds truncated to minute */
    unsigned counts[2][N_TYPES];/* [0]=DL, [1]=UL */
    unsigned uw_fail[2];
    struct { int sat_id; unsigned count; } sats[MAX_ROLLUP_SATS];
    int n_sats;
} A = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int archive_on = 0;

static int day_of(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* Open (append) the archive file for the given day. Caller holds lock. */
static int open_day(int day)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/iridium-%08d.jsonl", A.dir, day);
    FILE *fp = fopen(path, "a");
    if (!fp)
        return -1;
    if (A.fp)
        fclose(A.fp);
    A.fp = fp;
    A.file_day = day;
    return 0;
}

/* Rotate to today's file if the UTC day changed. Caller holds lock. */
static void rotate_if_needed(time_t now)
{
    int day = day_of(now);
    if (day != A.file_day && open_day(day) != 0)
        fprintf(stderr, "archive: cannot open file for day %d: %s\n",
                day, strerror(errno));
}

/* Emit the pending minute rollup line, if any. Caller holds lock. */
static void flush_rollup(void)
{
    if (A.minute == 0)
        return;
    int have = A.uw_fail[0] || A.uw_fail[1];
    for (int d = 0; d < 2 && !have; d++)
        for (int i = 0; i < N_TYPES && !have; i++)
            have = A.counts[d][i] != 0;
    if (have && A.fp) {
        fprintf(A.fp, "{\"e\":\"fmix\",\"t\":%llu",
                (unsigned long long)A.minute * 1000ULL);
        for (int d = 0; d < 2; d++) {
            int first = 1;
            for (int i = 0; i < N_TYPES; i++) {
                if (!A.counts[d][i])
                    continue;
                fprintf(A.fp, "%s\"%s\":%u",
                        first ? (d ? ",\"ul\":{" : ",\"dl\":{") : ",",
                        type_names[i], A.counts[d][i]);
                first = 0;
            }
            if (!first)
                fputc('}', A.fp);
        }
        fprintf(A.fp, ",\"uw\":[%u,%u]", A.uw_fail[0], A.uw_fail[1]);
        if (A.n_sats > 0) {
            fprintf(A.fp, ",\"sats\":{");
            for (int i = 0; i < A.n_sats; i++)
                fprintf(A.fp, "%s\"%d\":%u", i ? "," : "",
                        A.sats[i].sat_id, A.sats[i].count);
            fputc('}', A.fp);
        }
        fputs("}\n", A.fp);
        fflush(A.fp);
    }
    memset(A.counts, 0, sizeof(A.counts));
    A.uw_fail[0] = A.uw_fail[1] = 0;
    A.n_sats = 0;
    A.minute = 0;
}

/* Advance the rollup to the minute containing `now`, emitting the
 * previous minute's line when the boundary is crossed. Caller holds lock. */
static void tick(time_t now)
{
    time_t min = now - now % 60;
    if (A.minute != 0 && min != A.minute) {
        flush_rollup();
        rotate_if_needed(now);
    }
    if (A.minute == 0)
        A.minute = min;
}

/* Append a JSON-escaped string (without surrounding quotes). */
static void put_escaped(FILE *fp, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(fp, "\\%c", c);
        else if (c == '\n')
            fputs("\\n", fp);
        else if (c == '\r' || c == 0x7f)
            ; /* drop */
        else if (c < 0x20)
            fprintf(fp, "\\u%04x", c);
        else
            fputc(c, fp);
    }
}

int archive_init(const char *dir)
{
    if (!dir || !dir[0])
        dir = "archive";
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "archive: cannot create directory %s: %s\n",
                dir, strerror(errno));
        return -1;
    }
    pthread_mutex_lock(&A.lock);
    snprintf(A.dir, sizeof(A.dir), "%s", dir);
    time_t now = time(NULL);
    if (open_day(day_of(now)) != 0) {
        pthread_mutex_unlock(&A.lock);
        fprintf(stderr, "archive: cannot open archive file in %s: %s\n",
                dir, strerror(errno));
        return -1;
    }
    fprintf(A.fp, "{\"e\":\"start\",\"t\":%llu}\n",
            (unsigned long long)now * 1000ULL);
    fflush(A.fp);
    archive_on = 1;
    pthread_mutex_unlock(&A.lock);
    fprintf(stderr, "archive: writing to %s/iridium-%08d.jsonl\n",
            dir, A.file_day);
    return 0;
}

void archive_shutdown(void)
{
    pthread_mutex_lock(&A.lock);
    if (archive_on) {
        flush_rollup();
        if (A.fp) {
            fclose(A.fp);
            A.fp = NULL;
        }
        archive_on = 0;
    }
    pthread_mutex_unlock(&A.lock);
}

void archive_count_frame(const char *type, int uplink, int sat_id)
{
    if (!archive_on)
        return;
    pthread_mutex_lock(&A.lock);
    tick(time(NULL));
    int ti = N_TYPES - 1;   /* default: RAW bucket */
    for (int i = 0; i < N_TYPES; i++) {
        if (strcmp(type, type_names[i]) == 0) {
            ti = i;
            break;
        }
    }
    A.counts[uplink ? 1 : 0][ti]++;
    if (sat_id >= 0) {
        int i;
        for (i = 0; i < A.n_sats; i++)
            if (A.sats[i].sat_id == sat_id)
                break;
        if (i < A.n_sats) {
            A.sats[i].count++;
        } else if (A.n_sats < MAX_ROLLUP_SATS) {
            A.sats[A.n_sats].sat_id = sat_id;
            A.sats[A.n_sats].count = 1;
            A.n_sats++;
        }
    }
    pthread_mutex_unlock(&A.lock);
}

void archive_count_uw_fail(int uplink)
{
    if (!archive_on)
        return;
    pthread_mutex_lock(&A.lock);
    tick(time(NULL));
    A.uw_fail[uplink ? 1 : 0]++;
    pthread_mutex_unlock(&A.lock);
}

void archive_log_acars(uint64_t timestamp_ns, int uplink, double freq_hz,
                       const char *reg, const char *flight,
                       const char *label, const char *text,
                       const char *decoded_json)
{
    if (!archive_on)
        return;
    pthread_mutex_lock(&A.lock);
    tick(time(NULL));
    if (A.fp) {
        fprintf(A.fp, "{\"e\":\"acars\",\"t\":%llu,\"dir\":\"%s\","
                "\"freq\":%.0f,\"reg\":\"",
                (unsigned long long)(timestamp_ns / 1000000ULL),
                uplink ? "UL" : "DL", freq_hz);
        put_escaped(A.fp, reg ? reg : "");
        fputs("\",\"flt\":\"", A.fp);
        put_escaped(A.fp, flight ? flight : "");
        fputs("\",\"lbl\":\"", A.fp);
        put_escaped(A.fp, label ? label : "");
        fputs("\",\"txt\":\"", A.fp);
        put_escaped(A.fp, text ? text : "");
        fputc('"', A.fp);
        if (decoded_json && decoded_json[0] == '{') {
            fputs(",\"dec\":", A.fp);
            /* libacars JSON is a single-line object; write it verbatim
             * but defensively squash any newlines to keep JSONL valid. */
            for (const char *p = decoded_json; *p; p++)
                fputc((*p == '\n' || *p == '\r') ? ' ' : *p, A.fp);
        }
        fputs("}\n", A.fp);
        fflush(A.fp);
    }
    pthread_mutex_unlock(&A.lock);
}

void archive_log_pager(uint64_t timestamp_ns, int ric, int fmt,
                       int csum_ok, const char *text)
{
    if (!archive_on)
        return;
    pthread_mutex_lock(&A.lock);
    tick(time(NULL));
    if (A.fp) {
        fprintf(A.fp, "{\"e\":\"pager\",\"t\":%llu,\"ric\":%d,"
                "\"fmt\":\"%s\",\"ok\":%d,\"txt\":\"",
                (unsigned long long)(timestamp_ns / 1000000ULL),
                ric, fmt == 3 ? "BCD" : "ASCII", csum_ok ? 1 : 0);
        put_escaped(A.fp, text ? text : "");
        fputs("\"}\n", A.fp);
        fflush(A.fp);
    }
    pthread_mutex_unlock(&A.lock);
}
