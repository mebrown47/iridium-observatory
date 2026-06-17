# What's New in This Build — A Plain-Language Guide

If you're new to Iridium signal sniffing: `iridium-sniffer` listens to the
Iridium satellite network and pulls individual radio "bursts" out of the air,
then figures out what they are and what they say. This document explains
what's been added on top of the original tool, without assuming you already
know the acronyms.

## The short version

The original tool could already find Iridium bursts and read two specific
kinds of them (ring-alerts and broadcast frames). Everything else it heard,
it just dumped as raw bits and handed off to a separate Python program to
make sense of.

This update teaches the tool to:

1. **Read pager messages itself** — no more handing that job to Python.
2. **Read a second kind of data frame** (an internal IP/networking frame) and
   check that it decoded correctly.
3. **Recognize every single type of burst it hears**, even ones it can't
   fully decode, so you always know what's flying by.
4. **Show all of this in the built-in web map**, in new tabs and a live
   counter panel — no extra software needed.

No new external libraries were added, and nothing that already worked was
changed. It's strictly *more capability*, not a rewrite.

---

## Background: what is a "burst," and why are there different types?

Iridium satellites are constantly transmitting and receiving short radio
packets — "bursts" — for many different jobs at once: paging phones, telling
satellite phones where they are, carrying voice calls, carrying data/texts,
internal satellite-to-satellite housekeeping, and more. Each job uses a
slightly different burst format.

When `iridium-sniffer` picks one of these bursts out of the air, it has to
answer two separate questions:

- **What *type* of burst is this?** (a quick classification)
- **Can I actually understand what it says?** (a full decode)

Some burst types the tool can fully decode into readable information.
Others it can only recognize and count, because either the content isn't
publicly documented, or decoding it requires extra heavy machinery (like an
audio codec for voice) that's outside the scope of this tool.

---

## 1. Pager messages are now decoded directly

**What this is:** Iridium has a paging channel — short text messages sent to
pagers, similar in spirit to an old-school numeric pager network. Each
message is addressed to a "RIC" (a receiver ID number, like a pager's
phone number) and carries either plain text or a string of digits.

**Before:** The tool could detect that a pager message had gone by, but
couldn't read it. You'd have to run a separate Python toolkit
(`iridium-parser.py`) alongside it to actually decode the text.

**Now:** The tool reads the message itself — the recipient ID, the message
format, a sequence number, and the actual text (whether it's letters or
digits) — with nothing else running alongside it.

**How it was checked:** The new decoder was run side-by-side against the
existing, trusted Python decoder on a wide variety of test messages,
including messages with simulated radio errors (bad bits sprinkled in), and
the two agreed every time. In the process, a real bug was found and fixed —
in noisy conditions, a checksum was being calculated incorrectly in a way
that could cause messages to get cut short. That's now fixed.

## 2. A second data-frame type (IIP) is now decoded and verified

**What this is:** Besides the well-known data channel that carries things
like aircraft tracking messages (ACARS), Iridium also carries a more
generic internal networking frame type. Think of it as a different "lane"
on the same data channel, used for a different purpose.

**Before:** These frames were detected but never opened up — the tool
didn't have logic to even attempt reading them.

**Now:** The tool extracts their header, sequence info, and payload, *and*
checks a built-in checksum (called a CRC) to confirm the data wasn't
corrupted in transit. That checksum routine was tested against an
independent, trusted reference implementation and produces identical
results.

## 3. Every burst is now labeled, even the ones we can't fully read

This is arguably the most useful change for everyday use, even though it
doesn't "decode" anything new.

**Before:** Only two or three burst types were ever recognized by name.
Everything else showed up as an undifferentiated pile of "raw" data — you
couldn't tell, at a glance, whether you were hearing voice traffic, internal
satellite signaling, or something else.

**Now:** Every burst that comes in gets labeled with its actual type —
there are eleven categories in total. Some of those labels mean "fully
decoded and readable" (like the pager messages and data frames above).
Others mean "I recognize what this is, I just can't show you its contents,"
for one of these honest reasons:

| Why it's only labeled, not decoded | Example |
|---|---|
| The internal structure isn't publicly documented | Internal sync/signaling frames |
| Decoding needs a large reference table that adds little practical value | Time/location telemetry frames |
| It's voice audio, which needs a proprietary, patented audio decoder | Voice call frames |
| It needs a much heavier error-correction scheme that hasn't been built yet | A few rarer data-frame variants |

This mirrors exactly what the existing Python toolkit does in practice — it
also leaves voice and a few rare frame types to other, separate tools. So
nothing here is a step backward; it's making the tool's own picture of "what
just happened" match what was already true under the hood.

**Why this matters in practice:** if you're trying to understand satellite
or signal behavior — not just decode messages, but watch the *rhythm* of
what the network is doing — having every burst correctly labeled, instead
of a mystery pile of "raw," is the difference between a system you can
observe and one you can only partially see.

---

## 4. The built-in web map got two new features

If you've used the `--web` flag, you already know `iridium-sniffer` can pop
up a live map in your browser showing satellite ring-alerts and a feed of
decoded ACARS (aircraft) messages. That map server was extended, not
replaced — same Pi-friendly, no-extra-software approach.

**New pager tab.** Right alongside the existing aircraft-message feed,
there's now a second tab showing the live pager messages described above —
who they're addressed to, what they say, and whether the checksum passed.

**New live frame-type counter.** A panel showing real-time counts of every
burst type the tool has seen since it started — pager messages, data
frames, ring alerts, voice, sync frames, all eleven categories, updating as
they arrive. If you're curious "what is this satellite actually doing right
now," this panel is the answer at a glance.

---

## What didn't change

- No new software dependencies were added, and no build process changed.
  If it compiled before, it compiles the same way now.
- The frame types that were already fully decoded (ring-alerts and
  broadcast frames) work exactly as before.
- The plain-text data format the tool outputs is unchanged, so anything
  you've already built on top of it keeps working.

---

## In one sentence

The tool went from "reads two kinds of Iridium messages and is blind to
everything else" to "reads four kinds of Iridium messages directly, and
correctly identifies all the rest — with all of it visible live in your
browser."
