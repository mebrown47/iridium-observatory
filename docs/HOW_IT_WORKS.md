# Under the Hood — A Plain-Language Guide to How This Tool Works

This is a companion to `README_NEW.md`, which covered the new pager/data
decoding and the web map. This document goes a layer deeper: it explains
what `iridium-sniffer` actually *is*, how it was built and tuned, and how we
know it works — all translated out of signal-processing jargon.

If you just want to use the tool, you don't need this document. If you're
curious why it's fast, why some numbers in the stats line look the way they
do, or how confident you should be in its output, read on.

## What problem does this tool solve?

To listen to Iridium satellite traffic, you need two things: a radio
(an SDR — software-defined radio) to capture the airwaves, and software to
turn that raw radio signal into something readable. Historically, that
software was **gr-iridium**, which is built on a large, separate audio/
signal-processing framework called GNU Radio.

**`iridium-sniffer` is a standalone replacement for that whole pipeline.**
It does the same job — listening, detecting bursts, demodulating them — but
without requiring GNU Radio at all. That matters in practice because GNU
Radio is a heavyweight dependency to set up, especially on small computers
like a Raspberry Pi. This tool was built to behave identically to
gr-iridium while being lighter, faster, and easier to install.

It outputs the same plain-text format gr-iridium does, so anything you've
already built around that format keeps working without changes.

## How a satellite burst becomes readable text: the assembly line

Think of this as a factory assembly line with four stations. Raw radio
signal goes in one end; readable text comes out the other.

**Station 1 — Spot the burst.** The tool continuously watches a slice of
radio spectrum and looks for shapes that pop up out of the background
noise — Iridium transmissions are quick (a fraction of a second) bursts,
not a continuous stream, so the first job is just noticing *something just
happened* and grabbing the right chunk of signal.

**Station 2 — Clean it up.** A burst, fresh off the air, is dirty: it's
been Doppler-shifted by the satellite's motion, it's mixed with background
noise, and it's not perfectly aligned in time. This station nudges the
signal's frequency back to where it should be, filters out noise outside
the channel of interest, and finds the precise start of the burst.

**Station 3 — Turn waveform into bits.** Iridium encodes information as
phase shifts in the radio wave (a scheme called QPSK — think of it as a
four-position dial, where each position represents two bits). This station
reads the dial position over and over, locks onto the timing, and converts
the wave into a stream of 1s and 0s. It also checks a known "fingerprint"
pattern (the unique word) at the start of every burst to confirm it really
is an Iridium burst and not noise that happened to look like one.

**Station 4 — Make sense of the bits.** The raw bits then get handed off to
the decoders described in `README_NEW.md` — pulling out pager text, data
payloads, satellite positions, and so on — and optionally pushed into the
web map, or out to other tools like Wireshark.

This four-station design runs on separate processing threads so a slow step
in one station doesn't stall the others — similar to how an assembly line
keeps moving even if one station is briefly backed up, because there's
buffer space between stations.

## "Is this thing actually as good as the tool it replaces?"

This was tested extensively, not just assumed. Three kinds of checks were
done:

**1. Bit-for-bit comparison on a clean test signal.** A synthetic, known-
correct Iridium burst was fed through both gr-iridium and this tool. The
two produced *identical* decoded bits — not just "close," but
byte-for-byte the same. This confirms the actual signal-processing math is
correct, with no shortcuts that changed the answer.

**2. Side-by-side comparison on real satellite recordings.** Both tools
were pointed at the same recorded radio data and their output was
compared. This is where it gets interesting: this tool's settings are
intentionally a bit more aggressive about which faint, marginal signals it
even attempts to decode. The result is it tries to decode roughly 4-6x more
candidate bursts than gr-iridium, succeeds on a lower *percentage* of them
(since it's reaching further into the noise), but still comes out ahead on
the number that matters: total messages successfully decoded — often around
twice as many real decoded frames as gr-iridium, on the same recording.

Think of it like two fishermen at the same spot: one casts fewer times but
rarely misses; the other casts much more often, missing more often too, but
hauls in a bigger total catch by the end of the day. For a listening tool,
total catch is what you actually care about.

**3. Live hardware testing.** Beyond synthetic files, this was run against
real, live satellite passes using several different radio receivers
(an Ettus USRP, a Nuand bladeRF, and others). Real aircraft tracking
messages, real pager messages with GPS coordinates, and real satellite
housekeeping data were all successfully decoded from live sky signals —
confirming the tool works in the real world, not just on idealized test
files.

## Why does the live "ok%" number look low sometimes?

If you watch the tool's running statistics, you'll see a percentage often
called "ok%" — and it might look unimpressive (20-35%), especially next to
gr-iridium's number in the 70-80% range. This number alone is misleading
unless you know what it means.

That percentage answers: *"of every signal-shaped blip I tried to
decode, what fraction passed?"* It is **not** a quality score for the
output you actually receive — every burst that fails that check is simply
discarded silently, so you only ever see the bursts that passed.

This tool deliberately casts a wider net, attempting to decode much fainter,
more marginal signals that a more cautious detector would skip outright.
Most of those marginal attempts fail (hence the lower percentage), but
enough of them succeed that the *total number* of good decoded messages
ends up higher than the more conservative approach. A lower "ok%" with more
total decoded messages is a feature of the design, not a sign of lower
quality.

## A few specific tuning stories worth knowing about

While building this, a handful of subtle bugs were found by comparing
intermediate results against gr-iridium step by step. Each is a small
lesson in how sensitive radio signal processing can be:

- **A filter was simply too wide.** An early version let through about
  3x more "noise bandwidth" than necessary, which quietly degraded
  everything downstream — like trying to read a fingerprint through a
  smudged lens. Narrowing that filter to the actual width of an Iridium
  signal recovered most of the missing performance in one fix.

- **The tool was only checking for one direction of traffic.** Iridium
  bursts can be either "downlink" (satellite to ground) or "uplink"
  (handset to satellite), and each has its own fingerprint pattern. An
  early version only checked the fingerprint that matched its best guess
  of which direction a burst was — so correctly-decoded bursts were
  sometimes thrown away simply because the direction guess was wrong.
  Checking both fingerprints and accepting either fixed this.

- **A rounding choice cost a sliver of accuracy.** A calculation that
  estimates leftover frequency error was using a slightly oversized
  analysis window due to a rounding-up instead of rounding-down choice,
  which let a little extra noise leak into the measurement. Switching the
  rounding direction tightened it up.

None of these are dramatic stories, but together they illustrate the
general approach: when this tool's output didn't match gr-iridium's, the
difference was tracked down to a specific, explainable cause rather than
shrugged off.

## Going faster: using more of the computer at once

Beyond getting the *answer* right, a separate effort went into getting the
answer *faster*, using two techniques:

**Multiple CPU cores at once.** Modern processors have several cores that
can work in parallel. The tool is structured so that the most
computationally expensive step (cleaning up each burst) is spread across
several worker threads, since each burst's cleanup is independent of every
other burst's — much like several cashiers can ring up different customers
at the same time without needing to coordinate with each other.

**Using the chip's built-in "do many at once" instructions.** Modern CPUs
can apply the same simple math operation (like a multiply) to several
numbers in a single step, instead of one at a time — like stamping eight
envelopes with one press of a multi-stamp tool instead of stamping each
envelope individually. This tool detects what your specific CPU supports
and automatically uses the fastest option available, whether that's a
newer Intel/AMD chip, an older one, or an Apple Silicon / ARM chip — with a
plain, one-at-a-time fallback that always works as a safety net. Every one
of these speed paths was verified to produce the exact same decoded output
as the slow, simple version — speed was never allowed to come at the cost
of correctness.

**Optional graphics-card acceleration.** For one particular step (the
initial burst-spotting scan), the tool can optionally offload work to a
graphics card (GPU) instead of the CPU. This mainly helps on smaller, less
powerful computers like a Raspberry Pi; on a fast modern laptop or desktop
CPU, the overhead of handing work off to the GPU can actually make things
slightly slower, so the tool defaults sensibly and lets you force either
path.

## In one sentence

This tool rebuilds, from scratch, everything gr-iridium does to turn raw
satellite radio signals into readable data — without needing GNU Radio —
and it was proven correct against gr-iridium step by step, tuned to catch
more real signals, and sped up using every spare bit of computer hardware
available, all while keeping the same simple output format you'd already
expect.
