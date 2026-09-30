# Frame-decoder tests

Regression tests for the in-process frame decoders (MSG pager text, IIP CRC,
LCW classification). No SDR or capture file is needed — frames are synthesized.

## Run

```bash
tests/run_tests.sh                # tier 1, plus tier 2 if possible
SKIP_ORACLE=1 tests/run_tests.sh  # tier 1 only (no network/crcmod needed)
```

Exits non-zero if any check fails. Build artifacts and the fetched reference
parser go in `tests/.build/` (git-ignored).

## What it checks

**Tier 1 — self-contained** (needs only a C compiler + `python3`):

- **IIP CRC-24 vectors** (`test_crc.c`) — the CRC constants in `ida_decode.c`,
  checked against values verified bit-exact against `crcmod`.
- **MSG round-trip** — `enc_msg.py` encodes a messaging frame (inverting the
  documented bit layout); the real `frame_decode()` (`test_msg.c`) must recover
  the same RIC/text, including under injected BCH bit errors.
- **LCW classification** — `ida_lcw_ft()` (`test_classify.c`) returns ft=7 for
  iridium-toolkit's own ISY test vector.

**Tier 2 — oracle cross-check** (optional; needs network + `pip install crcmod`):

- Feeds identical bits to this repo's C decoder and upstream
  `iridium-parser.py`, asserting they agree. Auto-skips if `crcmod` is missing
  or the toolkit can't be fetched. The toolkit files (GPL) are downloaded into
  `tests/.build/toolkit/`, never vendored into this repo.

## Files

| File | Role |
|------|------|
| `run_tests.sh` | Runner (builds harnesses, runs both tiers) |
| `enc_msg.py` | MSG frame encoder / oracle input generator |
| `test_msg.c` | Harness over the real `frame_decode()` |
| `test_classify.c` | Harness over the real `ida_lcw_ft()` |
| `test_crc.c` | Standalone IIP CRC-24 vector check |

## Bit convention note

`RAW`-prefixed lines are symbol-reversed by `iridium-parser.py` before decoding;
the C demod output and the parser's post-reverse stream share one convention.
The 24-bit access code and 32-bit messaging header are symbol-reverse-invariant
(all `00`/`11` pairs), so `enc_msg.py` swaps only the payload pairs.

## Parser equivalence (native parser plan)

`parser_equiv.sh` compares the sniffer's parsed output with iridium-toolkit's,
line for line, on a corpus of `RAW:` lines replayed with `--replay-raw`
(no RF, GPU or timing involved):

    tests/parser_equiv.sh -p "--parsed" /media/mike/data/webspy-captures/parser-corpus

Each `NAME.raw` in the corpus directory needs `NAME.expected.txt`, the
toolkit's `iridium-parser.py` output for it at the pinned commit (8888124).
The report gives, per frame type, how many lines match and the first
differing pair. See `docs/NATIVE_PARSER_PLAN.md`.
