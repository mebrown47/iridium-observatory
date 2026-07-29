# The AT1 Tab: Breaking Out Iridium ATS Datalink (ADS-C & CPDLC)

*How this fork differs from the original iridium-sniffer in decoding and
surfacing aviation datalink messages — the "AT1" work.*

## The short version

Aircraft over the ocean have no radar, so they report to Air Traffic Control
over a satellite datalink — position reports (**ADS-C**) and text clearances
(**CPDLC**), using the FANS-1/A protocol carried inside ordinary ACARS. On the
Iridium network those ride in the same **IDA → SBD → ACARS** stream this
receiver already decodes.

The original iridium-sniffer decoded these under the hood, but only used them to
*place a dot on the map*. This fork **breaks them out** into dedicated,
human-readable tabs in the web UI — the **AT1** tab (structured flight messages)
and a **CPDLC** tab — and logs the full decode to the `--archive` file so the
Observatory dashboards can chart them. That's the whole difference in one line:
the decode engine was already there; **we surfaced its output.**

## What "AT1" actually means

`AT1` is an **ARINC-622 IMI** (Imbedded Message Identifier) — the 3-character
tag at the front of an ATS datalink message that says which application it is:

| IMI  | Application |
|------|-------------|
| **AT1** | **ADS-C** — Automatic Dependent Surveillance–Contract (position reports) |
| CR1 / CC1 / DR1 | CPDLC connection management (connect request / confirm / disconnect) |

We labelled the tab **AT1** because that's literally the identifier stamped on
the position-report messages it shows. (Think of it as "the ADS-C tab," named
after the wire tag rather than the acronym.)

## The signal chain

```
Iridium L-band burst
      │  QPSK demod → frame
      ▼
   IDA frame            ← the ACARS-bearing Iridium frame type
      │  SBD reassembly (multi-packet)   [sbd_acars.c]
      ▼
   ACARS message        ← label, tail, flight-id, text
      │  libacars ARINC-622 app decode    [la_acars_decode_apps]
      ▼
   ADS-C / CPDLC proto tree
      │  walk tags → readable one-liner
      ▼
   AT1 tab  +  CPDLC tab  +  --archive JSONL  →  Observatory
```

The reassembly and protocol details follow muccc's iridium-toolkit
(`reassembler/sbd.py`); the ARINC-622 decode is done by **libacars-2**.

## What this fork changed (vs. the original)

Everything below is additive — the RF path and the map are untouched.

**1. A dedicated AT1 tab in the web UI.** The original had a single ACARS
sidebar. This fork adds a tabbed sidebar with an **AT1** tab that carries only
the structured flight messages (`web_map.c`; `web_map_add_premium_msg`). The
original had no "premium"/AT1 tab at all.

**2. ADS-C reports rendered as a readable line.** The original decoded ADS-C
only far enough to extract a lat/lon for the map dot. This fork walks the full
ADS-C tag list and formats the whole report the way an operator wants to read
it — position, flight level, speed/heading, vertical speed, winds/temperature,
and the next waypoint with ETA:

```
N3721.4402 W15108.9931 FL380 | 486kt 271° +32fpm | WND 240/58kt -54°C | NXT 37.9500N 155.2100W FL380 ETA+14min
```

**3. A separate CPDLC tab.** The original didn't break CPDLC out at all. This
fork finds the CPDLC node (`la_proto_tree_find_cpdlc`), renders every message
element with the libacars text formatter, and flattens it to one line
(`flatten_la_text`) for a **CPDLC** tab — so controller/pilot clearances are
readable instead of raw ARINC-622 hex.

**4. Archived for analytics.** When `--archive` is on, each decoded message's
full proto tree is written to the daily JSONL (`la_proto_tree_format_json`).
That's what feeds the Observatory's **Aviation** dashboard (CPDLC clearance
counts, ADS-C contract counts, per-tail history) — none of which exists in the
original.

## The one dependency that matters

The AT1/CPDLC breakout requires **libacars-2** to be present at build time
(`HAVE_LIBACARS`). With it, you get the decoded tabs. **Without it, the binary
still runs and still shows raw ACARS text — but the AT1 and CPDLC tabs stay
empty**, because there's no ARINC-622 decoder linked in. If your AT1 tab is
blank while ACARS text flows, check `ldd ./iridium-sniffer | grep acars`; an
empty result means it was built without libacars.

## Where you see it

- **Live:** the **AT1** and **CPDLC** tabs in the web map sidebar (`--web`).
- **Historical/trends:** the Observatory **Aviation** dashboard, once
  `--archive` is on and `rollup.py` has folded the JSONL into the database.

---

*In one sentence: the original decoded FANS-1/A to find aircraft; this fork
breaks those same decodes out into readable AT1 (ADS-C) and CPDLC tabs and
archives them for trend analysis.*
