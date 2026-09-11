# Palimpsest — bench testing

How to drive the module from the Mac and a feature-by-feature checklist.
Milestone status lives in [`roadmap.md`](roadmap.md); this is the "does it
actually work" reference.

---

## 1. MIDI setup

The Daisy is a class-compliant **USB-MIDI device** over the same cable you flash
with. No driver. Install `sendmidi` (macOS `.pkg` from
<https://github.com/gbevin/SendMIDI/releases>), then:

```bash
sendmidi list                      # find the exact port name
```

Paste these helpers into the shell (swap the name if `list` shows something
other than `Daisy`):

```bash
D='Daisy'
tap()  { sendmidi dev "$D" on $1 100 off $1 0; }        # momentary press+release
hold() { sendmidi dev "$D" on $1 100; sleep ${2:-1}; sendmidi dev "$D" off $1 0; }
on()   { sendmidi dev "$D" on $1 100; }                 # note on, leave held
off()  { sendmidi dev "$D" off $1 0; }

REC=60 ODB=61 SUB=62 MUTE=63 REV=64 RET=65 UNDO=66
```

The **TRS MIDI IN** jack works in parallel (Pico keypad or a hardware keyboard,
same notes) — nothing here changes if you use that instead.

---

## 2. Control map

### MIDI notes (tap = latch / hold = momentary, except impulse functions)

| Note | Function | Type |
|---|---|---|
| 60 | Record   | impulse — each press advances EMPTY→REC→PLAY→EMPTY (clock-quantised) |
| 61 | Overdub  | latching |
| 62 | Substitute | latching (latch = commit, hold = audition; clock-quantised) |
| 63 | Mute     | latching — loop output on/off, clock-quantised, phantom playhead |
| 64 | Reverse  | latching |
| 65 | Retrigger | impulse |
| 66 | Undo     | impulse (toggles undo ⇄ redo) |

### Panel

| Control | Function |
|---|---|
| CTRL_1 | Feedback — deadzones snap to 0.0 / 1.0; full CW = freeze |
| CTRL_2 | Speed — 0.25×–4×, detents at ½ / 1 / 2×; CV on jack sums |
| CTRL_3 | Window length — full CW = window off; down = shrink (30 ms floor); CV sums |
| CTRL_4 | Window start — position through the loop; CV sums |
| encoder push | Record (tap advances; hold = record-while-held, close on release) |
| **GATE_IN_1** | **CLOCK IN** — quantise reference (was Record gate; use encoder/note 60 now) |
| GATE_IN_2 | Retrigger — edge = jump to window start; audio-rate = stutter |
| gate out | **SYNC OUT** — ~5 ms pulse at each loop/window cycle start |
| CV out 1 | **PHASE** — 0→~5 V ramp over the audible cycle (follows speed / reverse / window) |

Speed / Feedback / Window have **no MIDI** — knob + CV only.

### OLED readout

```
PLAY  1.234s        SUB      <- state | loop length | write/undo status
      ▓▓▓▓▓  W120ms          <- window bracket + length (only when windowed)
 |============|============| <- timeline, | = playhead (global position)
 1.00x   FB 75      <REV     <- speed ratio | feedback % or FRZ | reverse flag
R.S..T.        CPU 12%       <- engaged-function letters | peak CPU (1 s window)
```

Function letters: `R`ecord `O`verdub `S`ubstitute `M`ute re`V`erse re`T`rigger
`U`ndo — shown while engaged, `.` otherwise. Top-right status: `ODB` / `SUB`
while writing, `UND` when undone, `u` when an undo is available.

---

## 3. Smoke test (~30 s)

Feed audio into **IN1** the whole time.

```bash
tap $REC ; sleep 4 ; tap $REC     # record ~4 s, close -> PLAY, hear it loop
tap $ODB ; sleep 4 ; tap $ODB     # overdub a layer ('u' appears)
tap $UNDO                         # layer gone
tap $UNDO                         # layer back
tap $REV                          # reverses ('V', '<REV')
tap $REV                          # forward
tap $RET                          # jumps to loop start
# turn CTRL_3 down: bracket appears, playback confines to the window
# turn CTRL_3 full CW: window off
tap $REC ; tap $REC ; tap $REC    # back to EMPTY
```

---

## 4. Feature checklist

### M1a — Record / Play / Feedback

- [ ] `tap $REC` from EMPTY → `REC`; `tap $REC` → `PLAY` + a length; loop is audible
- [ ] encoder tap does the same
- [ ] `hold $REC 3` from EMPTY → records 3 s, closes on release (`PLAY 3.0xx s`)
- [ ] CTRL_1 mid → loop erodes each pass; lower = faster decay
- [ ] CTRL_1 full CW → `FRZ`; loop holds indefinitely, no degradation over minutes
- [ ] drone held through the close → loop wrap is clean (no click)

### M2 — Reverse / Speed / Retrigger

- [ ] `tap $REV` → `V` + `<REV`, playback backward; `tap $REV` → forward, clean flip
- [ ] `hold $REV 1` → reverses only during the hold
- [ ] CTRL_2 sweep → smooth pitch glide; detents catch at ½ / 1 / 2×
- [ ] CV into CTRL_2 jack → shifts speed
- [ ] `tap $RET` → playhead jumps to loop start, no click
- [ ] audio-rate gate (~20–50 Hz) into GATE_IN_2 → stutter

### M1b — Control layer

- [ ] `tap $REC` vs `hold $REC 2` behave differently (latched vs momentary)
- [ ] `tap $RET` five times in a row → fires every time (not every other)
- [ ] USB and TRS MIDI both drive the same functions
- [ ] engaged-function letters on the OLED track the latches
- [ ] *(keypad wired)* key LEDs follow state via the colour echo

### M3 — Overdub / Substitute / Undo

- [ ] `tap $ODB ; sleep 4 ; tap $ODB` → layer added, `u` shows; `tap $UNDO` removes it; `tap $UNDO` restores it
- [ ] `tap $REV ; tap $ODB ; sleep 4 ; tap $ODB ; tap $REV` → new material plays backward on the forward pass
- [ ] overdub with CTRL_2 off 1× → material lands at the moved position (artefact expected)
- [ ] `hold $SUB 2` → region replaced live while held, **snaps back** on release
- [ ] `tap $SUB ; sleep 4 ; tap $SUB` → change **sticks**, `u` shows; `tap $UNDO` reverts it
- [ ] commit an overdub, then `hold $SUB 2` (audition + revert), then `tap $UNDO` → still undoes the **overdub** (not a no-op)
- [ ] `tap $ODB ; sleep 200 ; tap $ODB` → no crash; oldest regions just stop being undoable

### M4 — Loop Window

- [ ] CTRL_3 down from full CW → `Wnnnms` + bracket bar; playback confines to the window, clean at the window wrap
- [ ] CTRL_4 sweep → window slides through the loop; playhead still sweeps the whole timeline
- [ ] LFO into CTRL_4 jack → tape-scrub / granular scan of your recording
- [ ] CTRL_3 to minimum (~30 ms) → chattering grain loop, no clicks (per-grain Hann below 60 ms)
- [ ] `tap $REV` / `tap $RET` while windowed → operate relative to the window
- [ ] CTRL_3 full CW → identical to M3 (window off)
- [ ] `tap $ODB` / `hold $SUB` while windowed → write inside the window

**Known M4-v1:** ~60–300 ms windows read a touch flat (read-side crossfade
trade-off); a window straddling the recorded loop's origin (CTRL_4 near max +
longish window) can tick at that internal seam.

### M5 — Clock / quantise / SYNC / PHASE

Patch a clock module (or an LFO square) into **GATE_IN_1**. The OLED shows
`nnnBPM` + a beat flash when a clock is detected.

- [x] no clock patched → Record / Mute / Substitute act immediately (M4 behaviour)
- [x] clock present, `tap $REC` from EMPTY → `ARM`; recording starts on the next
      pulse; `tap $REC` again → `ARM`; closes on a pulse → loop length is a whole
      number of clock periods
- [x] `tap $MUTE` with clock → `ARM`, then `MUTE` on the next pulse; loop output
      fades out (~2 ms, no click), input still passes
- [x] `tap $MUTE` again → loop returns on a pulse, **phase-locked** (it kept
      running silently)
- [x] `tap $SUB` / `tap $SUB` with clock → substitute region snaps to pulse
      boundaries
- [x] **SYNC OUT** (gate out): scope/LED shows one pulse per loop cycle; follows
      Retrigger, window length, reverse
- [x] **PHASE** (CV out 1): rising ramp over each cycle; halves rate at 0.5×,
      runs backward under Reverse, shortens to the window when windowed
- [x] pull the clock cable → after ~4 missed pulses `BPM` clears and behaviour
      returns to immediate

### M5.1 — clock-aware Loop Window

- [ ] no clock: CTRL_3/CTRL_4 behave exactly as M4/M5 (continuous, `Wnnnms`)
- [ ] clock patched, turn CTRL_3 through its range → OLED shows `W1/8 W1/4 W1/2
      Wx1 Wx2 Wx4 Wx8` in steps (not a continuous ms readout)
- [ ] turn CTRL_4 → window start jumps between clock-period boundaries
- [ ] each step is click-free
- [ ] pull the clock mid-window → falls back to continuous scan smoothly

**Known M5-v1:** clock tracked at block resolution (~0.7 ms jitter); quantise is
to the clock *pulse* (bar/subdivision needs the config menu at M7); `Record` no
longer has a gate input on the prototype (encoder / note 60 only).

---

## 5. Notes

- Serial logging is off — that USB port is MIDI now. All state is on the OLED.
- `receivemidi dev "Daisy"` shows the colour-echo notes (60–66, velocity =
  colour id) the module sends when a latch changes.
- To reflash: hold BOOT + tap RESET on the Seed, then `make program-dfu`. The
  `Error during download get_status` at the end is harmless (dfu-util leave-request quirk).

