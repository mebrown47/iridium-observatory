/*
 * Aircraft registration to ICAO hex lookup database
 *
 * Loads tar1090-db aircraft.csv and provides fast binary-search lookup
 * of registration -> ICAO hex address.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include "aircraft_db.h"

#define DB_URL "https://github.com/wiedehopf/tar1090-db/raw/refs/heads/csv/aircraft.csv.gz"

/* tar1090-db has ~600k rows; anything far below that is a broken download,
 * and must not replace a good database. */
#define DB_MIN_ROWS 100000

typedef struct {
    char reg[12];   /* registration (e.g., "N712TW") */
    char hex[7];    /* ICAO hex (e.g., "A98539") */
} db_entry_t;

static db_entry_t *db = NULL;
static int db_count = 0;
static int db_capacity = 0;

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const db_entry_t *)a)->reg,
                 ((const db_entry_t *)b)->reg);
}

/*
 * Normalize a registration string: strip leading dots, uppercase,
 * remove dashes/spaces.
 */
static void normalize_reg(const char *src, char *dst, int dstlen)
{
    /* Skip leading dots */
    while (*src == '.') src++;

    int j = 0;
    for (int i = 0; src[i] && j < dstlen - 1; i++) {
        if (src[i] == '-' || src[i] == ' ')
            continue;
        dst[j++] = toupper((unsigned char)src[i]);
    }
    dst[j] = '\0';
}

int aircraft_db_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "aircraft_db: cannot open %s\n", path);
        return -1;
    }

    aircraft_db_destroy();

    db_capacity = 700000;
    db = malloc(sizeof(db_entry_t) * db_capacity);
    if (!db) {
        fclose(f);
        return -1;
    }

    char line[512];
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Format: icao_hex;registration;type;flags;description;... */
        char *hex_field = line;
        char *semi1 = strchr(line, ';');
        if (!semi1) continue;
        *semi1 = '\0';
        char *reg_field = semi1 + 1;
        char *semi2 = strchr(reg_field, ';');
        if (semi2) *semi2 = '\0';

        /* Validate hex (6 chars, hex digits) */
        int hex_len = strlen(hex_field);
        if (hex_len != 6) continue;
        int hex_ok = 1;
        for (int i = 0; i < 6; i++) {
            char c = hex_field[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f'))) {
                hex_ok = 0;
                break;
            }
        }
        if (!hex_ok) continue;

        /* Skip empty registrations */
        if (!reg_field[0]) continue;

        if (db_count >= db_capacity) {
            db_capacity *= 2;
            db_entry_t *new_db = realloc(db, sizeof(db_entry_t) * db_capacity);
            if (!new_db) break;
            db = new_db;
        }

        normalize_reg(reg_field, db[db_count].reg, sizeof(db[db_count].reg));
        if (!db[db_count].reg[0]) continue;

        /* Store hex uppercase */
        for (int i = 0; i < 6; i++)
            db[db_count].hex[i] = toupper((unsigned char)hex_field[i]);
        db[db_count].hex[6] = '\0';

        db_count++;
        loaded++;
    }

    fclose(f);

    /* Sort by registration for binary search */
    qsort(db, db_count, sizeof(db_entry_t), entry_cmp);

    fprintf(stderr, "aircraft_db: loaded %d entries from %s\n", loaded, path);
    return loaded;
}

const char *aircraft_db_lookup(const char *registration)
{
    if (!db || db_count == 0 || !registration || !registration[0])
        return NULL;

    char norm[12];
    normalize_reg(registration, norm, sizeof(norm));
    if (!norm[0]) return NULL;

    db_entry_t key;
    strncpy(key.reg, norm, sizeof(key.reg));
    key.reg[sizeof(key.reg) - 1] = '\0';

    db_entry_t *found = bsearch(&key, db, db_count, sizeof(db_entry_t),
                                 entry_cmp);
    return found ? found->hex : NULL;
}

void aircraft_db_destroy(void)
{
    free(db);
    db = NULL;
    db_count = 0;
    db_capacity = 0;
}

const char *aircraft_db_default_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(path, sizeof(path), "%s/.iridium-sniffer/aircraft.csv", home);
    return path;
}

/* Fetch url into out with curl, run directly (no shell, so no quoting
 * problems with odd HOME paths). -f makes HTTP errors fail instead of
 * saving the server's error page. */
static int fetch_url(const char *url, const char *out)
{
    pid_t pid = fork();
    if (pid < 0) {
        warn("aircraft_db: fork");
        return -1;
    }
    if (pid == 0) {
        execlp("curl", "curl", "-fsSL", "--retry", "3",
               "--connect-timeout", "30", "-o", out, url, (char *)NULL);
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            warn("aircraft_db: waitpid");
            return -1;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        warnx("aircraft_db: download failed: curl is not installed");
    else if (WIFEXITED(status))
        warnx("aircraft_db: download failed (curl exit code %d)",
              WEXITSTATUS(status));
    else
        warnx("aircraft_db: download failed (curl killed by signal %d)",
              WTERMSIG(status));
    return -1;
}

/* A database row starts with a 6-digit ICAO hex address and a ';'. */
static int looks_like_row(const char *line)
{
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)line[i]))
            return 0;
    return line[6] == ';';
}

/* Decompress gz_path into out_path. Returns the number of database rows
 * written, or -1 if the file is not gzip, is truncated or corrupt, or the
 * output could not be written. */
static long gunzip_file(const char *gz_path, const char *out_path)
{
    gzFile in = gzopen(gz_path, "rb");
    if (!in) {
        warnx("aircraft_db: cannot open %s", gz_path);
        return -1;
    }
    if (gzdirect(in)) {     /* zlib passes non-gzip input through as-is */
        warnx("aircraft_db: download is not a gzip file");
        gzclose(in);
        return -1;
    }

    FILE *out = fopen(out_path, "w");
    if (!out) {
        warn("aircraft_db: cannot write %s", out_path);
        gzclose(in);
        return -1;
    }

    char line[4096];
    long rows = 0;
    int at_line_start = 1, failed = 0;
    while (gzgets(in, line, sizeof(line))) {
        size_t len = strlen(line);
        if (fwrite(line, 1, len, out) != len) {
            warn("aircraft_db: write to %s", out_path);
            failed = 1;
            break;
        }
        if (at_line_start && looks_like_row(line))
            rows++;
        at_line_start = len > 0 && line[len - 1] == '\n';
    }

    if (!failed) {
        int zerr;
        const char *msg = gzerror(in, &zerr);
        if (zerr != Z_OK) {
            warnx("aircraft_db: download is corrupt: %s", msg);
            failed = 1;
        }
    }
    /* gzclose reports a stream that ended early (Z_BUF_ERROR) */
    int zclose = gzclose(in);
    if (!failed && zclose != Z_OK) {
        warnx("aircraft_db: download is truncated or corrupt (zlib %d)",
              zclose);
        failed = 1;
    }
    if (fclose(out) != 0 && !failed) {
        warn("aircraft_db: write to %s", out_path);
        failed = 1;
    }
    return failed ? -1 : rows;
}

int aircraft_db_update(void)
{
    const char *path = aircraft_db_default_path();
    if (!path) {
        warnx("aircraft_db: cannot determine HOME directory");
        return -1;
    }

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.iridium-sniffer", getenv("HOME"));
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        warn("aircraft_db: cannot create %s", dir);
        return -1;
    }

    /* Download and unpack to temporary files beside the database; the
     * current database is replaced only once the new one checks out. */
    char gz_tmp[600], csv_tmp[600];
    snprintf(gz_tmp, sizeof(gz_tmp), "%s.gz.tmp", path);
    snprintf(csv_tmp, sizeof(csv_tmp), "%s.tmp", path);

    fprintf(stderr, "aircraft_db: downloading from tar1090-db...\n");
    if (fetch_url(DB_URL, gz_tmp) != 0) {
        unlink(gz_tmp);
        return -1;
    }

    long rows = gunzip_file(gz_tmp, csv_tmp);
    unlink(gz_tmp);
    if (rows < 0) {
        unlink(csv_tmp);
        return -1;
    }
    if (rows < DB_MIN_ROWS) {
        warnx("aircraft_db: download has only %ld aircraft (expected over "
              "%d); keeping the current database", rows, DB_MIN_ROWS);
        unlink(csv_tmp);
        return -1;
    }

    if (rename(csv_tmp, path) != 0) {
        warn("aircraft_db: cannot replace %s", path);
        unlink(csv_tmp);
        return -1;
    }

    struct stat st;
    double mb = stat(path, &st) == 0 ? st.st_size / 1e6 : 0.0;
    fprintf(stderr, "aircraft_db: saved %ld aircraft to %s (%.1f MB)\n",
            rows, path, mb);
    return 0;
}
