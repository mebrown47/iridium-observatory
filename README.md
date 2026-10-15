# iridium-observatory

iridium-observatory listens to the Iridium satellite network with a software-defined
radio and shows what it hears: pager messages, aircraft datalink (ACARS, ADS-C position
reports, CPDLC clearances), satellites passing overhead, and where your receiver is,
worked out from the satellites alone, typically to within a few km. It keeps a daily archive and turns it into
long-run dashboards, and it can feed aircraft messages and positions to aggregators
such as airframes.io.

It is a fork of [iridium-sniffer](https://github.com/alphafox02/iridium-sniffer) and keeps
that project's receiver engine at its core. [How it differs](#how-it-differs-from-iridium-sniffer)
is listed below.

## What you get

Two services that start at boot, each with a web page:

| Port | Service | What it shows |
|---|---|---|
| 8888 | **Receiver** (`iridium-sniffer`) | Live map of satellite beams, paging and aircraft, with ACARS, Pager and AT1 tabs (AT1 carries ADS-C position reports and CPDLC clearances) and a live count of every frame type heard |
| 8890 | **Observatory** | **Trends**: the traffic mix over hours and days. **Aviation**: per-aircraft history, CPDLC log and ADS-C contracts. **Doppler**: each satellite pass drawn as a frequency curve, the satellites overhead and their upcoming passes, and your receiver's estimated location |

The Observatory only reads the receiver's archive and live feed; it never changes what
the receiver does.

## What you need

- **A computer** running Ubuntu, Debian, Raspberry Pi OS or DragonOS. An Intel N100 or a
  Raspberry Pi 5 is enough; see the [engine reference](docs/ENGINE_REFERENCE.md#low-power-hardware-n100-pi-5-arm).
- **An SDR that tunes to 1.6 GHz.** An Airspy R2 covers the whole Iridium band with
  its native driver; a HackRF One, USRP, BladeRF or SDRplay also works well, and an
  RTL-SDR covers only a narrow slice of the band (the ring-alert and pager channels). See [SDR hardware](docs/ENGINE_REFERENCE.md#sdr-hardware).
- **An L-band antenna with a clear view of the sky,** and an LNA if the cable run is
  long. The installer can power the LNA through the SDR's bias tee.
- **Internet** for the install, and once a day afterwards for satellite orbit data
  (the location and satellite display need it; everything else works offline).

## Install

**New to Linux or the terminal?** Follow the step-by-step [Quick Start](QUICKSTART.md).

With your SDR plugged in:

```bash
git clone https://github.com/mebrown47/iridium-observatory.git
cd iridium-observatory
./install.sh
```

Run it as your normal user; it uses `sudo` when it needs to. It installs the packages
for the SDR it finds, builds the program and runs its tests, makes sure the service can
open the SDR after a reboot, and asks a few questions:

- which SDR, and for an Airspy its gain (sensitivity 0–21, default 12)
- whether to power an LNA through the bias tee
- whether to estimate your location from Doppler
- whether to feed ACARS to airframes.io, any other ACARS feeds (`HOST:PORT`), and a station ID
- whether to feed aircraft positions (SBS) to an aggregator, such as `thebaldgeek.net:2226`

It ends by printing the two page addresses.

| Path | What |
|---|---|
| `/etc/iridium-observatory.conf` | your settings: edit, then `sudo systemctl restart iridium-sniffer` |
| `/var/lib/iridium-observatory/` | the daily archive, the Observatory database, and cached satellite orbit data |
| `/opt/iridium-observatory/` | the installed program; `iridium-sniffer` is linked into `/usr/local/bin` |

```bash
./install.sh --update        # git pull, rebuild, test, reinstall, restart
./install.sh --reconfigure   # answer the questions again (new SDR, feeds, gain)
./install.sh --dry-run       # show what it would do as root, change nothing
./install.sh --uninstall     # remove the services and program (keeps settings and data)
```

**Upgrading from iridium-sniffer:** if you already run `iridium-sniffer` from a
hand-written systemd unit, the installer offers to keep that unit's settings and to copy
its archive history, and saves a copy of the old unit in `/var/lib/iridium-observatory/`.

To build by hand or run the program directly, see the [engine reference](docs/ENGINE_REFERENCE.md).

## Check it's working

- The receiver page on port 8888 loads and its frame counters climb.
- The Observatory on port 8890 loads; Trends fills in over the first hour.
- Within about 20 minutes, the Doppler page shows "Your estimated location is:" marked
  Settled, within a few km of where the receiver really is.
- If you turned feeds on, `journalctl -u iridium-sniffer` shows them connecting.

## Receiver location

The Observatory's Doppler page works out where your receiver is from the satellites it
hears, with no GPS, and shows it at the top of the page:

> **Your estimated location is:** 51.4779° N, 0.0015° W
> From 88 ring alerts of 3 satellites, fitted 21 s ago to 16 Hz rms. Settled: the last
> two fits agree within 4.2 km.

**How it works.** Every Iridium satellite sends ring alerts on a fixed frequency, and each
alert carries the satellite's own position. As a satellite passes, Doppler shifts that
frequency by up to ±37 kHz, by an amount that depends on where the receiver is. The
Observatory computes each satellite's exact path from published orbit data (TLEs, fetched
daily from CelesTrak) and searches for the one spot on Earth where the ring alerts of
several satellites all agree. This is the method from iridium-analyzer, ported to the
Observatory.

**Accuracy.** Typically within a few km. On the home feeder, the first settled fit came about
10 minutes after a restart and agreed with the receiver's known location to within about a
kilometre, and it held there overnight.

**What the display tells you:**
- **Settled or provisional.** A fit is refreshed every 2 minutes and called *settled* once
  two in a row agree within 20 km. Early fits, from a few ring alerts of satellites in one
  orbital plane, can be off by hundreds of km and are marked *provisional*.
- **The receiver's own estimate.** iridium-sniffer's built-in Doppler solver (`--position`)
  keeps its own estimate. The page shows it beneath, with the distance between the two;
  agreement within a few km is a good cross-check. The built-in solver takes longer to
  settle after a restart.
- **Receiver clock drift.** The receiver timestamps frames from its sample count, which
  falls behind real time when samples are dropped, by as much as a minute a day. Orbit
  calculations need true time (one second is 7.5 km of satellite travel), so the
  Observatory recovers the offset from the satellite positions in the ring alerts and
  shows it.

Beside the Doppler curves, a **Satellites** panel lists the satellites above your horizon
(elevation, azimuth, Doppler and drift) and draws their passes from 20 minutes ago to
40 minutes ahead, with a tick for each ring alert heard. The curves are labelled with the
satellites' names, such as IRIDIUM 136.

It needs the receiver running with `--position` (the installer's "estimate your location"
question) and internet once a day for orbit data; without it, the page says so instead of
guessing. The Observatory can also be given a known location (`--station LAT,LON`), which
the satellite panel then uses instead of the estimate.

## Troubleshooting

Run this from the repo folder and look through the report it writes:

```bash
./diagnose.sh
```

It needs no `sudo` and changes nothing. It collects the OS and library versions, which
library copies the program really loads, whether the service can open each SDR, service
and port status, the live pages' state, and recent logs, into one file. The report
includes your settings and estimated location, so check it before posting it publicly.

## How it differs from iridium-sniffer

The receiver engine (burst detection, downmix, demodulation) is iridium-sniffer's. This
fork adds:

**Decoding**
- **Pager messages** decoded in the program, with BCH error correction; no external parser needed.
- **IP-over-PPP (IIP) frames** decoded and CRC-checked.
- **Every frame classified** (IRA, IBC, MSG, IDA, ISY, IIP, VOC, ITL, IU3, IU6), not just the three types the original decodes.
- **A native port of iridium-toolkit's parser and reassembler** (`--parsed=full`, `--reassemble`, `--messages`), matching the Python tools' output on 395,365 frames of live reception.
- **Readable aircraft datalink:** ADS-C position reports and CPDLC clearances in plain language, encrypted or compressed payloads marked as such, and ground-sent ADS-C contract requests no longer misread as aircraft at 0,0.

**Receiver**
- **Native Airspy R2 / Mini support**, with its gain settings.
- **False-detection filtering** (`--uw-reject`), which cut the unidentified-burst rate from about 34% to 11–12% on the home feeder with no loss of decoded frames.
- **Doppler positioning fixes:** three errors that could put the estimate about 1,500 km off, and a display that went blank after a day of running.
- **Recordings keep their own time** (`--file-start`).

**Dashboards and data**
- **Pager and AT1 tabs** on the live map, beside the original ACARS panel, and a live frame-type count.
- **Daily archive** (`--archive`) and the **Observatory**: Trends, Aviation and Doppler pages.
- **Receiver location from the satellites:** "Your estimated location is" on the Doppler page, fitted against published satellite orbits, typically within a few km and settled within minutes of a restart; a second, independent check on the built-in Doppler solver. Plus the satellites overhead and their passes. Ported from iridium-analyzer; see [Receiver location](#receiver-location).

**Setup**
- **One-command installer** with services, device access, feeds and gain.
- **`diagnose.sh`** for troubleshooting reports.

[SCOPE_COMPARISON.md](SCOPE_COMPARISON.md) compares the two projects file by file.

## Documentation

| Document | For |
|---|---|
| [QUICKSTART.md](QUICKSTART.md) | Installing step by step, assuming only that you can open a terminal |
| [docs/ENGINE_REFERENCE.md](docs/ENGINE_REFERENCE.md) | Building by hand, every command-line option, piping to other tools, hardware notes |
| [docs/NEW_FEATURES.md](docs/NEW_FEATURES.md) | The decoding this fork added, in plain language |
| [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) | How a burst becomes a decoded message |
| [docs/AT1_DECODE.md](docs/AT1_DECODE.md) | ADS-C and CPDLC decoding |
| [docs/NATIVE_PARSER_PLAN.md](docs/NATIVE_PARSER_PLAN.md) | The native parser and reassembler, and how they were checked |
| [observatory/README.md](observatory/README.md) | The Observatory's design |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Signal processing pipeline and threading |
| [SCOPE_COMPARISON.md](SCOPE_COMPARISON.md) | File-by-file comparison with iridium-sniffer |

> **Names:** the program is still called `iridium-sniffer`, the engine's original name, and
> its per-user files stay in `~/.iridium-sniffer/`. The services are `iridium-sniffer` and
> `observatory-trends`.

## Origin & provenance

iridium-observatory is a fork of [iridium-sniffer](https://github.com/alphafox02/iridium-sniffer)
(© CEMAXECUTER LLC), licensed under **GPL-3.0-or-later**. The upstream project supplies the RF
engine (burst detection, downmix, DQPSK demodulation and iridium-toolkit-compatible RAW
output), which remains the foundation here. All upstream copyright and license notices are
retained, and per-file headers record authorship.

## License

GNU General Public License v3.0 or later. See [LICENSE](LICENSE).

Copyright © CEMAXECUTER LLC (upstream iridium-sniffer) and Mike Brown (this fork's modifications). Individual source files carry the applicable copyright and SPDX headers; the `qpsk_demod` sources additionally retain their Free Software Foundation / GNU Radio origin notice. As a GPL-3.0 derivative, this fork preserves all upstream notices and remains under the same license.

## Acknowledgments

iridium-observatory is a fork of, and builds directly on, **iridium-sniffer**. Full credit to that project and to the wider ecosystem it draws from:

- [iridium-sniffer](https://github.com/alphafox02/iridium-sniffer) (GPL-3.0-or-later) © CEMAXECUTER LLC -- the upstream project this is forked from. It provides the core RF engine (burst detection, downmix, DQPSK demodulation, RAW output) plus the ACARS/SBD, GSMTAP, BaseStation, and Doppler-positioning functionality that this fork extends.
- [gr-iridium](https://github.com/muccc/gr-iridium) (GPL-3.0-or-later) by Sec and schneider42 (muccc) -- the signal processing algorithms for burst detection, downmix, and QPSK demodulation are clean-room C ports of gr-iridium's GNU Radio blocks
- [ice9-bluetooth-sniffer](https://github.com/mikeryan/ice9-bluetooth-sniffer) (GPL-2.0) by Mike Ryan / ICE9 Consulting LLC -- the SDR backend abstraction, build system, and threading infrastructure are adapted from this project
- [VkFFT](https://github.com/DTolm/VkFFT) (MIT) by Dmitrii Tolmachev -- header-only GPU FFT library used for both OpenCL and Vulkan burst detection
- [iridium-toolkit](https://github.com/muccc/iridium-toolkit) (BSD-2-Clause) by Sec and schneider42 -- the downstream frame parser and reassembler, and the reference implementation for BCH error correction and de-interleaving algorithms
- [libacars](https://github.com/szpajder/libacars) (MIT) by Tomasz Lemiech (szpajder) -- optional dependency for ARINC-622, ADS-C, and CPDLC decoding within ACARS messages
