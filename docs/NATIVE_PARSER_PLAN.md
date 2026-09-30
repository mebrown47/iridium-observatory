# Native parser and reassembler — plan

Goal: iridium-sniffer produces, in C, the same frame lines as iridium-toolkit's
`iridium-parser.py`, and then the same ACARS and SBD output as its
`reassembler.py`, so nothing downstream (the Iridium analyzer's Live view and
decode-on-Freeze, the web map, archives) needs Python.

Order agreed 2026-09-29: **the parser first, then ACARS and SBD.** Other
reassembler modes come later, if at all.

## Why match the toolkit exactly

iridium-toolkit (muccc) is the community's reference for Iridium frame
formats. Its line output is what people compare, grep and feed to other tools.
A native parser is only useful if it says the same thing: same frame type,
same fields, same formatting, same verdict on frames it cannot classify. So
every phase is judged against the toolkit's own output on the same input —
line for line — not against a reading of the Python.

## License

The toolkit's README: "Unless otherwise noted in a file, everything here is
(c) Sec & schneider and licensed under the 2-Clause BSD License." No file
checked notes otherwise. BSD-2 is compatible with this repository's
GPL-3.0-or-later. Ported files keep the notice:

    Frame formats ported from iridium-toolkit (https://github.com/muccc/iridium-toolkit),
    (c) Sec & schneider, 2-Clause BSD License - see LICENSES/BSD-2-Clause-iridium-toolkit.txt

and the full BSD-2 text goes in `LICENSES/`. The combined files are
distributed under GPL-3.0-or-later like the rest of the project.

## Reference version

Frame-type names in this plan are the prefixes the toolkit prints, read from the `pretty()` methods in `bitsparser.py`.

Port against a pinned toolkit commit: **8888124** (2026-02-22, "[util] bugfix:
no leap seconds in the new ERA (yet)"). Record the commit in the test fixtures.
When the toolkit changes, re-run the equivalence tests against the new commit
and port the difference deliberately.

## Test method: equivalence against the toolkit

For each corpus file of `RAW:` lines:

    iridium-parser.py < corpus.raw        > expected.txt   # at the pinned commit
    iridium-sniffer --replay-raw corpus.raw --parsed=full > actual.txt
    diff expected.txt actual.txt          # must be empty

`--replay-raw` is new: it reads `RAW:` lines (the sniffer's own output format)
and runs only the parsing stage, so the parser is tested without RF, GPU or
timing. The existing oracle cross-check in `tests/run_tests.sh` (MSG vs
iridium-parser.py) is the seed of this; it becomes one case among many.

Same for phase 2 with `reassembler.py -m acars` / `-m sbd` against the native
output.

Differences that are cosmetic (float formatting, for example) are still
differences. If exact equality turns out impossible somewhere, write down why
in this file and normalise that field in the comparison — never silently.

### Corpus

| Source | Lines | Contents |
| --- | --- | --- |
| `expected_raw10m.sniffer.raw` (Data drive, iridium-2026-09-27) | 167 | 10 types: RAW 105, ISY 23, IDA 12, IRI 7, IBC 7, ITL 6, IME 3, IRA 2, IMS 1, IIU 1 |
| `expected_chan_1626.1539MHz.sniffer.raw` | 17 | RAW 7, ITL 5, IRI 3, IRA 2 |
| `q6_20min_1625.5_2M5.ci16` decoded once to RAW | ~20 min at 2.5 MS/s | to generate |
| Feeder live RAW (HackRF 10 MS/s, Elba) | hours | to capture; the only source likely to hold VOC, IU3, IIP, ACARS and SBD in quantity |
| Synthetic frames (`tests/enc_msg.py` style) | — | for types too rare to catch, and for error paths |

The first two are far too small; building the corpus is step 0.
Types with no corpus examples are listed as untested, not assumed working.

## What exists in C today

| Area | iridium-toolkit | iridium-sniffer now |
| --- | --- | --- |
| Frame parsing | `iridium-parser.py` (541 lines) + `bitsparser.py` (2045) | `frame_decode.c`: IRA, IBC, MSG; `ida_decode.c`: IDA (printed with `--parsed`); everything else printed as `RAW:` |
| ACARS / SBD | `iridiumtk/reassembler/sbd.py` (modes `sbd`, `acars`, `libacars`) on top of `ida.py` | `sbd_acars.c` (~2200 lines, "derived from reassembler/sbd.py") feeds the web map, UDP and archive |

`bitsparser.py` class structure (the port should mirror it, so later toolkit
changes map onto the C one to one):

- `Message` → `IridiumMessage` (IRI) → `IridiumLCWMessage` → SY (ISY), LCW3 (IU3, I36, I38), VO (VOC, VOD, VO6, VOZ; VDA hands over to IP), IP (IIP, IIQ, IIR, IIU, VDA)
- `IridiumMessage` → AQ (IAQ), STL (ITL), NXT (NXT)
- `IridiumMessage` → `IridiumECCMessage` (IME) → BC (IBC), RA (IRA), MS (IMS) → MS body → messaging ASCII (MSG) / BCD (MS3)
- `IridiumMessage` → `IridiumLCWECCMessage` → DA (IDA)
- helpers: de-interleave (2/3/LCW), symbol reverse, DQPSK, BCH, checksums

## Phase 0 — groundwork

- [x] `LICENSES/BSD-2-Clause-iridium-toolkit.txt` (the notice goes into each ported file's header when the first one lands).
- [x] `--replay-raw=FILE` in the sniffer: reads `RAW:` lines, rebuilds each demodulated frame (tag and time base, time, frequency, magnitude, noise, id, confidence, level, payload symbols, bits; direction from the access code; no soft values, as the toolkit has none) and runs only the output stage (`handle_demod_frame()`, shared with the live pipeline). Round trip RAW -> RAW is byte-identical on all 50,986 corpus lines; a live decode is unchanged by the refactor (167/167 frames identical to the previous build).
- [x] Corpus on the Data drive, `webspy-captures/parser-corpus/` (README, SHA256SUMS; expected outputs at toolkit 8888124): raw10m 167, chan1626 17, fc10m 16,723 (the 10 MS/s @1622 MHz cf32 capture), q6 34,263 frames. 18 toolkit types present; VOD, VDA, IIR, IAQ, NXT, MSG, MS3 missing.
- [ ] Feeder RAW, a few hours (the feeder's owner runs it) — for the missing types and for ACARS/SBD in phase 2.
- [x] `tests/parser_equiv.sh [-b SNIFFER] [-p FLAGS] CORPUS_DIR`: per corpus file and toolkit frame type, lines matching / total, and the first differing pair.

Baseline with today's `--parsed` (2026-09-29): **0 of 51,170 lines match** —
everything but IDA is printed as the sniffer's own RAW format. What the
differences already show for phase 1:

- The toolkit reformats the common header: file tag `i-<t0>-t1` becomes
  `p-<t0>-e000`, the time is widened (`000000258.7722`), and it prints
  `conf% level|noise|magnitude symbols DIR` instead of `N:` / `I:`.
- The existing IDA line differs only in the tag (`-e000` missing) and
  trailing padding.
- The toolkit's `RAW` lines are not a pass-through either: bits grouped by
  16 and an `ERR:` reason — the fall-through has to be ported too.
- RAW bits are symbol-reversed by the parser before decoding (see
  `tests/README.md`, "Bit convention note").

## Phase 1 — parser, all frame types

`bits_parser.c` ports `bitsparser.py` (and `iridium-parser.py`'s line
output) function by function, each naming the Python it follows; bit
strings stay strings of '0'/'1' as in the toolkit. `tk_rs.c` ports the
Reed-Solomon decoder the toolkit bundles (`reedsolo.py`, public domain;
the 8-bit field of `rs.py` and the 6-bit one of `rs6.py`), the IIP CRC-24
and `checksum_16`. `itl_tables.h` is generated from `itl.py`.
`--parsed=full` formats each frame's RAW: line in memory and parses that,
so the native parser sees the same rounded values (level, SNR, time) the
toolkit does.

Status (2026-09-29): **every line matches.**

| Set | Lines | Matching | Types |
| --- | --- | --- | --- |
| Recordings (raw10m, chan1626, fc10m, q6) | 51,170 | 51,170 | ISY, IDA, I36, IIP, IBC, VOC, VO6, VOZ, IRI, IU3, IIU, ITL, IRA, IME, IMS, I38, IIQ, RAW |
| Synthetic (`tests/parser_fuzz.py`, seed 7) | 29,500 | 29,500 | IAQ, NXT, MSG, MS3, VOD, VDA, IIR, plus IRI/IME/IMS/RAW error paths |
| Live, Airspy R2 + L-band patch via SoapySDR, 10 min (2026-09-29) | 50,605 | 50,605 | 18 types incl. real MSG (15) and VDA (1,413); 0 samples dropped |

The Reed-Solomon, CRC-24 and checksum ports were also checked alone against
the toolkit's Python on 12,000 vectors (codewords with 0-11 symbol errors,
random data): identical results, correcting and failing alike.

A 60 s live run with `--parsed=full` directly from the R2: 7,485 lines, 0 dropped.

Speed: 34,263 frames parse in 0.22 s (about 150,000 frames/s) against
3.46 s for iridium-parser.py.

- [x] 1. Common header and `IRI`.
- [x] 2. `RAW` fall-through.
- [x] 3. LCW family: `ISY`; `IU3`/`I36`/`I38`; `VOC`/`VOD`/`VO6`/`VOZ`; `IIP`/`IIQ`/`IIR`/`IIU`/`VDA`.
- [x] 4. ECC family: `IME`, `IBC`, `IRA`, `IMS`, `MSG`, `MS3`.
- [x] 5. `IDA` (LCW-ECC).
- [x] 6. `ITL`, `IAQ`, `NXT`.
- [ ] 7. Parser options beyond the defaults: `--harder`, `--uw-ec`, confidence filters, `--filter`. Not needed by the analyzer or the feeders; `--harder` / `--uw-ec` code paths are marked in bits_parser.c.
- [x] The Iridium analyzer on `--parsed=full` (iridium-analyzer 10be317, branch feat/native-parser): decode-on-Freeze via `--replay-raw --parsed=full`, Live via `--zmq-sub ... --parsed=full` with no Python process. corpus_check ALL PASSED native and Python, reports identical; live_check 167/167.
- [ ] `frame_decode.c` / `ida_decode.c` re-based on bits_parser.c (one parser, not two) — separate change; they feed the web map, positioning and ACARS today and are not touched by this phase.

Not handled (the toolkit fails there too, with an exception rather than a
line): malformed input that makes the Python raise IndexError/ValueError
(e.g. an odd number of bits into de_interleave). The native parser prints
something instead; no corpus line hits these.

## Phase 2 — ACARS and SBD

`tk_reassembler.c` ports `reassembler.py` with `iridiumtk/reassembler/`
`base.py`, `ida.py` and `sbd.py`. `--reassemble=ida|sbd|acars` feeds each
frame's parsed line (phase 1) to it and prints what
`iridium-parser.py | reassembler.py -m MODE` prints, summary lines included;
it works live and with `--replay-raw`. `tests/reasm_check.c` runs it on
parser lines; `tests/reasm_fuzz.py` makes synthetic IDA lines.

Status (2026-09-29):

| Input | ida | sbd | acars |
| --- | --- | --- | --- |
| Corpus (fc10m, q6, raw10m, chan1626) | 2,747 / 2,747 | 63 / 63 | 34 / 34 |
| Live R2, 10 min | 1,775 / 1,775 | 62 / 62 | 28 / 28 (21 ACARS messages) |
| Synthetic, seeds 3-5 | 23,545 / 23,545 | 15,621 / 15,621 | 5,090 / 5,090 |

(lines identical / lines). The recordings hold no multi-packet SBD and no
uplink ACARS; the synthetic set does (980 multi-packet messages per seed;
UL with SEQ/FNO, NAK, ETB, header blocks, bad parity/CRC/ETX, duplicates,
orphans, expiry). Two inputs make reassembler.py raise instead of printing -
an SBD packet with msgno 1 and no count, and an uplink ACARS without STX;
the port prints/skips there, and the generator avoids them.

- [x] IDA reassembly (`ida` mode) — the base for both.
- [x] `sbd` mode.
- [x] `acars` mode, plain text.
- [x] `acars` options, as `--reassemble=acars,json,showerrs,...` (`-a json,showerrs,...`): `json`, `showerrs`, `nopings`, `perfect` (accepted; no effect in these modes). Seeds 6-8: all 6 option combinations identical (≈13,000 acars lines per seed), live set too. Fixed on the way: ACARS fields are written byte for byte (Python prints NULs in `REG:` etc.).
- [x] `libacars` mode, plain text (with `showerrs`, `nopings`), when built with libacars-2: the same libacars calls as the toolkit's libacars.py wrapper, including its quirk of always passing 0 µs to the reassembly. Identical on the live set, q6, fc10m and a synthetic set (14,874 / 17,138 / 11,267 lines plain / showerrs / nopings).
- [x] `libacars` `json`: the toolkit re-serializes libacars' JSON through Python's json module (`json.loads(...)['acars']` into `json.dumps`); tk_reassembler.c does the same round trip (numbers as Python keeps them, strings re-escaped ASCII-only, dict key order). Identical on the live set, q6, fc10m and synthetic ACARS (1,846 / 2,978 / 1,393 lines json / +showerrs / +nopings). Not exercised: ARINC-622 payloads (ADS-C, CPDLC), where libacars prints floats - none in the corpus yet.
- [ ] Reconcile with `sbd_acars.c` (the web map / UDP / archive path) — compare its reassembly with this one on the corpus, then share one.

Done when the corpus' ACARS and SBD output matches the toolkit's line for line.

## Later, if wanted

`msg`/`page` (pager messages), `ida`/`idapp`/`lap`/`gsmtap` (Wireshark),
`itlmap`/`satmap`/`time`/`ira`/`ppm` (overlap with the positioning and the
analyzer's satellite identification), statistics and live-map modes (the
dashboard covers some). Voice decoding (VOC → audio) stays out: it needs an
AMBE codec that is not part of the toolkit and has licensing problems of its
own.

## Working rules

- Work on branches in a separate worktree; the main checkout may hold
  uncommitted work in progress.
- Every ported function names the Python function it follows.
- No behaviour the toolkit doesn't have in `--parsed=full` / reassembler
  output; extras (confidence scores, satellite IDs) go in other outputs.

## Open questions

- Does the sniffer's `RAW:` line carry everything `iridium-parser.py` reads?
  (It is parsed by the toolkit today, so presumably yes; `--replay-raw` will
  confirm.)
- Where the toolkit's output depends on floating-point formatting of derived
  values (positions, times), is C's `printf` identical to Python's? Check
  early, on ITL/IRA/IBC.
- Feeder RAW capture: file on the feeder, or streamed? (Owner's choice.)
