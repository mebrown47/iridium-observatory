# Quick Start: install iridium-observatory

This guide takes you from a fresh computer to a working Iridium receiver, step by step.
You only need to be able to open a terminal window and type (or paste) commands.

The installer does all the hard work: it downloads what it needs, builds the program,
and sets it up to start by itself every time the computer starts.

## Before you start

You need:

- **A computer** running Ubuntu, Debian, Raspberry Pi OS or DragonOS, connected to the internet.
- **An SDR radio** plugged into a USB port, such as an Airspy R2 or a HackRF One.
- **An Iridium antenna** connected to the radio, outdoors or in a window with a view of the sky.
- **Your password** for this computer. The installer will ask for it.
- **About 30 minutes.** Most of that is the computer working while you wait.

Two tips for the terminal:

- **Pasting:** copy a command from this page, then press **Ctrl+Shift+V** in the terminal
  (plain Ctrl+V doesn't work there). Press **Enter** to run it.
- **Passwords don't show.** When asked for your password, type it and press Enter.
  Nothing appears on screen as you type; that's normal.

## Step 1: Open a terminal

On Ubuntu, press **Ctrl+Alt+T**. On other systems, find **Terminal** in the applications menu.

A window opens with a line ending in `$`. That's where you type commands.

## Step 2: Install git

`git` is the tool that downloads iridium-observatory. Copy this line, paste it into the
terminal, and press Enter:

```
sudo apt update && sudo apt install -y git
```

Type your password when asked. Wait until the `$` prompt comes back.

## Step 3: Download iridium-observatory

```
git clone https://github.com/mebrown47/iridium-observatory.git
```

This creates a folder called `iridium-observatory` in your home folder.

## Step 4: Go into the folder

```
cd iridium-observatory
```

## Step 5: Run the installer

Make sure the radio is plugged in, then:

```
./install.sh
```

Type your password if asked. The installer works through several stages and prints a
line for each one. **Building takes 5 to 20 minutes**; let it run.

## Step 6: Answer the questions

Partway through, the installer asks a few questions. Each one shows a suggested answer in
`[brackets]`; **press Enter to accept it.** If you're not sure, the suggested answer is fine.

| The installer asks | What it means | If unsure |
|---|---|---|
| **Which one** | Which radio to use, from a numbered list | Press Enter for the first one |
| **Airspy gain** | How much the radio amplifies the signal (only asked for an Airspy) | Press Enter (12) |
| **Power an LNA through the SDR's bias tee** | Whether the radio sends power up the antenna cable to an amplifier | Type `y` only if your antenna has an amplifier that needs power from the cable |
| **Estimate this receiver's position from Doppler** | Work out where you are from the satellites | Press Enter (yes) |
| **Feed ACARS to airframes.io** | Share the aircraft messages you receive with airframes.io | `y` to share, Enter to skip |
| **Other ACARS feeds** | Extra places to send aircraft messages | Press Enter (none) |
| **Station ID** | A name for your station on those sites, such as `XYZ-IRIDIUM` | Your choice, or Enter for none |
| **Feed aircraft positions (SBS/BaseStation) to an aggregator** | Share aircraft positions with a tracking site | `y` to share, Enter to skip |
| **Aggregator HOST:PORT** | Where to send them | Press Enter (thebaldgeek.net:2226) |
| **Include beam-estimated positions** | Also send rough positions (about 200 km) | Press Enter (yes) |

You can change any answer later (see "Change your answers" below).

## Step 7: Open your receiver's pages

When the installer finishes, its last lines show two web addresses, like this:

```
  Live map:     http://192.168.1.50:8888/
  Observatory:  http://192.168.1.50:8890/
```

Open them in a web browser, on this computer or any other one on your home network.
On this computer you can also use `http://localhost:8888` and `http://localhost:8890`.

- **Live map (8888):** satellites, paging, and aircraft, updating every second.
- **Observatory (8890):** charts of what you've received over time, and the **Doppler**
  page with the satellites overhead.

## Step 8: Give it 20 minutes

- The live map's counters should start climbing within a minute or two.
- The Observatory's charts fill in over the first hour.
- On the Observatory's **Doppler** page, "Your estimated location is:" appears within about
  20 minutes and says **Settled** once it's sure. It should be within a few km of where
  you really are.

That's it. The receiver keeps running, and starts again by itself after a reboot.

## Later on

Open a terminal and go into the folder first (`cd iridium-observatory`), then:

| To | Type |
|---|---|
| **Update to the latest version** | `./install.sh --update` |
| **Change your answers** (new radio, feeds, gain) | `./install.sh --reconfigure` |
| **Remove it** (your settings and data are kept) | `./install.sh --uninstall` |

## If something goes wrong

**"Permission denied" when running `./install.sh`:** type `bash install.sh` instead.

**The installer says "no SDR found":** check the radio is plugged in, unplug it and plug
it back in, then run `./install.sh --reconfigure`.

**The pages won't open from another computer** but work on this one: the computer's
firewall is blocking them. Run:

```
sudo ufw allow 8888/tcp && sudo ufw allow 8890/tcp
```

**Anything else:** make a diagnostics report and send it to whoever is helping you:

```
cd iridium-observatory
./diagnose.sh
```

It takes about 10 seconds and changes nothing. It writes a file named like
`diagnose-yourcomputer-20261015-120000.txt` in the folder and prints its name. The report
includes your settings and your estimated location, so share it privately rather than
posting it in public.

---

For what iridium-observatory does and how it differs from iridium-sniffer, see the
[README](README.md). To build by hand or run the program directly, see the
[engine reference](docs/ENGINE_REFERENCE.md).
