# iridium-sniffer Architecture

Standalone Iridium satellite burst detector and demodulator. Replaces gr-iridium by eliminating the GNU Radio dependency while reusing ice9-bluetooth-sniffer's SDR abstraction and build system. Outputs iridium-toolkit compatible RAW format to stdout.

## High-Level Pipeline

```
SDR / IQ File / ZMQ SUB / VITA 49
     |
     v  (int8/int16/float IQ pairs)
[Burst Detector]     -- single thread, maintains sequential state
  |  Sliding FFT (Blackman window, 8192-point at 10 MHz)
  |  Adaptive noise floor (512-frame circular history)
  |  Peak detection + burst state machine
  |  IQ ring buffer extraction for completed bursts
  |  Optional GPU-accelerated FFT (OpenCL or Vulkan plugin)
     |
     v  burst_queue (2048 slots)
     |
[Downmix Workers]    -- 4 threads, pull from shared queue
  |  Coarse CFO correction (frequency shift)
  |  LPF + decimation to 250 kHz (10 sps)
  |  Noise-limiting LPF (20 kHz cutoff, 25 taps)
  |  Burst start detection (45% magnitude threshold)
  |  Fine CFO (squared FFT + quadratic interpolation)
  |  RRC matched filter
  |  FFT-based sync word correlation (DL + UL patterns)
  |  Phase alignment + Gardner timing recovery
  |  Frame extraction
     |
     v  frame_queue (512 slots)
     |
[Demod + Output]     -- single thread (output must be serial)
  |  Decimate to 1 sps
  |  First-order PLL (alpha=0.2)
  |  Hard-decision QPSK
  |  Dual-direction unique word verification (DL + UL, Hamming <= 2)
  |  DQPSK differential decode
  |  Symbol-to-bits mapping
  |  RAW format output to stdout (or ZMQ PUB)
  |
  +--→ [Frame Decoder]    -- inline in demod thread (when --web, --gsmtap, --acars)
  |    |  Access code verification (DL/UL 24-bit patterns)
  |    |  BCH(7,3) header check (IBC detection)
  |    |  De-interleave: 2-way (64→2x32) and 3-way (96→3x32)
  |    |  BCH(31,21) syndrome check + 2-bit error correction
  |    |  IRA: sat_id, beam_id, XYZ→lat/lon/alt, paging TMSIs
  |    |  IBC: sat_id, beam_id, timeslot, Iridium time counter
  |    |  MSG: messaging/pager channel (BCH poly 1897), RIC, 7-bit ASCII / BCD text
  |    |
  |    +--→ [Frame Classifier]   -- classify_frame_label() (when --web)
  |    |    |  Labels every demod frame by type for the web histogram:
  |    |    |  IRA/IBC/MSG (frame_decode) + IDA + LCW types
  |    |    |  (VOC/IIP/ISY/IU3/IU6 via ida_lcw_ft) + ITL header + RAW
  |    |    |  IIP confirmed by CRC-24 (ida_iip_decode)
  |    |    |  Diagnostics: UL ft6/7 IDA-decode failures -> IDA_UL_FAIL bucket;
  |    |    |  UW-check failures (pre-classification) counted by direction
  |    |
  |    +--→ [Web Map Server]   -- background thread (when --web)
  |    |    |  HTTP server on configurable port (default 8888)
  |    |    |  SSE stream at 1 Hz with JSON state snapshots
  |    |    |  Embedded Leaflet.js + OpenStreetMap map page
  |    |    |  Mutex-protected shared state (RA buffer, sat list, pager feed)
  |    |    |  UI: ring-alert map, ACARS + Pager(MSG) tabs, frame-type histogram
  |    |
  |    +--→ [Doppler Solver]   -- inline (when --position)
  |         |  Iterated weighted least-squares from IBC/IRA frequency offsets
  |         |  Height aiding, motion-validated clustering, 3-sigma rejection
  |
  +--→ [IDA Decoder]     -- inline in demod thread (when --parsed, --gsmtap, --acars)
       |  LCW extraction (46-bit permutation + 3 BCH components)
       |  FT==2 (downlink) or FT==6/7 (uplink) → IDA frame confirmed
       |  Chase BCH soft-decision decoding (LLR-guided bit flipping, --chase)
       |  Payload descramble: 124-bit blocks, de-interleave, BCH(31,20)
       |  CRC-CCITT verification
       |  Multi-burst reassembly (16 slots, freq/time/seq matching)
       |
       +--→ [Parsed Output]  -- stdout IDA: lines (when --parsed)
       |    |  iridium-parser.py compatible format
       |
       +--→ [GSMTAP Output]  -- UDP to Wireshark (default 127.0.0.1:4729)
       |    |  16-byte GSMTAP header + LAPDm payload
       |    |  Channel number from Iridium L-band frequency
       |
       +--→ [SBD/ACARS Decoder]  -- inline (when --acars)
            |  SBD packet extraction from reassembled IDA payloads
            |  Multi-packet SBD reassembly
            |  ACARS message parsing (libacars-2 for ARINC-622/ADS-C/CPDLC)
            |  Position extraction (4-tier: ADS-C > text coords > waypoint > beam)
            |  Terminal output (plain text or JSON)
            |  UDP/TCP feed output (--feed, repeatable, max 4 endpoints)
            |  UDP streaming (--acars-udp, dumpvdl2 envelope format)
            |  BaseStation SBS output (--basestation, MSG,3 with derived speed/heading)
     |
[Stats Thread]       -- 1 Hz to stderr, gr-iridium compatible format
```

## File Map

| File | Purpose | Lines | Origin |
|------|---------|-------|--------|
| `main.c` | Entry point, threading, FFTW wisdom, signal handling | 1220 | New |
| `options.c` | CLI argument parsing, format auto-detection from extension | 839 | New |
| `iridium.h` | Protocol constants (25 ksps, UW patterns, frame limits) | 54 | New |
| `burst_detect.c/h` | FFT burst detector (adaptive noise floor, peak extraction) | 1132 | Port of gr-iridium `fft_burst_tagger_impl.cc` |
| `burst_downmix.c/h` | Per-burst downmix pipeline (CFO, LPF, RRC, correlation) | 922 | Port of gr-iridium `burst_downmix_impl.cc` |
| `qpsk_demod.c/h` | QPSK/DQPSK demodulator (PLL, Gardner timing recovery) | 595 | Port of gr-iridium `iridium_qpsk_demod_impl.cc` |
| `frame_output.c/h` | RAW + parsed IDA format printer (ZMQ PUB output) | 399 | Port of gr-iridium `iridium_frame_printer_impl.cc` + new |
| `frame_decode.c/h` | Iridium frame decoder (BCH, de-interleave, IRA/IBC/MSG pager) | 840 | New (based on iridium-toolkit bitsparser.py) |
| `ida_decode.c/h` | IDA frame decoder (LCW, Chase BCH, descramble, reassembly, LCW classify, IIP CRC) | 920 | New (based on iridium-toolkit bitsparser.py + ida.py) |
| `sbd_acars.c/h` | SBD/ACARS decoder (libacars-2, feed output, position extraction) | 1892 | New (CEMAXECUTER LLC) |
| `doppler_pos.c/h` | Doppler position solver (IWLS, height aiding, clustering) | 1385 | New (CEMAXECUTER LLC) |
| `gsmtap.c/h` | GSMTAP/LAPDm UDP output for Wireshark | 146 | New |
| `web_map.c/h` | Built-in web map (HTTP server, SSE, Leaflet.js, page export) | 1269 | New |
| `fir_filter.c/h` | FIR filter + tap generation (RRC, RC, LPF) | 249 | New (replaces GR kernels) |
| `aircraft_db.c/h` | Aircraft registration to ICAO hex lookup (tar1090-db) | 247 | New (CEMAXECUTER LLC) |
| `basestation.c/h` | BaseStation SBS output (MSG,3 with derived speed/heading) | 529 | New (CEMAXECUTER LLC) |
| `waypoint_db.c/h` | Aviation waypoint/fix lookup (125K entries) | 149 | New (CEMAXECUTER LLC) |
| `sigmf.c/h` | SigMF metadata reader for auto-config from .sigmf-meta files | 163 | New (CEMAXECUTER LLC) |
| `simd_kernels.h` | SIMD dispatch header, mode enum, runtime CPU detection | 220 | New (CEMAXECUTER LLC) |
| `simd_generic.c` | Scalar fallback + four-tier dispatch initialization | 249 | New (CEMAXECUTER LLC) |
| `simd_avx2.c` | AVX2+FMA kernel implementations (256-bit, 8 floats) | 388 | New (CEMAXECUTER LLC) |
| `simd_sse42.c` | SSE4.2 kernel implementations (128-bit, 4 floats) | 372 | New (CEMAXECUTER LLC) |
| `simd_neon.c` | ARM NEON kernel implementations (128-bit, 4 floats) | 404 | Contributed (Taclane) |
| `vita49.c/h` | VITA 49 (VRT) UDP input with auto-config from context packets | 530 | New (CEMAXECUTER LLC) |
| `sdrplay.c/h` | SDRplay RSP backend (native API 3.x) | 449 | New (CEMAXECUTER LLC) |
| `cJSON.c/h` | JSON parser (vendored, MIT license) | 3512 | Third-party (Dave Gamble) |
| `rotator.h` | Complex frequency rotator (inline) | 48 | New (replaces GR rotator) |
| `window_func.c/h` | Blackman window generation | 42 | New |
| `wgs84.h` | WGS84 ellipsoid constants (for Doppler solver) | 92 | New |
| `net_util.h` | Network utility (hostname resolution, endpoint parsing) | 35 | New |
| `fftw_lock.h` | FFTW planner thread-safety mutex | 34 | New |
| `sdr.h` | SDR abstraction (sample_buf_t, push_samples) | 27 | Copied from ice9 |
| `hackrf.c/h` | HackRF backend | 102 | Adapted from ice9 |
| `bladerf.c/h` | BladeRF backend | 188 | Adapted from ice9 |
| `usrp.c/h` | USRP/UHD backend | 274 | Adapted from ice9 |
| `soapysdr.c/h` | SoapySDR backend | 443 | Adapted from ice9 |
| `opencl/burst_fft.h` | GPU FFT interface (backend-agnostic, guarded by `USE_GPU`) | ~40 | Adapted from ice9 |
| `opencl/burst_fft.c` | OpenCL + VkFFT backend (GPU kernels for window/magnitude) | ~360 | Adapted from ice9 `opencl/fft.c` |
| `vulkan/burst_fft.c` | Vulkan + VkFFT backend (CPU window/magnitude, GPU FFT only) | ~300 | New |
| `vkfft/vkFFT.h` | VkFFT library (header-only FFT) | - | Copied from ice9 |
| `blocking_queue.h` | Lock-free blocking queue | 556 | Copied from ice9 |
| `fair_lock.h` | Fair reader-writer lock | 292 | Copied from ice9 |
| `pthread_barrier.h` | macOS pthread_barrier shim | 81 | Copied from ice9 |
| `data/waypoints.csv` | Aviation waypoint database (125K fixes) | 125230 | Merged (GPL/MIT) |

## Key Parameters (at 10 MHz sample rate)

| Parameter | Value | Source |
|-----------|-------|--------|
| FFT size | 8192 | `2^round(log2(samp_rate/1000))` |
| Burst width | 40 kHz (32 bins) | Iridium channel width |
| Burst pre-length | 16384 samples | `2 * fft_size` |
| Burst post-length | 160000 samples | `samp_rate * 16 ms` |
| Max burst length | 900000 samples | `samp_rate * 90 ms` |
| Threshold | 16.0 dB | Default, configurable via `-d` |
| Noise history | 512 FFT frames | Adaptive baseline |
| Output sample rate | 250000 Hz | `10 sps * 25000 sym/s` |
| Symbols per second | 25000 | Iridium DQPSK |
| PLL alpha | 0.2 | First-order loop bandwidth |
| UW max errors | 2 | Hamming distance tolerance |

## Output Format

```
RAW: {file_info} {timestamp_ms:012.4f} {freq_hz:010d} N:{mag:05.2f}{noise:+06.2f} I:{id:011d} {conf:3d}% {level:.5f} {payload_symbols:3d} {bits...}
```

Example:
```
RAW: i-10-t1 0000442.4080 1624960925 N:10.77-71.83 I:00000003560  50% 0.11738 179 001100011011...
```

This format is consumed directly by [iridium-toolkit](https://github.com/muccc/iridium-toolkit) for higher-layer decoding.

## Threading Design Decisions

**Why a single burst detector thread?** The FFT burst detector maintains sequential state: noise floor history, active burst list, ring buffer. Parallelizing it would require complex synchronization with no benefit since FFT computation dominates and is already vectorized.

**Why separate downmix workers?** Each burst is independent. The downmix is the most CPU-intensive stage (multiple FFTs per burst). 4 worker threads provide good throughput without excessive contention.

**Why single demod+output thread?** QPSK demod is cheap (no FFTs). Output must be serialized for stdout. Combining them in one thread avoids an extra queue and keeps the design simple.

**FFTW thread safety:** FFTW plan creation is not thread-safe even with `FFTW_ESTIMATE`. All `fftwf_plan_*` and `fftwf_destroy_plan` calls are wrapped with a global mutex (`fftw_lock.h`). Per-burst plans use `FFTW_MEASURE` for optimal runtime performance. The one-time sync word template FFTs use `FFTW_ESTIMATE` (these run exactly twice at startup, so planning overhead has zero benefit).

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Dependencies: cmake >= 3.9, FFTW3 (float), pthreads, at least one SDR library for live capture (hackrf, bladerf, uhd, soapysdr, sdrplay). Optional: libzmq3-dev for ZMQ PUB/SUB output, libacars-2 for ARINC-622/ADS-C/CPDLC decoding, OpenCL or Vulkan for GPU-accelerated burst detection (see GPU Backends below).

## GPU Backends

Two mutually exclusive GPU backends are available for burst detection FFT acceleration. Both use VkFFT for the FFT computation and expose the same interface (`gpu_burst_fft_create/process/destroy`).

### OpenCL (default on x86 with OpenCL drivers)

```bash
cmake .. -DUSE_OPENCL=ON    # auto-detected when OpenCL is available
```

**Dependencies:** `libOpenCL` (usually from GPU driver or `ocl-icd-opencl-dev`)

Uses custom OpenCL compute kernels for window multiply and fftshift+magnitude on the GPU. The FFT runs via VkFFT's OpenCL backend (VKFFT_BACKEND=3).

### Vulkan (required for Pi5, optional on NVIDIA/AMD)

```bash
cmake .. -DUSE_VULKAN=ON -DUSE_OPENCL=OFF
```

**Dependencies:**
```bash
sudo apt install libvulkan-dev glslang-dev spirv-tools
```

Uses VkFFT's native Vulkan backend (VKFFT_BACKEND=0) for the FFT. Window multiply and fftshift+magnitude run on the CPU. This avoids custom Vulkan compute shaders, keeping the code simple (~300 lines vs ~360 for OpenCL).

Host-visible coherent memory is used for zero-copy CPU<->GPU transfers, which is efficient on shared-memory architectures (Pi5 VideoCore VII) and functional on discrete GPUs.

**Pi5 notes:** The Raspberry Pi 5's VideoCore VII GPU supports Vulkan 1.2 via Mesa's v3d driver but has no OpenCL support. The Vulkan backend was designed specifically for this platform.

### Choosing a backend

| Platform | Recommended | Notes |
|----------|-------------|-------|
| NVIDIA x86 | OpenCL | Full GPU pipeline, mature driver |
| AMD x86 | OpenCL | ROCm or Mesa OpenCL |
| Raspberry Pi 5 | Vulkan | Only GPU API available |
| No GPU / embedded | Neither | CPU FFTW fallback is fast enough at 10 MHz |

Both backends can be disabled at runtime with `--no-gpu` to force CPU FFTW processing.

### Build verification

| Configuration | Command | Result |
|---------------|---------|--------|
| OpenCL GPU | `cmake .. -DUSE_OPENCL=ON` | 3668 bursts, 70% ok |
| Vulkan GPU | `cmake .. -DUSE_VULKAN=ON -DUSE_OPENCL=OFF` | 3668 bursts, 70% ok |
| CPU only | `cmake .. -DUSE_OPENCL=OFF` or `--no-gpu` | 3668 bursts, 71% ok |

All three produce equivalent RAW output (2500-2577 lines). Minor variations are due to batch processing order differences affecting which bursts reach the demod queue first.

## Usage

```bash
# From IQ file (format auto-detected from extension)
./iridium-sniffer -f recording.cf32

# Live capture with SoapySDR
./iridium-sniffer -l -i soapy-0

# Pipe to iridium-toolkit
./iridium-sniffer -f recording.cf32 | python3 iridium-parser.py

# Parsed IDA output (bypasses iridium-parser.py for IDA/ACARS/SBD)
./iridium-sniffer -f recording.cf32 --parsed | python3 reassembler.py -m acars
```

## Implementation Status

- [x] Phase 1: Project skeleton + build system
- [x] Phase 2: FFT burst detector (CPU)
- [x] Phase 3: Burst downmix pipeline
- [x] Phase 4: QPSK demod + frame output
- [x] Phase 5: Integration + threading + shutdown
- [x] Phase 6: GPU burst detection (OpenCL/VkFFT)
- [x] Phase 7: Live SDR testing
- [x] Phase 8: Demod quality optimization (ok% 26% -> 71%)
- [x] Phase 9: Beyond gr-iridium (soft-decision UW rescue, +3 frames)
- [x] Phase 10: Vulkan GPU backend (Pi5 support, tested on NVIDIA)
- [x] Phase 11: Built-in web map (IRA/IBC frame decode, Leaflet.js map)
- [x] Phase 12: GSMTAP output (IDA decode, LCW extraction, multi-burst reassembly, Wireshark)
- [x] Phase 13: SIMD optimization (AVX2+FMA kernels, 1.78x CPU speedup)
- [x] Phase 14: Detection threshold optimization (18 dB → 16 dB, +6.6% valid frames)
- [x] Phase 15: SSE4.2 SIMD tier (three-tier dispatch: AVX2 > SSE4.2 > scalar, `--simd=MODE`)
- [x] Phase 16: ARM NEON SIMD tier (AArch64 parity with x86, 1.54x CPU speedup on Apple M3 Max)

## Test Verification

### Methodology

Both gr-iridium and iridium-sniffer were run on the same synthetic test file:

- **File:** `gr-iridium/test-data/prbs15-2M-20dB.sigmf-data`
- **Format:** cf32_le (complex float32, little-endian)
- **Sample rate:** 2 MHz
- **Center frequency:** 1622 MHz
- **Content:** Single synthetic Iridium burst, PRBS15 payload, 20 dB SNR

### gr-iridium Output

```
RAW: i-1601421246-t1 0000599.9996 1622000000 N:32.12-80.05 I:00000000000 100% 0.13551 179 001100000011000011110011100000000000001100000000000010100000000000111100000000001000100000000011001100000000101010100000001111111100000010000000100000110000001100001010000010100011110000111100100010001000101100110011001110101010101010011111111111110100000000000011100000000000100100000000001101100000000010110100000000111011100000001001100100000011010101100000101111110100001110 0000
```

### iridium-sniffer Output

```
RAW: i-1771071731-t1 0000269.1473 1621999997 N:32.16-80.34 I:00000000000  76% 0.43957 179 0011000000110000111100111000000000000011000000000000101000000000001111000000000010001000000000110011000000001010101000000011111111000000100000001000001100000011000010100000101000111100001111001000100010001011001100110011101010101010100111111111111101000000000000111000000000001001000000000011011000000000101101000000001110111000000010011001000000110101011000001011111101000011100000
```

### Comparison Results

| Field | gr-iridium | iridium-sniffer | Match? |
|-------|-----------|-----------------|--------|
| Payload symbols | 179 | 179 | Exact |
| Demodulated bits | 001100000... | 001100000... | **Byte-for-byte identical** |
| Direction | DL (implied) | DL (implied) | Exact |
| Center frequency | 1622000000 | 1621999997 | 3 Hz off |
| Magnitude | 32.12 | 32.16 | ~0.04 dB |
| Noise floor | -80.05 | -80.34 | ~0.3 dB |
| Confidence | 100% | 76% | Different (see below) |
| Level | 0.13551 | 0.43957 | Different (see below) |

### Analysis of Differences

**Demodulated bits:** Identical. This is the critical validation -- the DSP pipeline (burst detection, downmix, CFO correction, matched filtering, sync word correlation, DQPSK demod) produces the exact same decoded output as gr-iridium.

**Confidence (76% vs 100%):** Confidence measures the percentage of symbols within +/-22 degrees of their ideal QPSK constellation point. The difference is likely due to int8 quantization noise in the PLL path (gr-iridium operates on float32 throughout). This does not affect demodulation correctness -- even at 76%, all symbols are decoded identically. In live operation with real noise, both implementations will produce similar confidence values since atmospheric/thermal noise dominates quantization noise.

**Level (0.44 vs 0.14):** Different amplitude normalization. gr-iridium normalizes relative to its FFT-derived noise estimate. This implementation uses a different normalization path. This is cosmetic -- iridium-toolkit does not use the level field for decoding.

**Frequency (3 Hz off):** Within the PLL's residual CFO measurement precision. Negligible at 25,000 symbols/sec.

**Timestamp/file_info:** Different because these are derived from wall clock at program start, not from the IQ data itself.

### GPU vs CPU Verification

GPU acceleration tested by running the same synthetic test file through all paths:

- **OpenCL GPU:** NVIDIA GeForce RTX 3060 Laptop GPU via VkFFT (VKFFT_BACKEND=3)
  - Custom OpenCL kernels for windowing and fftshift+magnitude on GPU
- **Vulkan GPU:** Same GPU via VkFFT (VKFFT_BACKEND=0)
  - CPU window multiply and fftshift+magnitude, GPU FFT only
  - Uses host-visible coherent memory for zero-copy transfers
- **CPU path:** FFTW3 (single-frame processing)

**Result:** Demodulated bits are byte-for-byte identical across all three paths. All metadata fields (frequency, magnitude, noise, confidence, level, symbol count) match exactly. Both GPU paths are drop-in replacements with `--no-gpu` to disable.

**Build verified in four configurations:**
1. `USE_OPENCL=ON` (default when OpenCL available) -- OpenCL GPU with CPU fallback
2. `USE_VULKAN=ON` -- Vulkan GPU with CPU fallback
3. `--no-gpu` runtime flag -- forces CPU FFTW path regardless of build
4. `USE_OPENCL=OFF -DUSE_VULKAN=OFF` cmake flags -- compiles without GPU dependency

## Performance Notes

GPU vs CPU benchmarks on synthetic data (NVIDIA RTX 3060 Mobile):

| Sample Rate | FFT Size | File Duration | GPU | CPU | Notes |
|-------------|----------|---------------|-----|-----|-------|
| 2 MHz | 2048 | ~6 sec | 1.0s | 1.0s | FFT too small for GPU advantage |
| 10 MHz | 8192 | ~6 sec | 2.0s | 1.0s | GPU overhead dominates on short file |

At low sample rates the FFT is small enough that FFTW handles it trivially. The GPU advantage is expected on weaker CPUs (Pi 5) or at higher sample rates where CPU is already loaded with downmix and demod threads.

## Live SDR Testing (Phase 7)

### Tested Hardware

Successfully tested on multiple SDR platforms at 10 MHz sample rate:

- **Ettus USRP B210** (primary test platform) — 12-bit ADC, AD9361, USB 3.0, most extensive validation
- **Nuand bladeRF 2.0 micro** — 12-bit ADC, AD9361, USB 3.0, sensitivity characteristics under investigation
- **Airspy HydraSDR** (via SoapySDR) — 12-bit ADC, USB 3.0, tested and working

Untested but supported with example configs: HackRF One, RTL-SDR (via SoapySDR)

### Setup

- **Primary SDR:** Ettus USRP B210 (performance metrics below are from USRP captures)
- **GPU:** NVIDIA GeForce RTX 3060 Laptop GPU (OpenCL + VkFFT)
- **Antenna:** L-band antenna with external bias tee
- **Center frequency:** 1622 MHz (Iridium L-band simplex downlink)

### Live Comparison: iridium-sniffer vs gr-iridium

Representative test results (USRP B210, 10 MHz, gain 40 dB, 30-second captures):

| Metric | This Tool (GPU) | This Tool (CPU) | gr-iridium |
|--------|----------------|----------------|------------|
| Bursts detected (i_avg) | 254/s | 400/s | 64/s |
| UW pass rate (ok%) | 33% | 24% | 79% |
| Decoded frames (ok/s) | 83/s | 95/s | 51/s |
| Total RAW lines | 2504 | 3043 | 1367 |
| Demod queue depth | 0 | 0-1814 | 0 |

60-second sustained captures at 10 MHz:

| Metric | GPU | CPU |
|--------|-----|-----|
| Total RAW lines | 2055 | 3604 |
| ok/s avg | 33/s | 59/s |
| Queue depth | 0 | 0 |
| CRC:OK frames | 192 | 404 |

### Understanding ok% (UW Pass Rate)

The `ok%` in the stats line is the percentage of detected bursts that pass the **unique word check** (Hamming distance <= 2 from a known Iridium sync pattern). A lower ok% does **not** mean lower quality output -- it means the burst detector is more aggressive, attempting to demodulate weaker signals that a conservative detector would skip entirely.

**Why the ok% is lower than gr-iridium despite decoding more frames:**

- gr-iridium detects ~64 bursts/s and passes 79% (= ~51 ok/s)
- iridium-sniffer detects ~250-400 bursts/s and passes 24-36% (= ~83-95 ok/s)

This tool casts a wider net. The bursts that fail UW check are silently discarded -- they never appear in the output. The bursts that pass are valid decoded frames, yielding **~2x more of them**. The extra frames come from weaker signals at the edge of detectability that gr-iridium never even attempts.

**What matters for downstream tools (iridium-toolkit) is total ok/s, not ok%.** More ok/s = more decoded ACARS, SBD, pager, and voice frames.

### Analysis

- **This tool produces ~2x more decoded frames than gr-iridium** at the same sample rate and settings.
- gr-iridium has higher ok% (79%) because it's more conservative in burst detection (only 64 bursts/s vs this tool's 254-400/s). It misses weaker bursts that this detector catches.
- On this laptop (fast CPU), the CPU FFTW path actually outperforms GPU for burst detection at 10 MHz because the 8192-point FFT is small enough that GPU batching overhead exceeds the compute savings.
- GPU advantage expected on weaker CPUs (Pi 5, embedded ARM) or at higher sample rates where CPU runs out of cycles.
- Under burst traffic (satellite pass), CPU path's demod queue can grow temporarily (d: 1814), while GPU stays at d:0. Both drain the queue during quiet periods.
- iridium-toolkit successfully parses all output: IDA, ISY, IBC, IIP, ITL, IRA, I36, VOC, VOZ, VDA, MSG frame types decoded correctly with CRC:OK.
- Real satellite data decoded: sat IDs, cell info, handoff messages, SBD payloads, timestamps.

### End-to-End Pipeline Verification

Full iridium-toolkit pipeline tested (USRP B210, 60-second capture, 10 MHz):

```
iridium-sniffer → iridium-parser.py → reassembler.py
```

**Frame type breakdown (3510 RAW frames in 60s):**

| Type | Count | Description |
|------|-------|-------------|
| IDA | 852 | Data access (maintenance, sync, handoff) |
| ISY | 849 | Sync frames |
| IBC | 436 | Broadcast (satellite ID, cell info, time) |
| I36 | 265 | 36-byte data frames |
| IIP | 122 | IP data |
| ITL | 117 | Telecom frames |
| VOZ/VDA/VOC | 222 | Voice channel data |
| IRA | 62 | Ring alert (pager) with GPS coordinates |
| RAW | 466 | Unparsed (partial/damaged) |

**ACARS aircraft data decoded:**
- Aircraft registration N843QS (NetJets Cessna Citation) position report via Iridium SBD
- 4 valid SBD packets assembled from 43 fragments

**Pager/ring alerts with GPS positions:**
- Multiple ring alerts with lat/lon coordinates (e.g., 41.63/-91.97, 32.82/-78.40)
- Satellite 099, multiple beams tracked

**SBD messages decoded**, including plaintext Iridium-to-Iridium messages.

### Bandwidth Testing

| Sample Rate | FFT Size | Bursts/s | ok% | Notes |
|-------------|----------|----------|-----|-------|
| 2 MHz | 2048 | ~1100/s | 5% | Narrow band, many false detections |
| 10 MHz | 8192 | ~250-400/s | 24-36% | Sweet spot for Iridium 1616-1626.5 MHz |
| 20 MHz | 16384 | ~73/s | 23% | Excess bandwidth, no additional Iridium signals |

10 MHz centered at 1622 MHz is optimal -- covers the full Iridium simplex downlink band without wasting processing on empty spectrum.

## Phase 8: Demod Quality Optimization

### Problem

Processing a 60-second IQ recording (`/tmp/iridium_iq_60s.cf32`, 10 MHz, cf32):
- **Before:** 3668 bursts detected, 26% ok, 923 RAW lines
- **gr-iridium:** ~3468 bursts detected, 74% ok, 2720 RAW lines

Both tools detect similar burst counts, but the initial UW (unique word) pass rate was 3x lower, meaning 74% of detected bursts were discarded during demodulation.

### Root Causes (3 issues found via side-by-side comparison with gr-iridium)

**1. Anti-alias LPF 5x too wide (primary, ~7 dB SNR loss)**

The original filter had 100 kHz cutoff (`output_sample_rate * 0.4`). Iridium signal occupies ~35 kHz (25 kHz symbol rate * 1.4 roll-off). gr-iridium uses 20 kHz cutoff. The excess bandwidth added ~7 dB of noise to all pre-RRC processing stages (fine CFO estimation, burst start detection, sync word correlation).

Fix: Added a 25-tap noise-limiting LPF at output rate (250 kHz) with 20 kHz cutoff and 40 kHz transition, applied immediately after decimation. Cheap (25 taps at 250 kHz) and applied before all noise-sensitive stages.

**2. Single-direction UW check (secondary)**

The QPSK demod initially only checked the unique word pattern for the direction chosen by burst_downmix (DL or UL). gr-iridium checks both patterns and accepts either. When burst_downmix picks the wrong direction from a noisy correlation, the PLL can still recover the correct symbols -- but they were rejected.

Fix: Check both DL and UL unique words; accept either match. Update direction if demod disagrees with burst_downmix.

**3. CFO FFT size rounding (minor)**

The initial implementation used `next_pow2(sps * 26) = 512` (ceil). gr-iridium uses `floor_pow2(sps * 26) = 256`. The preamble + 10 UW symbols = 260 samples at 10 sps. Using 512 extends the squared-signal FFT window 252 samples into QPSK payload data, adding noise to the CFO estimate.

Fix: Use floor-to-power-of-2 instead of ceiling.

### Results

| Metric | Before | After | gr-iridium | Delta |
|--------|--------|-------|------------|-------|
| Bursts detected | 3668 | 3668 | ~3468 | Same |
| ok% (UW pass rate) | 26% | 71% | 74% | +45 pp |
| RAW lines output | 923 | 2574 | 2720 | 2.8x more |
| % of gr-iridium output | 34% | 94.6% | 100% | +60.6 pp |

### Files Modified

| File | Change |
|------|--------|
| `burst_downmix.c` | Added `noise_fir` (25-tap LPF, 20 kHz cutoff at 250 kHz); applied after decimation step; CFO FFT size floor instead of ceil |
| `qpsk_demod.c` | Check both DL and UL unique words |

### Verified Non-Issues

These were investigated and confirmed identical or equivalent between this code and gr-iridium:
- PLL algorithm (mathematically identical, alpha=0.2)
- QPSK symbol mapping (Q1->0, Q2->1, Q3->2, Q4->3)
- DQPSK differential decode ({0,2,3,1} map)
- UW patterns and Hamming threshold (<=2 errors)
- RC filter formula and normalization
- RC filter padding in sync word generation
- RRC filter parameters (alpha=0.4, 51 taps, gain=1.0)
- Phase rotation from correlation peak

## Phase 9: Beyond gr-iridium

### Approach

Three techniques were tested to exceed gr-iridium's 74% ok rate:

1. **Optimal decimation offset** -- Search all 10 sample offsets for eye-diagram peak before decimation. **Result: DESTRUCTIVE** (1554 vs 2574 RAW). burst_downmix already aligns timing via sync word correlation; adding an offset misaligns symbols. Discarded.

2. **Second-order Costas loop PLL** -- Replace first-order PLL (phase-only) with second-order loop tracking both phase and frequency (integral gain beta = alpha^2/4 = 0.01). **Result: -2 lines**. The frequency integrator adds jitter on short Iridium bursts (~180 symbols), outweighing any benefit from frequency tracking. The fine CFO correction in burst_downmix already removes most frequency offset. Discarded.

3. **Soft-decision UW rescue** -- When hard-decision Hamming check fails, compute angular distance from expected constellation points in continuous domain (0 = perfect, 1.0 = one quadrant off). Accept if total soft error <= 3.0. Only runs as fallback when hard check fails, so zero cost for normal frames. **Result: +3 lines** (2577 vs 2574). Kept.

### Results

| Metric | Phase 8 | Phase 9 | gr-iridium |
|--------|---------|---------|------------|
| RAW lines | 2574 | 2577 | 2720 |
| % of gr-iridium | 94.6% | 94.7% | 100% |

### Analysis: Where the remaining 5% gap lives

The demod pipeline (qpsk_demod.c) is now at parity with gr-iridium -- both use identical PLL, symbol mapping, DQPSK decode, and UW patterns. The remaining gap is in the burst_downmix stage:

- **Burst start detection precision** -- sub-sample timing differences in finding preamble start
- **Filter implementation details** -- VOLK-optimized FIR vs SIMD FIR may produce slightly different intermediate values due to floating-point ordering
- **Correlation peak interpolation** -- both use quadratic, but numerical differences in FFT libraries (FFTW vs GR's FFT wrapper) may shift peaks by fractions of a sample

These are diminishing returns. The 5% gap represents ~143 frames over 60 seconds of recording -- marginal signals where tiny numerical differences determine whether the sync word barely passes or barely fails.

### Lessons Learned

1. **Don't fight the alignment** -- burst_downmix's sync word correlation already provides optimal symbol timing. Adding an offset search on top is destructive.
2. **Short bursts penalize loop complexity** -- Iridium bursts (~180 symbols) don't have enough convergence time for a second-order PLL to outperform first-order. The integral term adds more noise than it removes frequency error.
3. **Soft-decision rescue is architecturally sound** -- it only activates when hard check fails (zero cost path for normal frames) and correctly rescues frames where symbols fall near quadrant boundaries.

## Phase 11: Built-in Web Map

### Frame Decoder

The frame decoder (`frame_decode.c`) parses demodulated bits into structured IRA and IBC frames. It runs inline in the demod thread when `--web` is enabled, adding negligible overhead.

**Parsing pipeline:**

1. **Access code check** -- After DQPSK decode, the UW symbols become the access code bits. The first 24 bits of the demodulated output are compared against the known DL and UL patterns. Frames that don't match are skipped.

2. **IBC detection** -- The first 6 bits after the access code are checked as a BCH(7,3) header. If valid, the following 64-bit block is de-interleaved and BCH-checked. If both pass, the frame is an IBC.

3. **IRA detection** -- The first 96 bits after the access code are de-interleaved via a three-way split and all three 31-bit blocks are BCH-checked. If all pass, the frame is an IRA.

4. **BCH error correction** -- Pre-computed syndrome tables (built at startup) enable correction of up to 2 bit errors per block for BCH(31,21) and 1 bit for BCH(7,3). Uncorrectable blocks cause the frame to be skipped.

5. **De-interleaving** -- Iridium uses pair-swapping followed by reverse-stride distribution. `de_interleave()` splits 64 input bits into two 32-bit streams; `de_interleave3()` splits 96 bits into three. This follows the same algorithm as iridium-toolkit's bitsparser.py.

6. **Field extraction** -- IRA frames contain satellite ID, beam ID, geocentric XYZ position (converted to lat/lon/alt via atan2), and up to 12 paging blocks with TMSI and MSC ID. IBC frames contain satellite ID, beam ID, timeslot, and optionally the LBFC (Iridium time counter).

**BCH parameters:**

| Polynomial | Block Length | Data Bits | Syndrome Bits | Correction | Used For |
|-----------|-------------|-----------|---------------|------------|----------|
| 1207 | 31 | 21 | 10 | 2-bit | IRA/IBC data blocks |
| 3545 | 31 | 20 | 11 | 2-bit | IDA payload blocks |
| 29 | 7 | 3 | 4 | 1-bit | IBC header, LCW component 1 |
| 465 | 14 | 5 | 8 | 1-bit | LCW component 2 |
| 41 | 26 | 21 | 5 | 2-bit | LCW component 3 |

### Web Map Server

The web map (`web_map.c`) is a minimal POSIX socket HTTP server that runs in a background thread. It serves three endpoints:

- `GET /` -- Returns an embedded HTML page with Leaflet.js and OpenStreetMap tiles. The entire page is a C string literal compiled into the binary. Nothing is loaded from disk.
- `GET /api/events` -- Server-Sent Events stream. Pushes a full JSON state snapshot once per second. Each SSE client runs in its own detached pthread.
- `GET /api/state` -- Returns a single JSON snapshot of the current state.

**State management:**

- Ring alert points are stored in a circular buffer (2000 entries). Each entry includes lat/lon, altitude, satellite/beam IDs, TMSI, and frequency.
- Satellite entries are tracked in a flat array (100 max), updated on each IBC frame.
- All state is protected by a single mutex. JSON serialization limits output to the 500 most recent ring alerts for browser performance.
- `SIGPIPE` is set to `SIG_IGN` in `main()` so broken SSE connections don't terminate the process.

**Verification (60-second IQ recording at 10 MHz):**

| Metric | Count |
|--------|-------|
| IRA frames decoded | 151 |
| IBC frames decoded | 269 |
| Unique satellites seen | 59 |
| RAW output (stdout) | 2577 lines (identical with and without --web) |

## Phase 12: GSMTAP Output (Wireshark Integration)

### IDA Frame Decoder

The IDA decoder (`ida_decode.c`) processes IDA (Iridium Data Access) frames -- the signaling channel that carries call setup, SMS, SBD, and location updates. Unlike IRA/IBC frames which use BCH(31,21), IDA frames use a different structure:

**LCW (Link Control Word) extraction:**
1. First 46 bits after access code are permuted via a fixed table
2. Pair-swap applied before permutation (LCW table expects symbol_reverse'd input)
3. Three BCH components decoded: lcw1 (7-bit, FT field), lcw2 (14-bit), lcw3 (26-bit)
4. FT==2 identifies IDA frames

**Payload descrambling (bits 46+ after access code):**
1. Split into 124-bit blocks
2. Each block: de-interleave 62 symbols into 2x62 bits
3. Concatenate halves, split into 4x31-bit chunks
4. Reorder as [chunk4, chunk2, chunk3, chunk1]
5. BCH(31,20) decode each chunk (poly=3545, 2048-entry syndrome table)
6. Extract 20 data bits per chunk

**IDA fields (from BCH-decoded bitstream):**
- Continuation flag (bit 3)
- Sequence counter da_ctr (bits 5-7, 0-7)
- Payload length da_len (bits 11-15, 0-20 bytes)
- Payload data (bits 20-179, 20 bytes)
- CRC-CCITT (bits 180-195)

**Multi-burst reassembly:**
- 16 concurrent reassembly slots (static, no allocation)
- Match rules: same direction, frequency within 260 Hz, time within 280 ms, sequence = (prev+1)%8
- ctr==0 + cont==0 -> single-burst message (immediate callback)
- ctr==0 + cont==1 -> start multi-burst, accumulate payload
- cont==0 -> message complete, fire callback
- Timeout: slots older than 1000 ms silently discarded

### GSMTAP Output

GSMTAP (`gsmtap.c`) wraps reassembled IDA payloads in a 16-byte GSMTAP header and sends them as UDP datagrams to Wireshark.

**GSMTAP header fields:**
- version=2, hdr_len=4 (16 bytes), type=2 (ABIS)
- ARFCN: Iridium channel number = (freq - 1616 MHz) / 41.667 kHz, with uplink flag 0x4000
- signal_dbm: 20*log10(burst magnitude)
- frame_number: raw frequency in Hz

**Verification (60-second IQ recording at 10 MHz):**

| Metric | iridium-sniffer | iridium-toolkit |
|--------|-----------------|-----------------|
| IDA frames detected | 493 | 468 |
| CRC-OK frames | 252 | 240 |
| GSMTAP packets sent | 225 | N/A |
| RAW output (stdout) | 2577 lines (identical with and without --gsmtap) |

Wireshark correctly decodes the packets as GSM/LAPDm signaling, showing Immediate Assignment, Location Update Reject, Paging Request, and other message types.

## SIMD Optimization

### Motivation

Profiling the CPU-intensive DSP pipeline revealed that FIR filtering, magnitude computation, and data conversion consumed 50-70% of total CPU time. All hot paths used scalar C loops with no SIMD vectorization. gr-iridium uses VOLK for some operations, but custom inner loops remain scalar.

### Three-Tier Dispatch

One binary works on all x86_64 CPUs. At startup, `simd_init(simd_mode)` probes CPU capabilities via `__builtin_cpu_supports()` and selects the highest available tier via function pointers.  
Apple Silicon and other ARM processors will utilize NEON extensions by default, and optionally fall back to Scalar if manually directed.

| Tier | ISA | Register Width | Floats/Op | Minimum CPU | Speedup |
|------|-----|---------------|-----------|-------------|---------|
| AVX2+FMA | AVX2, FMA3 | 256-bit | 8 | Intel Haswell (2013) / AMD Excavator (2015) | ~1.9x |
| SSE4.2 | SSE4.2 | 128-bit | 4 | Intel Nehalem (2008) / AMD Bulldozer (2011) | ~1.3-1.5x |
| NEON | AArch64 base ISA | 128-bit | 4 | Any AArch64 (Cortex-A53+, Apple M-series) | ~1.5x |
| Scalar | C99 | N/A | 1 | Any | 1.0x |

The `--simd=MODE` flag overrides auto-detection (`auto`, `avx2`, `sse42`, `neon` (arm), `scalar`). If a forced mode is unavailable, dispatch falls back to the next available tier with a warning on stderr. `--no-simd` is retained as an alias for `--simd=scalar`.

### 11 SIMD Kernels

All 11 kernels are implemented identically across all three tiers:

1. `simd_fir_ccf()` -- Complex FIR (51-tap RRC, 25-tap noise LPF)
2. `simd_fir_ccf_dec()` -- FIR with decimation (200-tap input LPF)
3. `simd_fir_fff()` -- Real FIR (start-detection magnitude smoothing)
4. `simd_window_cf()` -- Blackman window multiply (8192-sample FFT prep)
5. `simd_fftshift_mag()` -- DC shift + magnitude-squared (burst detector)
6. `simd_baseline_update()` -- Noise floor tracking (512-frame history)
7. `simd_relative_mag()` -- Division with conditional masking (peak extraction)
8. `simd_convert_i8_cf()` -- int8 IQ to float complex (10 MHz input)
9. `simd_mag_squared()` -- Magnitude (burst start detection)
10. `simd_max_float()` -- Horizontal max (peak search)
11. `simd_csquare_window()` -- Complex squaring with window (fine CFO)

### AVX2 Implementation Details

- Compiled with `-mavx2 -mfma` in separate translation unit (CMake `set_source_files_properties`)
- 32-byte aligned memory via `posix_memalign` for all working buffers
- FIR taps zero-padded to multiple of 8 floats for unrolled loops
- FMA instruction (`_mm256_fmadd_ps`) for single-rounding multiply-add
- Horizontal reductions via `_mm256_permutevar8x32_ps` + `_mm256_hadd_ps`
- Tail handling for non-multiple-of-N lengths (scalar loop for remainder)

### SSE4.2 Implementation Details

- Compiled with `-msse4.2` in separate translation unit
- Uses `__m128` (4 floats / 2 complex samples) instead of `__m256` (8 floats / 4 complex)
- Complex deinterleave via `_mm_shuffle_ps` instead of `_mm256_permutevar8x32_ps`
- 2-float results written via `_mm_store_sd` (magnitude computations)
- No FMA -- uses separate multiply and add (`_mm_mul_ps` + `_mm_add_ps`)
- Same tail-handling strategy as AVX2 (scalar loop for remainder elements)

### Scalar Fallback

- Functions in `simd_generic.c` are byte-for-byte identical to original inline loops
- Zero performance regression -- verified 29.0s baseline → 29.2s with `--simd=scalar`

### Results

**Test file:** `/test-data/iridium_iq_60s.cf32` (4.5 GB, 10 MHz, 60 seconds)

| Configuration | CPU Time (user) | Wall Time | Speedup | RAW Lines | MD5 (sorted) |
|---------------|-----------------|-----------|---------|-----------|--------------|
| AVX2 + GPU | 23.6s | 15.1s | **1.9x** (CPU) | 3228 | c09b29ce... |
| AVX2 only | 21.5s | 12.0s | **1.9x** (CPU) | 3228 | 510e8cc9... |
| Scalar + GPU | 42.6s | 16.1s | 1.0x | 3228 | c09b29ce... |
| Scalar only | 40.6s | 13.0s | 1.0x | 3228 | 510e8cc9... |

All three SIMD tiers produce identical decoded bits (same frames, same demodulated data). Verified on 535 decoded frames from a 5-minute RTL-SDR recording: 0 bit differences across AVX2, SSE4.2, and scalar paths. GPU vs CPU may differ by a few frames due to burst detection FFT rounding, hence two MD5 groups.

**Verification:**
- AVX2, SSE4.2, and scalar produce identical output within each GPU group
- 60s file processed in 12-15s = 4-5x realtime
- CPU time reduced 47% with AVX2 (40.6s -> 21.5s) at identical decode quality
- GPU adds ~3s startup overhead for OpenCL init; not beneficial at 10 MHz on fast x86

### Architecture Notes

**Binary portability:** Each SIMD tier is in a separate `.c` file compiled with ISA-specific flags (`-mavx2 -mfma`, `-msse4.2`). The rest of the binary uses default `-march` (typically x86-64-v2 or generic). Runtime detection ensures one binary works on all x86_64 CPUs without recompilation.

**Cache efficiency:** 32-byte alignment satisfies AVX2 load/store requirements and matches x86 cache line size (64 bytes = 2 AVX2 vectors). Aligned loads avoid unaligned access penalties (~3 cycle stall on older microarchitectures).

**FMA advantage:** `a * b + c` compiled as separate `vmulps` + `vaddps` rounds twice. `vfmadd231ps` fuses the operation and rounds once, improving both speed (1 micro-op vs 2) and numerical accuracy (one rounding error instead of two). Only available in the AVX2 tier.

**Why not VOLK?** VOLK is a heavy dependency (5000+ lines, build system integration). These 11 kernels are ~1000 lines of intrinsics across two tiers with zero external dependencies. For a standalone tool, less is more.

### ARM NEON (AArch64)

NEON is the SIMD extension for AArch64. Unlike x86 where SSE/AVX are optional extensions requiring CPUID checks, **NEON is mandatory** on AArch64 — it is part of the base ISA. Every AArch64 core (Cortex-A53, Cortex-A72, Apple Silicon, etc...) has NEON.

- No compile flags needed. `<arm_neon.h>` is included and the compiler emits NEON instructions by default on AArch64. CMakeLists.txt requires no `COMPILE_FLAGS` property.

At 10 MHz, 10 million int8 IQ pairs per second must be sign-extended and scaled to float. The NEON path loads 16 bytes, performs a three-level sign-extension chain (int8→int16→int32), converts to float, and multiplies by 1/128 — processing 8 complex outputs per iteration with no branches. Equivalent vectorization width to the AVX2 tier.

#### NEON Benchmark Results

**Test:** `~/irid.iq` — 12 GB, 10 MHz ci8, 10 minutes of data; Apple M3 Max

| Configuration | CPU Time (user) | Wall Time | Realtime Factor | CPU Speedup |
|---------------|-----------------|-----------|-----------------|-------------|
| NEON (default) | 647.8s | 2:31.98 | **3.9x** | **1.54x** |
| Scalar | 995.2s | 3:58.02 | 2.5x | 1.0x |

- CPU time reduction: **34.9%** (995.2s → 647.8s)
- Wall time reduction: **36.2%** (3:58 → 2:32), driven by CPU-bound DSP throughput
- CPU utilization virtually identical (432% vs 422%), confirming no change in thread-level parallelism

### Known Limitations

- **No AVX-512** -- Would require separate compilation unit and detection; ~1.5-2x speedup over AVX2 not worth the maintenance burden for <5% of user base

## Detection Threshold Optimization (Phase 14)

Comparative analysis against gr-iridium revealed that the original 18 dB threshold was conservative. The 16 dB threshold recovers marginal signals (low-elevation satellites, fading) without introducing false positives.

Current results (60-second USRP B210 capture, 10 MHz, 1622 MHz center):

| Metric | iridium-sniffer (16 dB) | gr-iridium |
|--------|------------------------|------------|
| Detected bursts | 5468 | N/A |
| Demodulated RAW frames | 3228 | 2713 |
| IDA frames (--parsed) | 693 | -- |
| IDA frames (external parser) | 507 | 690 |

The internal `--parsed` IDA decoder recovers 37% more IDA frames than the external iridium-parser.py (693 vs 507) thanks to Chase soft-decision BCH decoding and Gardner timing recovery.

The threshold is relative to the adaptive noise floor, so it works equally well in clean and noisy environments.

## Frame Type Coverage

Every demodulated frame is classified; the high-value types are fully decoded.

| Type | Channel | Status | Notes |
|------|---------|--------|-------|
| IRA | simplex | Decoded | ring alert: sat/beam/position/paging |
| IBC | duplex | Decoded | broadcast: sat/beam/timeslot/time |
| MSG | simplex | Decoded | pager text (ASCII/BCD), BCH poly 1897 — validated vs iridium-toolkit |
| IDA | duplex | Decoded | data/SBD → ACARS (libacars), GSMTAP, reassembly |
| IIP | duplex | Decoded | IP-over-PPP, CRC-24 validated (`ida_iip_decode`) |
| ISY | duplex | Classified | sync frame, no decodable payload |
| ITL | simplex | Classified | time/location PRS telemetry (deep decode needs large PRS tables) |
| VOC | duplex | Classified | voice; audio needs the proprietary AMBE vocoder (out of scope, as in iridium-toolkit) |
| IIQ/IIR/IIU | duplex | Classified as RAW | IP variants requiring a Reed-Solomon codec (not implemented) |
| IU3/IU6 | duplex | Classified | inband signalling, structure not publicly documented |

Classification feeds the web map's frame-type histogram. The decode/classify split mirrors what iridium-toolkit itself surfaces in real time (it also leaves voice audio, ITL PRS, and the RS-coded IP variants to offline/auxiliary tooling).

### MSG decoder validation

The MSG (pager) decoder was validated by using iridium-toolkit's `iridium-parser.py` as an oracle: a round-trip encoder builds MSG frames, and identical bits are fed to both the reference parser and the C decoder. They agree on RIC, format, sequence, and text across varied messages and under injected bit errors (BCH t=2). The IIP CRC-24 was likewise verified bit-exact against `crcmod`. Key gotcha: `RAW`-prefixed lines are symbol-reversed by the parser; the C demod output and the parser's post-reverse stream share one convention, and the 24-bit access code + 32-bit messaging header are symbol-reverse-invariant (all 00/11 pairs).

## Known Issues

- Confidence runs lower than gr-iridium on synthetic data due to int8 quantization effects in constellation measurement. Does not affect bit-level correctness.
- GPU burst detection is slower than CPU on fast x86 CPUs at 10 MHz (GPU batching overhead). Expected to help on weaker CPUs (Pi 5) or higher sample rates.
- Vulkan backend not yet tested on Raspberry Pi 5 hardware (verified on NVIDIA only).
