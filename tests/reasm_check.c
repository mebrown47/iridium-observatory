/* reasm_check [--tagged] MODE [ARG...] < parsed-lines (--tagged: RSM: lines): run tk_reassembler.c on parser lines
 * (iridium-parser.py's format) and print what reassembler.py -m MODE would -
 * for testing the reassembler on synthetic input (tests/reasm_fuzz.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tk_reassembler.h"
static void emit(const char *line, void *ctx) { (void)ctx; printf("%s\n", line); }

int main(int argc, char **argv) {
    int tagged = 0;
    if (argc > 1 && !strcmp(argv[1], "--tagged")) { tagged = 1; argv++; argc--; }
    tkr_mode_t m = argc > 1 ? tkr_mode_from_name(argv[1]) : TKR_OFF;
    if (m == TKR_OFF) { fprintf(stderr, "usage: %s ida|sbd|acars [json|showerrs|nopings|perfect ...] < lines\n", argv[0]); return 2; }
    for (int i = 2; i < argc; i++)
        if (tkr_set_arg(argv[i]) != 0) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    if (tagged) {                       /* RSM: lines, as --messages prints them */
        tkr_t *t = tkr_new(m, stdout);
        tkr_set_emit(t, argv[1], emit, NULL);
        for (int i = 2; i < argc; i++) tkr_arg(t, argv[i]);
        char *line = NULL; size_t cap = 0;
        while (getline(&line, &cap, stdin) > 0) tkr_feed(t, line);
        tkr_finish(t);
        free(line);
        return 0;
    }
    tkr_init(m, stdout);
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, stdin) > 0) tkr_line(line);
    tkr_end();
    free(line);
    return 0;
}
