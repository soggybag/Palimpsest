# Web Controller

A clickable mockup of the panel design (`docs/Screenshot 2026-09-11 at
11.34.31 AM.png`) that drives the real hardware over USB-MIDI, using the Web
MIDI API — a visual alternative to typing `sendmidi` commands from
[`../docs/testing.md`](../docs/testing.md).

## Running it

Web MIDI only works in **Chrome or Edge** (not Safari, not Firefox). Two ways
to open it:

```bash
open -a "Google Chrome" index.html          # directly as a file
```

or, if the browser is picky about `file://` permissions:

```bash
python3 -m http.server 8000                 # from this directory
# then open http://localhost:8000/ in Chrome
```

Click **Connect MIDI**, allow the permission prompt, and pick **Daisy** from
the dropdown (it's auto-selected if it's the only output).

## What's real vs. mocked

- **The 8 buttons are fully live** — they send the real MIDI notes 60–67, tap
  vs. hold exactly like a physical button (a genuine press/release, not a
  pre-computed guess), and the firmware's own `FuncControl` state machine
  decides latch/momentary from the timing, same as always.
- **Feedback / Speed / Size / Start send real CC 20–23**, which the firmware
  was extended to listen for (see `Palimpsest.cpp`, `RouteMidiEvent` /
  `s_cc_touch`) — once a CC arrives for one of these, it overrides the
  physical knob for the rest of the session. No revert-to-knob gesture yet.
- **Mix (CC 24) is not wired in firmware.** The knob turns and sends the CC,
  but nothing on the Daisy listens for it yet — there's no dry/loop mix stage
  in the audio path currently (see `docs/interface.md`, open question on the
  Mix control).
- **The Encoder and the OLED are decorative.** Loop Select (what the encoder
  will do) is M6, not built yet. The OLED mock shows a static illustrative
  frame — this page has no read path from the hardware, so it can't mirror
  live loop length / BPM / undo state. Button light colours are this page's
  own local latch simulation (mirroring `FuncControl`'s tap/hold rule), not
  read back from the module either.

## Note / CC map

| | |
|---|---|
| 60 Rec · 61 Dub · 62 Sub · 63 Mute · 64 Rev · 65 R.Trig · 66 Undo · 67 Copy | matches `docs/testing.md` |
| CC 20 Feedback · 21 Speed · 22 Size (window length) · 23 Start (window start) · 24 Mix | 0–127, mapped to 0..1 |
