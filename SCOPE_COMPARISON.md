# Scope Comparison: Burst-Type Decoding & GUI vs. Original iridium-sniffer

This document compares each completed scope item against the original
iridium-sniffer (master @ `eb2f8d7`, "Add light theme with toggle button").
Total change: **638 insertions / 7 deletions** across 8 files, no new
dependencies, no build-system changes.

| File | +lines | −lines |
|------|-------:|-------:|
| `frame_decode.c` | 275 | 0 |
| `web_map.c` | 172 | 3 |
| `ida_decode.c` | 74 | 0 |
| `main.c` | 43 | 1 |
| `ARCHITECTURE.md` | 34 | 3 |
| `ida_decode.h` | 18 | 0 |
| `frame_decode.h` | 14 | 0 |
| `web_map.h` | 8 | 0 |

---

## Scope 1 — Decode all burst types

### 1a. MSG (pager / messaging channel) — NEW decode

| | Original | Now |
|---|---|---|
| Simplex MSG frames | Demodulated to RAW bits only; passed through for the external `iridium-parser.py` | Decoded in-process to RIC, format, sequence, and 7-bit ASCII / 4-bit BCD text |
| BCH polynomial | Only 1207 (IRA/IBC) and 3545 (IDA) tables existed | Added the **messaging poly 1897** syndrome table — this was the missing primitive |
| Frame model | `frame_type_t` had `IRA`, `IBC` only | Added `FRAME_MSG` + `msg_data_t` (ric, format, seq, ctr/ctr_max, csum_ok, text) |
| Validation | n/a | Bit-exact agreement with `iridium-parser.py` across varied messages **and** under injected BCH t=2 bit errors |

**Why it matters:** "read Iridium pages" is the headline capability the original
delegated entirely to the Python toolkit. It now works with zero external
dependencies. A real parity-check bug (parity computed over corrected data but
uncorrected check bits → message truncation on noisy frames) was found and fixed
during oracle validation.

### 1b. IIP (IP-over-PPP) — NEW decode

| | Original | Now |
|---|---|---|
| LCW ft==1 frames | Demodulated only; `ida_decode` bailed on any non-IDA frame type | Decoded via `ida_iip_decode()`: header / seq / ack / 32-byte payload |
| CRC | n/a | **CRC-24** (`iip_crc24`) ported from iridium-toolkit's `crcmod` definition, verified bit-exact against `crcmod` on 6 vectors incl. random input |

### 1c. Frame-type classification — NEW for every burst

| | Original | Now |
|---|---|---|
| Per-frame type identification | None in-process; only IRA/IBC/IDA were ever recognized, everything else was opaque RAW | `classify_frame_label()` labels **every** demod frame: IRA, IBC, MSG, IDA, ISY, IIP, VOC, ITL, IU3, IU6, RAW |
| LCW type access | `ida_decode` extracted `ft` but discarded it for non-IDA frames | `ida_lcw_ft()` exposes the LCW frame-type (0=voice…7=sync); validated ft=7 on iridium-toolkit's ISY test vector |

### Decode vs. classify — deliberate boundary

| Type | Original | Now | Rationale for not deep-decoding |
|------|----------|-----|---------------------------------|
| IRA, IBC | Decoded | Decoded (unchanged) | — |
| IDA → ACARS | Decoded | Decoded (unchanged) | — |
| **MSG** | RAW passthrough | **Decoded** | — |
| **IIP** | RAW passthrough | **Decoded (CRC-validated)** | — |
| ISY, IU3, IU6 | RAW passthrough | **Classified** | No decodable payload / structure not public |
| ITL | RAW passthrough | **Classified** | Deep decode needs large PRS telemetry tables; no map value |
| VOC | RAW passthrough | **Classified** | Audio requires the proprietary AMBE vocoder (out of scope; iridium-toolkit doesn't do it in real time either) |
| IIQ/IIR/IIU | RAW passthrough | **Counted as RAW** | Require a Reed-Solomon (GF256) codec — not implemented |

This split mirrors what `iridium-parser.py` itself surfaces in real time; voice
audio, ITL PRS, and the RS-coded IP variants are left to offline/auxiliary
tooling in the upstream project too.

---

## Scope 2 — GUI (built-in web map)

The original already shipped a `--web` Leaflet map (ring alerts, satellites, and
an ACARS sidebar). The work **extended** that same server rather than adding a
new GUI stack — keeping it headless and Pi-friendly.

| Web map feature | Original | Now |
|---|---|---|
| Ring-alert map + satellites | Yes | Unchanged |
| ACARS message sidebar | Yes (single panel) | Now a **tabbed** sidebar |
| Pager (MSG) message feed | — | **New tab**: RIC, ASCII/BCD, sequence, checksum status |
| Frame-type histogram | — | **New panel** (top-right): live per-type counts for all 11 buckets |
| JSON / SSE state | ra, beams, mt, sats, aircraft, acars | + `msg[]`, `total_msg`, `ftypes{}` |
| Header stat bar | …ACARS | + **MSG** counter |

New thread-safe state APIs added to `web_map.[ch]`: `web_map_add_msg()` and
`web_map_count_type()`, following the existing ACARS-feed pattern (circular
buffer + mutex + JSON serialization).

---

## Architecture & validation

| | Original | Now |
|---|---|---|
| Frame decoder home | `frame_decode.c` (IRA/IBC) | + MSG; unified classification routed through `classify_frame_label()` in `main.c` |
| LCW reuse | `ida_decode.c` internal only | Exposed `ida_lcw_ft()` / `ida_iip_decode()` for classification |
| Docs | ARCHITECTURE.md | + pipeline node, file-map updates, **Frame Type Coverage** table, MSG/IIP validation notes |
| External deps | unchanged | **unchanged** (no new libs, no CMake changes) |

**Validation method (new):** with no test IQ and no SDR available, the upstream
`iridium-parser.py` and `crcmod` were used as oracles. A round-trip encoder
feeds identical bits to both the reference parser and a C harness; agreement on
RIC/format/sequence/text — including under bit errors — validates the port. The
one subtlety: `RAW`-prefixed lines are symbol-reversed by the parser, and the C
demod output shares the parser's post-reverse convention (the 24-bit access code
and 32-bit messaging header are symbol-reverse-invariant, all `00`/`11` pairs).
