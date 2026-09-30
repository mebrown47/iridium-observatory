#!/bin/bash
# parser_equiv.sh - compare the sniffer's parsed output with iridium-toolkit's,
# line for line, on a corpus of RAW: lines (see docs/NATIVE_PARSER_PLAN.md).
#
#   tests/parser_equiv.sh [-b SNIFFER] [-p "PARSE FLAGS"] CORPUS_DIR
#
# CORPUS_DIR holds NAME.raw (RAW: lines) and NAME.expected.txt
# (iridium-parser.py's output for NAME.raw at the pinned toolkit commit). Each
# NAME.raw is replayed with `SNIFFER --replay-raw=NAME.raw PARSE FLAGS` and
# the output compared with NAME.expected.txt, one line per frame, grouped by
# the frame type the toolkit gave. Exit 0 only if every line matches.
#
# Defaults: SNIFFER = build/iridium-sniffer, PARSE FLAGS = --parsed=full.
set -u
sniffer=build/iridium-sniffer
flags="--parsed=full"
while getopts "b:p:" o; do
    case $o in
        b) sniffer=$OPTARG ;;
        p) flags=$OPTARG ;;
        *) echo "usage: $0 [-b SNIFFER] [-p FLAGS] CORPUS_DIR" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
dir=${1:?usage: $0 [-b SNIFFER] [-p FLAGS] CORPUS_DIR}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

total=0; same=0
for raw in "$dir"/*.raw; do
    name=$(basename "$raw" .raw)
    exp="$dir/$name.expected.txt"
    [ -f "$exp" ] || { echo "$name: no $name.expected.txt, skipped"; continue; }
    # shellcheck disable=SC2086
    if ! "$sniffer" --replay-raw="$raw" $flags > "$tmp/$name.out" 2> "$tmp/$name.err"; then
        echo "$name: $sniffer failed:"; tail -3 "$tmp/$name.err"; exit 1
    fi
    n_exp=$(wc -l < "$exp"); n_out=$(wc -l < "$tmp/$name.out")
    [ "$n_exp" -eq "$n_out" ] || echo "$name: $n_out lines out, $n_exp expected (lines compared in order)"
    # per toolkit frame type: lines equal / lines total, and the first differing pair
    paste -d '\n' "$exp" "$tmp/$name.out" | awk -v name="$name" -v dlog="$tmp/$name.diff" '
        NR % 2 == 1 { e = $0; next }
        {
            t = e; sub(/:.*/, "", t)
            tot[t]++; all++
            if ($0 == e) { eq[t]++; alleq++ }
            else if (!(t in shown)) { shown[t] = 1; print t "\n  toolkit: " e "\n  native:  " $0 > dlog }
        }
        END {
            printf "%s: %d of %d lines match\n", name, alleq, all
            for (t in tot) printf "    %-4s %6d / %-6d\n", t, eq[t], tot[t]
            print alleq + 0, all + 0 > (dlog ".sum")
        }'
    read -r s t < "$tmp/$name.diff.sum"
    same=$((same + s)); total=$((total + t))
    if [ -s "$tmp/$name.diff" ]; then
        echo "    first difference per type (cut to 150 columns):"
        cut -c1-150 "$tmp/$name.diff" | sed 's/^/      /'
    fi
done
echo "TOTAL: $same of $total lines match"
[ "$same" -eq "$total" ] && [ "$total" -gt 0 ]
