# Scope Comparison: This Fork vs. Original iridium-sniffer

This document summarizes everything this branch changes relative to the
original iridium-sniffer (master @ `eb2f8d7`, "Add light theme with toggle
button").

**Total change: ~5,800 insertions / 28 deletions across 41 files** (25 new,
16 modified). **No new dependencies** — the AT1/CPDLC work uses libacars-2,
which is already an *optional* dependency in upstream's `CMakeLists.txt`
(`HAVE_LIBACARS`); without it the binary still builds and runs. **Build-system
change is one line**: `archive.c` added to the source list (`sbd_acars.c` was
already built by upstream).

The RF signal path (burst detect → QPSK/DQPSK demod → framing) and the Leaflet
map are untouched at their core; all additions are additive layers on top.

## Change by area

| Area | ~lines | What |
|------|-------:|------|
| In-process C decode + web engine | ~1,820 | Burst-type decode, frame classification, aviation-datalink surfacing, Doppler, UW gating, `--archive` |
| Observatory analytics (Python) | ~2,270 | Read-only rollup + dashboards over the archive; **never touches the RF engine** |
| Test harnesses | ~1,020 | Oracle-validated decode regression + UW replay |
| Documentation | ~680 | This file + `docs/` explainers + ARCHITECTURE updates |

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
| IDA → ACARS | Decoded (map position only) | **Decoded + surfaced**: ADS-C/CPDLC broken out into AT1 & CPDLC tabs and archived | See Scope 3 / [docs/AT1_DECODE.md](docs/AT1_DECODE.md) |
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
| AT1 (ADS-C) message feed | — | **New tab** (see Scope 3): structured flight position reports |
| CPDLC message feed | — | **New tab** (see Scope 3): controller/pilot clearances |
| Frame-type histogram | — | **New panel** (top-right): live per-type counts for all 11 buckets |
| Doppler S-curves | — | **New page** backed by `GET /api/doppler` (see Scope 4) |
| JSON / SSE state | ra, beams, mt, sats, aircraft, acars | + `msg[]`, `premium[]`, `total_msg`, `ftypes{}` |
| Header stat bar | …ACARS | + **MSG** counter |

New thread-safe state APIs added to `web_map.[ch]`: `web_map_add_msg()`,
`web_map_add_premium_msg()`, and `web_map_count_type()`, following the existing
ACARS-feed pattern (circular buffer + mutex + JSON serialization).

**Bug fixed:** `JSON_BUF_SIZE` was raised 128 KiB → 1 MiB. On a busy receiver a
fully-populated `/api/state` overran 128 KiB, `snprintf` clamped mid-object, and
the truncated JSON failed to parse client-side — **every tab reading /api/state
went silently blank with no error.** 1 MiB clears the worst case with headroom.

---

## Scope 3 — Aviation datalink surfaced (ADS-C & CPDLC)

Full write-up in [docs/AT1_DECODE.md](docs/AT1_DECODE.md). In short: upstream
decoded FANS-1/A (ADS-C position reports, CPDLC clearances) with libacars only
far enough to drop a dot on the map. This fork walks the full decode tree in
`sbd_acars.c` and **surfaces** it:

- **AT1 tab** — ADS-C reports rendered as one operator-readable line
  (position / flight level / speed / heading / vertical rate / winds / temp /
  next waypoint + ETA), via `web_map_add_premium_msg(reg, flt, "AT1", …)`.
- **CPDLC tab** — clearances located with `la_proto_tree_find_cpdlc()`, each
  element formatted and flattened to one line (`flatten_la_text()`).
- **Archived** — each decoded message's proto tree is written to the `--archive`
  JSONL (`archive_log_acars()`), feeding the Observatory's Aviation dashboard.

All additive; the RF path and the existing map positioning are unchanged.
Requires the (already-optional) libacars-2 at build time; without it these tabs
stay empty and raw ACARS still flows.

---

## Scope 4 — Signal-quality gating & Doppler tracking

**`--uw-reject=T` (NEW flag).** A matched-filter unique-word **sync-score gate**
(`uw_sync_score()` in `qpsk_demod.c`): UW-fail bursts scoring below `T` are
dropped as burst-detector false positives rather than counted. Off by default
(`T=0` disables); characterization of the UW-fail population drove the threshold
choice.

**Doppler tracking (NEW).** `doppler_pos.c/.h` collect per-satellite Doppler
residuals; a `GET /api/doppler` route (`build_doppler_json()` in `web_map.c`,
read-only, mutex-guarded) feeds the Observatory's Doppler S-curve page. A
UW-ambiguous direction counter was also added.

---

## Scope 5 — Observability: `--archive` + Observatory analytics

**`--archive[=DIR]` (NEW flag).** `archive.c/.h` — an append-only JSONL
observability archive: per-minute frame-type rollups, decoded ACARS (with the
libacars tree), and pager messages; rotates daily (UTC). Self-contained (libc +
`archive.h` only).

**Observatory (NEW, `observatory/`).** A stdlib-only Python analytics layer that
**consumes** the archive and never touches the RF engine:

- `rollup.py` — folds the JSONL into a SQLite database (incremental byte-offset
  ingest; `observatory.db` is runtime state, gitignored).
- `trends_server.py` — serves three self-contained dashboards on **:8890**:
  Trends, Aviation (per-tail history, CPDLC/ADS-C counts), and Doppler.
- systemd units + `install_observatory.sh` one-shot installer; see
  `observatory/QUICKSTART.md`.

Architectural principle: **the RF engine is sacred; the Observatory is a
read-only consumer.**

---

## Architecture & validation

| | Original | Now |
|---|---|---|
| Frame decoder home | `frame_decode.c` (IRA/IBC) | + MSG; unified classification routed through `classify_frame_label()` in `main.c` |
| LCW reuse | `ida_decode.c` internal only | Exposed `ida_lcw_ft()` / `ida_iip_decode()` for classification |
| Aviation decode | libacars → map dot | libacars tree walked and surfaced (Scope 3) |
| Docs | ARCHITECTURE.md | + pipeline nodes, file-map updates, Frame Type Coverage table, and `docs/` explainers (HOW_IT_WORKS, NEW_FEATURES, AT1_DECODE) |
| External deps | libacars-2 (optional) | **unchanged** (still optional; one source file added to the build) |

**Validation method (new):** with no test IQ and no SDR available, the upstream
`iridium-parser.py` and `crcmod` were used as oracles. A round-trip encoder
feeds identical bits to both the reference parser and a C harness; agreement on
RIC/format/sequence/text — including under bit errors — validates the port. The
one subtlety: `RAW`-prefixed lines are symbol-reversed by the parser, and the C
demod output shares the parser's post-reverse convention (the 24-bit access code
and 32-bit messaging header are symbol-reverse-invariant, all `00`/`11` pairs).
The suite (`tests/run_tests.sh`, 10 tier-1 checks) covers CRC-24 vectors, MSG
encode→decode round-trips with BCH error correction, and LCW classification; a
UW-fail replay harness (`tests/uw_replay.c`) characterizes the reject gate.
