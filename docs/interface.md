# Palimpsest — interface reference

Every UI element under consideration for the final module: pots, buttons, the
encoder, inputs, outputs, LEDs, display. Each entry notes whether it's
**settled** (design decided), **prototyped** (actually wired on the Patch
bench today, per [`testing.md`](testing.md)), or **open** (still a decision to
make). Design spec: [`../CLAUDE.md`](../CLAUDE.md).

Rules every control obeys (from CLAUDE.md): **twin controls** (hand control +
adjacent jack), **length is the verb** (tap=latch, hold=momentary, gate=level),
**colour is the contract** (red=destructive, cyan=playback-safe, green=undo,
amber=feedback), **screen is a timeline**.

Current count: **5 pots**, **8 buttons**, **1 encoder**, **~19 jacks**, **8 LEDs**,
**1 OLED** — the concrete input to the still-open panel-HP decision.

---

## Pots / knobs

| Control | Colour | CV jack | Does | Status |
|---|---|---|---|---|
| Feedback / Decay | amber | yes | loop persistence; 100% = bit-exact freeze | **settled** · prototyped (CTRL_1) |
| Speed | cyan | yes (~1V/oct) | varispeed 0.25×–4×, detents ½/1/2× | **settled** · prototyped (CTRL_2) |
| Window Length | cyan | yes | scannable-region size, 30 ms floor, full CW = off | **settled** · prototyped (CTRL_3) |
| Window Start | cyan | yes | scan position through the loop | **settled** · prototyped (CTRL_4) |
| Dry / Loop Mix | — | maybe | input-monitor level vs. loop level | **settled** (design) · not yet on the bench — the Patch always mixes both to all 4 outs |
| Tile Count | — | — | how many copies Crop & Tile makes | **open** — see Q1 below |

## Buttons *(each a twin control: button + gate jack)*

| Control | Colour | Gesture | Does | Status |
|---|---|---|---|---|
| Record | red | tap = advance EMPTY→REC→PLAY→EMPTY; hold = record-while-held | defines the loop; quantised to CLOCK when present | **settled** · prototyped (encoder + MIDI note 60). No bench gate — GATE_IN_1 became CLOCK IN at M5; final module needs Record's own gate jack, separate from Clock |
| Overdub | red | tap = latch / hold = momentary | additive write | **settled** · prototyped (MIDI note 61 only) |
| Substitute | red / cyan | hold = audition + auto-revert; tap-latch = commit | replace write — the crown jewel | **settled** · prototyped (MIDI note 62 only) |
| Mute | cyan | tap = latch / hold = momentary; quantised | loop output on/off; phantom playhead keeps running | **settled** · prototyped (MIDI note 63 only) |
| Reverse | cyan | tap = latch / hold = momentary | direction flip | **settled** · prototyped (MIDI note 64) |
| Retrigger / Stutter | cyan | tap = jump; gate = stutter | jump to window start | **settled** · prototyped (MIDI note 65 + GATE_IN_2) |
| Undo | green | impulse, toggles undo⇄redo | one-level undo | **settled** · prototyped (MIDI note 66 only) |
| Tile / Copy | red? | tap = crop + append; hold + clock = build per pulse | Crop & Tile (M5.2, not yet built) | **open** — see Q1 |

## Encoder

One physical encoder is shared across at least three jobs:

- **Turn** — Loop Select bank navigation (M6, not yet built), *or* scroll a
  config-menu parameter.
- **Push** — open, see Q3.
- **Right now on the bench** it stands in for the Record button (tap/hold) —
  a stand-in, not the final mapping; the final module gets a dedicated Record
  button.

**Open — see Q2:** with Loop Select nav, config-menu nav, and possibly
Tile-count all wanting the encoder, what decides which job it's doing at a
given moment?

## Inputs (jacks)

| Jack | Does | Status |
|---|---|---|
| AUDIO IN | mono for v1 | **settled** · prototyped (IN1; hardware-normalled to IN2-4, see CLAUDE.md) |
| CLOCK IN | quantise reference; drives SYNC/PHASE timing | **settled** · prototyped (GATE_IN_1) |
| Per-function CV / gate jacks | one per pot and per button (twin-control rule) | **settled** (design) · only 4 knob-CVs + 2 gates exist on the Patch bench; the rest are MIDI-note-only for now |

## Outputs (jacks)

| Jack | Does | Status |
|---|---|---|
| DRY OUT / LOOP OUT | separate monitor vs. loop signal (or one OUT + the Mix knob) | **settled** (design) · not on the bench — Patch currently mixes both to all 4 outs |
| SYNC OUT | gate pulse once per loop/window cycle | **settled** · prototyped (Patch gate output) |
| PHASE CV OUT | 0→~5 V ramp over the audible cycle; follows speed/reverse/window | **settled** · prototyped (Patch CV out 1) |
| Second CV OUT | assignable | **open** — gesture CV? loop envelope follower? phase at a division? (carried from CLAUDE.md) |

## LEDs

One per button, in the function's colour, dim when idle — the physical
expression of "colour is the contract." Logic exists (MIDI colour-echo to the
Pico rig); no physical LED hardware wired yet.

## Display

OLED timeline, three tiers (see CLAUDE.md's UI section for the full spec):
- **Tier 1** (always on): loop, playhead, window bracket, reverse arrow,
  feedback level, speed ratio.
- **Tier 2** (contextual): gesture lane, loop-select dots, quantise division,
  undo flag.
- **Tier 3**: probability ticks, clock ticks, per-slot mini-overview.

Built so far on the bench: Tier 1 plus BPM, ARM, MUTE, the engaged-function
row, and CPU load — a working subset, not the full spec.

---

## Open questions

**Q1 — How do you specify "copy N times" for Crop & Tile?**
- **(A) Repeated taps** — each tap of TILE appends one more copy. No new
  control; matches the gesture-first design. Can't preview or set an exact
  count in advance.
- **(B) Hold TILE + turn the encoder** — live "×N" on screen, release to
  commit. Precise, but time-shares the already-double-booked encoder.
- **(C) Hold TILE + clock pulses** — one copy appended per pulse while held
  (already sketched into M5.2). Musical, no new control — but needs a clock.
- **(D) Dedicated CV/knob for count** — patchable/automatable, but spends a
  whole twin-control pair on an occasional operation.

  Lean: **A** as the always-available baseline, **C** as the natural upgrade
  when a clock is present. Neither needs a new control. B is the "precise"
  option if we're willing to arbitrate the encoder for it; D is probably
  overkill for how often this gets used.

**Q2 — Encoder job arbitration.** With Loop Select nav, config-menu nav, and
possibly Tile-count all wanting the encoder, what decides which job it's doing
right now? (e.g. "the encoder means Loop Select, unless some other function's
button is currently held, in which case it modifies that function.")

**Q3 — Encoder push semantics.** Confirm/select inside the config menu, sure —
but what does a push do during normal performance? Nothing? Enter config
mode? An immediate Loop-Select override (bypassing the quantised switch)?

**Q4 — Window on/off legibility.** "Off" is currently just a knob position
(length at max) with the bracket disappearing from the timeline as the only
tell. Enough, or does it want its own indicator?

**Q5 — Dry/Loop mix control.** Settled in spirit, unassigned in practice —
does it get a real panel knob, or is it a set-once value that belongs in the
config menu instead of the performance surface?

**Q6 — Second CV out** default assignment (carried from CLAUDE.md).

**Q7 — Panel HP.** Carried from CLAUDE.md; the counts at the top of this
document (5 pots, 8 buttons, 1 encoder, ~19 jacks, 8 LEDs, 1 OLED) are the
concrete input to that decision.
