# Palimpsest — live-edit looper firmware (Daisy)

## What this is
A Eurorack looper that ports the Echoplex Digital Pro's *live editing* vocabulary
to a modular, CV-controlled form. Inspiration: Andre Lafosse's "turntablist guitar"
approach — the loop is raw material to be re-edited in real time (substitute, reverse,
window, speed), not just layered up (Frippertronics). The design goal: keep the EDP's
depth, lose the menu-diving. Every performance function is one gesture AND a patch point.

**This is a rack citizen, not a pedal.** It expects Eurorack signal levels (±5 V audio,
~10 Vpp), lives next to a clock module and a stack of CV sources, and assumes the patch
cables are half the interface. A guitar/line front end is a separate future project,
not a mode.

## Eurorack integration (the operating context)
- **Audio I/O at modular level.** No instrument preamp on board — hot signals in and
  out. Line/instrument sources need a preamp module upstream.
- **Dry vs loop is a mix, not an assumption.** Input-to-output thru is a defeatable
  control (knob + jack). The loop bus has its own output. On the 4-out prototype:
  separate DRY OUT and LOOP OUT; on a 2-out final build, one MIX knob.
- **CLOCK IN** is a dedicated jack (not one of the per-function jacks). Drives
  quantise, stutter reference, gesture-loop sync. Free-running when no clock present.
- **SYNC OUT** — gate pulse at each loop start (Window start when a Window is active).
  Lets a sequencer / envelope lock to the loop.
- **PHASE CV OUT** — 0→5 V ramp over the loop cycle, tracking the playhead (follows
  Speed / Reverse / Window). A free modulation source.
- **Second CV out** — assignable (gesture-lane CV, loop envelope follower, or a phase
  ramp at a division). Default TBD.
- Every panel control is mirrored by a jack (twin-control rule); CV is **summed on top**
  of the knob/switch, not switched in.

## Hardware target
- **Prototype on:** Electro-Smith **Daisy Patch (legacy)** — on hand. `daisy::DaisyPatch`
  BSP. 4-in / 4-out 24-bit audio, 4 CV in, 2 CV out, 2 gate in, 1 gate out, 4 knobs,
  encoder w/ push, TRS MIDI in/out, microSD, 128×64 OLED (SSD1309). Original Daisy Seed
  inside (STM32H750, 64 MB SDRAM, AK4556 codec). Better than patch.Init() here: real
  dry + loop outputs, more CV in, standard OLED, hardware MIDI. Confirm the BSP matches
  the actual board revision before assuming the codec.
- **External control surface for prototyping:** Pimoroni **Pico RGB Keypad** (16 keys,
  16 RGB LEDs, RP2040) ↔ Patch over **bidirectional TRS MIDI**. Keys → MIDI Note On/Off;
  LED colours ← CC/Note from the Daisy, so the rig renders "colour is the contract" from
  real engine state. Rig firmware ~50 lines MicroPython. Lets us test the gesture grammar
  (tap-latch vs hold-momentary) with fingers while the engine is still record→play→feedback.
- **Final build:** custom carrier PCB around a bare **Daisy Patch Submodule** (STM32H750,
  480 MHz M7, 64 MB SDRAM, PCM3060 codec). Patch SM chosen over a bare **Seed3** (also on
  hand) because it already carries the Eurorack analog conditioning — bipolar CV input
  scaling / protection, modular audio levels, gate comparators. Seed3's upside (TAC5242
  codec, 32-bit / 192 kHz, 8 MB flash, USB-C) does not outweigh redoing that analog
  design for v1; revisit for a "Plus" revision. Patch SM breaks out SDMMC, so an SD slot
  is a carrier-board option.
- Patch quirk: the four audio **input jacks are hardware-normalled** in a chain
  (IN1→IN2→IN3→IN4) — an unpatched jack carries the previous input, and firmware
  cannot detect insertion. v1 reads IN1 only; use shorting plugs on IN2–4 for
  isolated bench tests. Moot on the Patch SM final build (own input stage).
- **Persistence:** loops are RAM-only (lost on power-down) for v1. microSD save/recall on
  the legacy prototype; final-build persistence TBD (QSPI flash slots vs. SD on carrier).

## Memory budget (the enabling constraint)
64 MB SDRAM, identical on Seed / Patch SM / Seed3. The **int16 storage** decision
(process in float, store the buffer as int16) is what makes the whole feature set fit.

| Format | Rate | 44 MB loop pool |
|---|---|---|
| float32 mono @ 48 kHz | 192 KB/s | ~240 s (4 min) |
| **int16 mono @ 48 kHz** | **96 KB/s** | **~480 s (8 min)** |

v1 partition (mono, int16, of 64 MB):
- **Loop audio: 44 MB.** v1 = **4–8 fixed slots**, ~60–120 s each (count is a config
  choice). v1.1 refinement = one shared arena the slots draw from dynamically
  (arena-allocated, compact on clear) for ~480 s spread however you use it.
- **Undo / Substitute snapshot pool: 16 MB** (block copy-on-write). ~175 s of edited
  regions total. When exhausted, the oldest snapshot drops and that region becomes
  un-undoable.
- **Headroom / gesture lanes / scratch: ~4 MB.** Gesture lanes are event lists — tiny.

**Does the Seed have enough memory for Loop Select? Yes, comfortably.** int16 storage
buys ~8 minutes of mono loop time in 44 MB; four fixed slots is ~120 s each, eight is
~60 s each, all inside 64 MB alongside the 16 MB COW pool. Only **stereo** makes it
tight: int16 stereo ≈ float32 mono ≈ ~240 s / 44 MB, and no larger SDRAM exists on the
platform. **Mono for v1** stands; stereo "Plus" accepts ~4 min total.

Undo is **not** a second full buffer — block COW snapshots only the regions a destructive
op is about to overwrite. You pay memory for what you actually edit, and Substitute rides
the same mechanism (snapshot region on entry → restore on release, or keep on commit).

## Feature set → controls (the interface grammar)
Four rules the whole design obeys:
1. **Twin controls** — every function is a hand control + an adjacent patch jack. CV sums
   on top of the hand control.
2. **Length is the verb** — tap = latch (toggle), hold = momentary. Maps 1:1 onto gates
   (gate-high = active). One rule replaces the EDP's SUS-vs-latched distinction.
3. **Colour is the contract** — red = writes to the buffer (destructive); cyan = playback
   only (non-destructive, always safe); green = Undo; amber = Feedback.
4. **Screen is a timeline** — playhead, Window bracket, substitute regions, and a second
   lane for edit gestures. Not a menu.

### Functions
- **Feedback / decay** (amber, knob + CV) — loop persistence, **always active on the
  recirculating buffer** (not only during Overdub). 100 % = bit-exact freeze (gain stage
  bypassed); below = old material erodes. **Hard-capped at 100 % for v1** (no
  above-unity self-oscillation yet). The one control always in motion.
- **Record** (red, button + gate) — gate/hold defines loop length; tap to close. Close is
  **clock-quantised** when a clock is present (snaps to N divisions), free-run otherwise.
  Loop length is **immutable after close** — no Multiply/Insert. Use Window to play
  shorter; **hold-Record** for a fresh canvas (new length); tap-tap to re-record the same
  length (old loop → one Undo).
- **Overdub** (red, button + gate) — hold to layer while held / tap to latch. Never
  changes loop length. Writing while **Reverse** is active lays new material in reversed
  orientation (EDP behaviour). Writing while **Speed ≠ 1×** is allowed and writes at the
  moved read position (accept the artefact — it is usable).
- **Substitute** (*crown jewel*, button + gate) — **two behaviours, disambiguated by
  rule 2:**
  - **hold / clocked gate → audition.** Live input replaces that region while active; on
    release the original returns. Non-destructive. Drawn **cyan**.
  - **tap-latch → commit.** Region is written destructively from here on; pre-state
    snapshotted for Undo. Drawn **red**.
  A steady clock gives rhythmic chop-and-swap; an arbitrary gate (a drum trigger, say)
  gives event-driven swaps. Same jack, no mode switch — it responds to edges.
- **Mute** (cyan, button + gate) — loop-output on/off. tap-latch / hold-momentary,
  **clock-quantised**. A **phantom playhead keeps running** while muted so re-entry is
  phase-locked. Essential in a rack next to a band or a sequencer.
- **Reverse** (cyan, button + gate) — tap latches / hold momentary. Playback only.
  Operates within **Window** bounds when a Window is active.
- **Speed** (cyan, knob, 1V/oct CV) — varispeed (pitch tracks speed). Fractional read
  pointer + **4-point Hermite** interpolation. Knob **detented at ½× / 1× / 2×**; 1V/oct
  CV summed in the exponential domain on top of the knob base (CV offset transposes
  cleanly; the detent is a knob feel, CV rides through it).
- **Loop Window** (cyan, start + length, each knob + CV) — scannable sub-region. CV on
  start = granular scanning of your own performance. **Min length 30 ms**; below ~60 ms
  an auto Hann window is applied per read to kill grain clicks. Start-CV past buffer end
  **wraps**. When active, Window bounds are the reference frame for Reverse, Retrigger,
  SYNC OUT and PHASE CV.
- **Retrigger / Stutter** (cyan, button + gate) — tap jumps to loop/Window start;
  audio-rate gate = stutter. Every jump is crossfaded; the declick layer must handle
  overlapping fades at 40 Hz+.
- **Loop Select** (cyan, encoder + CV) — CV-addressable bank of loops. **1 V/slot,
  clamped.** Switch is **end-of-loop quantised** by default; hold the encoder (or a
  modifier gate) for an immediate mid-loop switch with a short crossfade. Each slot
  stores its own Speed / Reverse / Window / Feedback / gesture lane. **Inactive slots
  freeze** by default (menu option to keep them recirculating).
- **Undo** (green, dedicated button + gate) — real-time undo of the last destructive
  span, where a span = "everything written since the last red-function engage." **Depth 1
  for v1**; the button toggles (re-press = redo). First-class, never a menu item.

### Beyond the EDP (modular-native)
- **Gesture recorder** — records edit triggers as a second loop: discrete events
  (function + hold-duration + loop-phase timestamp) **plus one assignable CV lane**
  (e.g. Speed motion) in v1. Gesture-loop length is independent of the audio loop; an
  **offset** knob slides the gesture track against the audio phase. Playable, offsettable,
  stored per slot.
- **Quantised commit + per-jack probability** — patched clocks land on subdivisions;
  optional Bernoulli gate per function for generative editing. Set-once config → lives in
  the encoder menu, **not** on the performance surface. Its *effect* is drawn on the
  timeline (failed rolls = dim ticks) so you can see it working.

## UI — the timeline screen (128×64 OLED, must read from 2 m)
Draw priority; Tier 1 is always on, ~5 elements max at once.
- **Tier 1 (always):** loop as full panel width; playhead; Window bracket; Reverse
  direction arrow; Feedback level; red fill over regions being written (Record / Overdub /
  committed Substitute); Speed ratio as a number ("0.50×").
- **Tier 2 (contextual — shows while you touch the related control, then fades):**
  gesture lane with event marks + offset position; Loop Select slot dots; quantise
  division; "Undo available" flag.
- **Tier 3:** probability roll feedback; clock-tick markers along the timeline; per-slot
  mini-overview while turning Loop Select.

Encoder menu (set-once config only, never performance): quantise division, per-jack
probability, gesture CV-lane assignment, keep-inactive-slots-live, dry/loop mix trim,
save / recall.

## Architecture direction (design together before coding)
- One int16 loop-audio arena in SDRAM; slots are offset + length views into it.
- Read pointer + write pointer model; fractional read with Hermite interpolation.
- **Declick / crossfade is a first-class layer** built with the buffer engine — every
  edit boundary (loop wrap, retrigger, punch in/out, reverse flip, window jump,
  substitute in/out) gets a 2–10 ms equal-power fade. ~40 % of "feels pro vs. toy".
- **Block copy-on-write snapshot layer** — shared by Undo and Substitute. Snapshot a
  block before its first destructive write since the last commit; restore on undo /
  substitute-release.
- **Clock / quantise decoupled from the read pointer.** Loop length in samples is the
  source of truth (free-run). A separate phase tracker + quantiser off CLOCK IN snaps
  gesture events and, on request, loop length. Clocked functions follow the clock, not
  the (varisped) playhead.
- **Control layer** abstracts "button vs gate" into one event source per function
  (rule 2): each yields `{engaged, latched|momentary, phase}`. Gesture lane and per-jack
  probability are additional event sources feeding the same layer.
- Audio-callback path allocation-free and deterministic; UI / display on the main loop.
- Block size 32 for tight retrigger / stutter response.

Module layout:
```
Palimpsest.cpp     entry, audio callback, hw glue (DaisyPatch BSP)
src/
  LoopBuffer       int16 SDRAM store; read/write pointers; Hermite read; declick
  Snapshot         block COW layer  ->  feeds both Undo and Substitute
  Clock            ext clock in, phase accumulator, event quantiser, SYNC/PHASE out
  Engine           record / overdub / play / feedback / mute; edit state machine
  Controls         rule-2 abstraction: (button | gate) -> Event{engaged, latch, phase}
  GestureRec       second loop of edit events + one CV lane, offsettable
  ui/Display       the timeline scope (main loop, never in the callback)
```

## Milestones (vertical slices — hear / test on hardware each step)
0. **Skeleton + I/O** — builds and flashes to Daisy Patch (`DaisyPatch` BSP); audio
   passes through at modular level; knobs / gates / encoder read and printed; OLED shows
   a bare timeline. Toolchain end-to-end.
1a. **Core buffer engine** — mono int16 record → loop playback with variable Feedback;
    declick layer and Hermite read in from the start. Get this *musical* first.
1b. **(parallel) Pico RGB Keypad bench rig** — MicroPython, bidirectional TRS MIDI,
    8–10 keys mapped to the function grammar, LED colour from engine state. Test
    tap-latch vs hold-momentary with fingers.
2. **Playback edits (safe first)** — Reverse, Speed (Hermite + detent + 1V/oct),
   Retrigger / Stutter. Verify no clicks; measure worst-case SDRAM non-linear read load.
3. **Snapshot layer → Overdub + Substitute (audition + commit) + Undo** — one milestone,
   one mechanism.
4. **Loop Window** scanning — start / length + CV, wrap, sub-60 ms Hann.
5. **Clock** — quantised Record close, quantised Mute, clocked Substitute; SYNC OUT and
   PHASE CV OUT.
6. **Loop Select** — fixed slots first (per-slot state, quantised vs immediate switch,
   freeze-inactive); shared arena as v1.1.
7. **Gesture recorder** + per-jack probability config + timeline polish.

Build vertically: record → play → feedback solid and musical before any edits.

## Toolchain
- C++ with **libDaisy + DaisySP**. Build / flash via arm-none-eabi-gcc, make, dfu-util.
- Chosen over Pd / gen~ / Faust because the core is custom pointer surgery on an audio
  buffer, which expresses cleanly in C++.
- Share libDaisy / DaisySP with the sibling projects under `../common/` if that is how
  the existing setup is arranged — confirm the local Daisy dev env before assuming.

## Working preferences
- Direct and practical; concrete results over clarifying questions.
- Technically accurate, actionable, minimal hedging.
- Propose a plan, then build in vertical slices I can hear / test on hardware early.

## Open decisions (the few genuinely still open)
- **Panel format / HP.** ~19 jacks (12 function + 7 I/O) + 7 buttons + 4 pots + encoder +
  OLED does not fit 22 HP. Realistically 28–34 HP, or a main + expander split (push Loop
  Select, gesture I/O, and the extra CV jacks to the expander).
- **Final-build persistence** — QSPI flash loop slots vs. SD on the carrier vs. RAM-only
  ship.
- **Second CV out** default assignment (gesture CV? loop envelope follower? phase at a
  division?).
- **Stereo "Plus" variant** — accepts ~4 min total loop time; worth it for ping-pong
  window scanning, or keep Palimpsest mono-forever and do stereo as a different module?
- **Module name** (working title "Palimpsest").
