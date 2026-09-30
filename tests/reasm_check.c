/* reasm_check MODE [ARG...] < parsed-lines: run tk_reassembler.c on parser lines
 * (iridium-parser.py's format) and print what reassembler.py -m MODE would -
 * for testing the reassembler on synthetic input (tests/reasm_fuzz.py). */
#include <stdio.h>
#include <stdlib.h>
#include "tk_reassembler.h"
int main(int argc, char **argv) {
    tkr_mode_t m = argc > 1 ? tkr_mode_from_name(argv[1]) : TKR_OFF;
    if (m == TKR_OFF) { fprintf(stderr, "usage: %s ida|sbd|acars [json|showerrs|nopings|perfect ...] < lines\n", argv[0]); return 2; }
    for (int i = 2; i < argc; i++)
        if (tkr_set_arg(argv[i]) != 0) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    tkr_init(m, stdout);
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, stdin) > 0) tkr_line(line);
    tkr_end();
    free(line);
    return 0;
}
