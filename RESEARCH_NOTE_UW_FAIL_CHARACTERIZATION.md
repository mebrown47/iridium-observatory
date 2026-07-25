# Research Note: Characterization of the Iridium UW-Fail Population

**Date:** 2026-07-24  
**Author:** Mike Brown / mebrown47  
**Platform:** iridium-sniffer fork (`mebrown47/iridium-sniffer`, branch `msg-iip-decode-and-classify`)  
**Feeder:** Elba, Alabama — south-facing L-band antenna, HackRF SDR

---

## ⚠️ CORRECTION — 2026-07-24 (added same day, supersedes the conclusions below)

The original conclusions in this note (Findings §3 and Implications) were based on
**spectral eyeballing of 20 burst files only**. A quantitative replay harness
(`tests/uw_replay.c`) was subsequently built and run against the same 40-file
sample. It replays the *exact* `qpsk_demod` decode logic and sweeps each recovery
knob independently. Built-in oracle: baseline passes 20/20 `_DL` and fails 20/20
`_UN`, confirming the replay reproduces production behaviour.

**The harness contradicts two of this note's three original conclusions:**

| Recovery attempt | UN recovered (UW Hamming ≤ 2) |
|---|---|
| 4-quadrant phase rotation | 0 / 20 |
| Start-phase / timing sweep | 0 / 20 |
| Full-frame exhaustive UW slide (every offset × 4 rotations × DL/UL) | **1 / 20** |
| Residual-CFO sweep ±40 kHz | 4 / 20 |

- **Hypothesis 3 (alignment failure) is NOT supported.** Phase rotation and
  timing re-alignment recover *nothing*. The standard DL/UL unique word is simply
  **absent** from 19 of 20 UN bursts at *any* alignment (best-case Hamming
  bottoms out at 3–7). **The original recommendation to "widen the UW correlator
  search window in `qpsk_demod.c`" would recover essentially nothing and should
  NOT be actioned.**
- **The UW/alignment machinery is in `burst_downmix.c`, not `qpsk_demod.c`.**
  Coarse CFO → fine CFO (squared-FFT) → RRC → FFT sync-word correlation → phase
  align all happen there. `qpsk_demod.c` only does a lightweight *re-check*.

**Corrected breakdown of the UN population (n=20):**

1. **~20% — residual large Doppler (recoverable, real defect).** 4 files reach
   Hamming ≤ 2 only under a large applied CFO: **−8.5, −18.7, −24.4, −24.7 kHz**
   (all negative, up to ~25 kHz). Iridium LEO Doppler runs to ±37 kHz;
   `estimate_fine_cfo()` in `burst_downmix.c` is not removing the full offset for
   these. **Fix belongs in the downmix CFO stage, not the demod.**
2. **~75% — no UW at any alignment/rotation/CFO.** Not standard-UW Iridium frames.
   Mechanism: `correlate_sync()` has **no acceptance threshold** — it always
   returns the arg-max peak, so any energy-detector hit (adjacent-channel splatter,
   other L-band emitters, noise) is extracted and logged as "UN." This means much
   of the 37% is **burst-detector false positives** — i.e. **Hypothesis 1, which
   this note originally claimed to have "definitively eliminated," is back in play
   and likely the majority.**

**Two real levers to reduce the 37% (neither is the original recommendation):**
- Extend/repair the CFO search range in `burst_downmix.c` → recovers the Doppler subset.
- Add a correlation-quality acceptance threshold in `correlate_sync()` → stops
  counting false-positive bursts as UW-failures.

### AT SCALE — 1951 UN + 3806 DL, fresh 30 s feeder capture (2026-07-24)

The harness was re-run on the feeder against a fresh corpus (`~/uw_corpus`,
UN fraction 34% — matches the reported UW-fail rate). Oracle held: DL baseline
pass **3799/3806 (99.8%)**, UN baseline pass **0/1951**.

| Measure (n=1951 UN) | Result |
|---|---|
| DL control — UW found somewhere (definitive slide) | 3803 / 3806 (99.9%) ✓ |
| **UN — standard DL/UL UW present ANYWHERE** | **312 / 1951 (16%)** |
| UN — no standard UW at any alignment | **1639 / 1951 (84%)** |
| UN recovered by current demod knobs (timing+CFO+rot) | 211 / 1951 (11%) |
| — by residual timing / start-phase | 99 |
| — by residual CFO/Doppler (143 large-Doppler hits w/ overlap) | 103 |
| — by 4-quadrant phase rotation | 1 |

**Confirmed at scale:**
- **~84% of UW-fails contain no standard unique word at all** → burst-detector
  false positives / non-standard content. `correlate_sync()` has no acceptance
  threshold, so any energy hit is logged as a UW-fail. **This is the dominant
  component of the 37% and is a counting artifact, not lost Iridium.**
- **~16% contain a real DL/UL UW** recoverable in principle; ~11% are reached by
  modest CFO+timing improvement. Phase rotation is irrelevant (1/1951).

**Rate decomposition (UN = 34% of decode attempts):** ~84% × 34% ≈ **28 points are
false positives** (fix: acceptance threshold in `correlate_sync`), ~16% × 34% ≈
**5 points are recoverable real Iridium** (fix: CFO range + start-timing in
`burst_downmix.c`; ~3.7 pts reachable today). Widening the `qpsk_demod` UW window
— the original recommendation — addresses neither.

**Still open:** the recoveries are UW-match candidates at Hamming ≤2, **not
confirmed decodes** — must be validated by *downstream* BCH/CRC before the ~16%
is trusted (a 12-symbol UW can match by chance across a wide sweep).

*The original text below is retained unaltered as the record of the initial
(spectral-only) investigation. Read Findings §3 and Implications in light of the
correction above.*

---

## Background

The `iridium-sniffer` tool reports a sustained unique-word (UW) failure rate of
approximately 35% on the Elba feeder — meaning roughly one in three detected
bursts passes the energy-based burst detector but fails the subsequent unique-word
correlation step and is classified as a decode failure. The Iridium Observatory
dashboard tracks this as the "unknown-burst rate," which is also proposed as an
early-warning tripwire for new constellation modes.

Prior to this investigation, the nature of the UW-fail population was unknown.
Three competing hypotheses existed:

1. **False positives** — the burst detector is finding noise events that were
   never Iridium transmissions, making UW failure expected and correct.
2. **Signal quality failures** — the bursts are real Iridium transmissions but
   too weak, short, or distorted for the UW correlator to match.
3. **Alignment failures** — the bursts are real Iridium transmissions of
   adequate quality, but a timing or frequency offset prevents the UW correlator
   from achieving a clean match.

Distinguishing between these hypotheses requires direct inspection of the raw IQ
for failed bursts — which this investigation provides for the first time.

---

## Method

iridium-sniffer was run against the live Elba feeder with `--save-bursts` enabled,
capturing the downmixed IQ (cf32 format, 250 kS/s) for every detected burst
regardless of decode outcome. Files are tagged `_DL`, `_UL`, or `_UN` based on
direction assignment.

A 3-minute capture produced 782,080 burst files (8.1 GB), split:
- **DL**: 495,024 files (63%)
- **UN**: 287,056 files (37%)
- **UL**: 0 files (consistent with previously documented UL scarcity finding)

A random sample of 20 DL and 20 UN files was selected for IQ analysis. Files
were visualized using Python (NumPy/Matplotlib) due to a minimum frame-size
constraint in the CUDA_FFT GUI tool that prevented opening the shorter files
directly.

---

## Key Findings

### 1. UN bursts are structurally identical to DL bursts

The most significant finding: **spectral and temporal analysis of UN burst IQ is
indistinguishable from successfully decoded DL burst IQ.**

Both populations show:
- Identical flat-topped, steep-sided QPSK spectral envelope (~25 kHz bandwidth)
- Identical burst duration (~7.64ms at 250 kS/s, consistent with the 8.28ms
  Iridium TDMA burst specification)
- Identical energy levels (noise floor ~50 dB below peak in both cases)
- Identical sustained, coherent spectrogram pattern across the full burst duration

**Hypothesis 1 (false positives) is definitively eliminated.** The UN population
is not noise or interference — it consists entirely of real, coherent,
modulation-bearing transmissions that are spectrally and temporally
indistinguishable from successfully decoded bursts.

### 2. File size is uniform across both populations

Every burst file — both DL and UN — is 15,280 bytes (1,910 samples at 250 kS/s
= 7.64ms), with the exception of two DL files at 35,520 bytes (4,440 samples =
17.76ms) which represent longer multi-slot frames. There is no size difference
between the DL and UN populations.

**Hypothesis 2 (signal quality failures) is eliminated.** UN bursts are not
shorter, weaker, or truncated relative to DL bursts.

### 3. The failure is localized to the UW correlation stage

Since UN bursts are spectrally real, adequately strong, and full-duration, the
failure cannot be occurring at burst detection, downmixing, or signal quality
assessment. The only remaining stage between burst capture and UW-fail is the
unique-word correlation in `qpsk_demod.c`.

**Hypothesis 3 (alignment failure) is supported by elimination.** The UW
correlator is receiving a real, valid Iridium burst but failing to achieve a
clean match. Potential mechanisms include:

- **Residual frequency offset** — the downmixer places the burst near but not
  exactly at 0 Hz; a small residual shifts the UW pattern slightly out of the
  correlator's acquisition window.
- **Timing misalignment** — the UW correlator searches a fixed window of symbol
  positions; if the burst arrives with a timing offset outside that window, the
  correlation peak is missed even when the UW is present.
- **Both simultaneously** — a combination of small frequency and timing offsets
  that individually would be tolerable but together push the correlation score
  below threshold.

One subtle spectral difference was noted: DL burst spectra show a sharp
double-spike at the center frequency (the preamble/unique word standing above
the data payload), while UN burst spectra show a broader, smoother peak at the
same position. This is consistent with the UW pattern being present in the UN
burst but smeared by a slight alignment offset rather than absent entirely.

---

## Implications

### Recovery potential

The 35% UW-fail population represents real Iridium transmissions that are
currently lost to a correlator alignment failure rather than to any fundamental
signal quality limitation. This is a tunable threshold, not a physical ceiling.
Widening the UW correlator's search window — either in frequency offset tolerance
or in timing alignment range — could recover a substantial fraction of this
population.

Even a 50% recovery rate would reduce the overall loss from ~35% to ~17%,
representing a significant improvement in observational fidelity without any
hardware changes.

### New-mode detection

The "unknown-burst rate as tripwire" concept (currently implemented in the
Iridium Observatory dashboard) is validated but requires a refinement: the
baseline 35% rate is now known to consist of alignment failures on known-good
Iridium bursts, not anomalous content. A meaningful tripwire requires
characterizing and modeling the baseline alignment-failure rate so that deviations
from it — rather than deviations from zero — become the detection signal.

A sustained increase in the UW-fail rate beyond the modeled baseline, or a
change in the spectral character of the UN population (e.g., a new spectral shape
appearing that doesn't match the QPSK envelope), would be the correct signature
of a new constellation mode or transmission type.

### Architectural note

All 139,543 UW-fail events in the verbose log showed `dir=??` (DIR_UNDEF).
This is expected behavior: direction assignment in `burst_downmix.c` is
provisional and the finer assignment in `qpsk_demod.c` only completes for
bursts that pass UW correlation. Bursts that fail UW correlation retain
DIR_UNDEF. This is not a bug; it is a consequence of the current pipeline's
direction-assignment architecture.

---

## Next Steps

1. **Quantify the frequency offset distribution** across the UN population by
   computing the centroid frequency of each UN burst's spectrum and comparing
   against the nominal channel center. This would directly measure the magnitude
   of the alignment failure and inform the correlator search window adjustment.

2. **Widen the UW correlator search parameters** in `qpsk_demod.c` and measure
   the recovery rate against a controlled capture where the ground truth (number
   of real Iridium bursts) is known.

3. **Characterize the spectral shape of UN bursts at scale** to establish a
   baseline distribution. Deviations from this baseline — not from zero — are
   the correct new-mode detection signal.

4. **Add 2K FFT support to CUDA_FFT** to enable direct visualization of
   1,910-sample burst files in the existing analysis workflow, removing the
   current dependency on external Python scripts for this population.

---

## Data

All burst IQ files and verbose logs from this investigation are retained on the
Elba feeder. The 40-file sample (20 DL, 20 UN) used for spectral analysis is
available in `/home/mike/burst_sample/` on the feeder.

Raw capture files (`irdm_01.raw`) are available on the dev laptop for further
analysis pending clarification of capture parameters.
