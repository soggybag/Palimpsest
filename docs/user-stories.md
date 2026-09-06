# Palimpsest — user stories

Performance workflows that exercise the whole grammar. Each is an acceptance
reference: the **Must verify** boxes are checked on hardware as the relevant
milestone lands. Design spec: [`../CLAUDE.md`](../CLAUDE.md).

Context: a Eurorack module in a rack, modular signal levels, a clock module and
CV sources patched in. "Hands" means panel controls *and* patch cables.

---

## 1 — Ambient layering (Frippertronics)

Build an evolving pad from one held phrase, no click, let it erode.

1. Guitar-via-preamp / synth into IN; dry heard at DRY OUT (defeatable mix).
2. Tap **Record**, play an 8 s swell, tap **Record** — loop closes and plays.
3. Tap **Overdub** (latched) for one cycle, add a harmony, tap off. Feedback
   < 100 % so the first pass is already quieter.
4. Bad note — tap **Undo**; the overdub pass is gone.
5. Let it run: it erodes over minutes as you add single notes. Push Feedback to
   100 % to freeze the bed; drop to 70 % to clear fast.

**Exercises:** Record, Overdub, Feedback, Undo, dry/loop mix.
**Must verify:**
- [ ] input always mixed to output; mix is a real control
- [ ] Overdub never changes loop length
- [ ] Feedback acts on recirculation at all times; 100 % = bit-exact freeze
- [ ] Undo removes exactly the last destructive span
- [ ] no click at the loop wrap at any Feedback setting

## 2 — Backing loop for a solo (band context)

1-bar chord loop locked to the drummer; solo; drop it for the bridge; back in phase.

1. Drummer's clock -> **CLOCK IN**. Menu: quantise = 1 bar.
2. **Hold Record** across the bar, release near the end — close snaps to 4 beats.
3. Solo over the loop; dry input passes through.
4. Bridge: tap **Mute** — loop output stops at the next bar; your live playing is
   untouched; phantom playhead keeps moving.
5. Tap **Mute** again — loop returns on the downbeat, phase-locked.
6. Wrong chord baked in — tap **Record** twice to replace the whole loop
   (old -> one Undo).

**Exercises:** clock-quantised Record, Mute, CLOCK IN, re-record.
**Must verify:**
- [ ] Mute exists: tap-latch / hold-momentary, quantised
- [ ] phantom playhead keeps running while muted; re-entry is phase-locked
- [ ] Record close snaps to N clock divisions; free-run with no clock
- [ ] CLOCK IN is its own jack

## 3 — Turntablist re-editing (the Lafosse workflow, crown jewel)

Perform a recorded spoken phrase as raw material: chop, swap, reverse-stab, zoom.

1. Record a 4 s phrase, no clock.
2. /16 clock -> **Substitute** jack; speak new words into IN — each 1/16 slice
   swaps while the pulse is high, original returns between pulses.
3. Sweep **Speed** 1x -> 0.5x: phrase drops an octave; clocked substitutes still
   land rhythmically (they follow the clock, not the read pointer).
4. Tap **Reverse** (latched) — whole loop backward; tap again forward; then
   *hold* for a two-word backward stab.
5. Tap **Window** on; length down to ~600 ms around one phrase; slide start.
6. Current mangle is gold — tap **Substitute** itself (latched) to commit the
   last swap; snapshot for Undo.

**Exercises:** clocked Substitute (audition + commit), Speed, Reverse, Window.
**Must verify:**
- [ ] hold / clocked Substitute auto-reverts (non-destructive, cyan)
- [ ] tap-latch Substitute commits destructively (red, snapshot for Undo)
- [ ] clocked functions follow the external clock, not the varisped playhead
- [ ] Reverse / Retrigger / "loop start" use Window bounds when a Window is active
- [ ] Speed detents at 1/2x, 1x, 2x; 1V/oct CV rides through the detent

## 4 — Beat construction from a drum machine

1. Drum-machine clock -> CLOCK IN, quantise = 1 bar. Hold **Record** a bar.
2. Snare trigger -> **Substitute** jack (as a gate): each hit, a live rimshot
   replaces that slice; original snare returns between hits.
3. Like it — tap **Substitute** latched to commit that slot.
4. **Window** length -> 1/2 bar (half-time loop of beats 1-2). LFO at 1/4 the
   clock -> **Window-start**: the half-bar window walks the full bar over 4 cycles.
5. Audio-rate gate (~40 Hz) -> **Retrigger** for a stutter buildup; pull it.
6. **Overdub** a shaker. Feedback 100 % — nothing decays.

**Exercises:** gate-driven Substitute, Window + CV, audio-rate Retrigger, Overdub.
**Must verify:**
- [ ] one Substitute jack takes both a steady clock and an arbitrary gate, no mode
- [ ] audio-rate Retrigger stutters click-free (overlapping crossfades)
- [ ] Window-start CV past the buffer end wraps
- [ ] Feedback 100 % is bit-exact over minutes (no drift/degrade)

## 5 — Granular self-scanning from a vocal phrase

1. Record a 2 s vowel. **Window** on, length -> ~120 ms — a chattering grain loop.
2. Slow LFO (0.1 Hz) -> **Window-start**: the grain scans the vowel, formants sweep.
3. Pressure CV -> **Speed** (1V/oct): press = grain pitches up; detent still
   catches 1x at CV = 0.
4. Tap **Reverse** latched — grains play backward while the window scans forward.
5. Bring Window length up slowly — texture resolves back into the sung word.

**Exercises:** small Window, Window-start CV, Speed CV, Reverse-in-window.
**Must verify:**
- [ ] min Window length 30 ms; auto Hann window below ~60 ms (no grain clicks)
- [ ] Speed CV is exponential, summed on top of the knob base
- [ ] Reverse flips read direction within the window; the window still advances

## 6 — Hands-free evolution (gesture recorder + probability)

1. Loop recorded (8 s), Feedback ~90 %.
2. Tap **Gesture Record**; perform a 16 s pass of edits (reverse-stab, retrigger,
   Speed dip, momentary Substitute); tap off.
3. The 16 s gesture loop re-fires against the 8 s audio loop — edits land in a
   different spot each cycle (phase drift = variation).
4. Turn **gesture offset** — slide the gesture track against the audio phase.
5. Menu: per-jack probability — Substitute 60 %, Reverse 80 %. Patch a clock to
   both — generative editing on top of the gesture lane.
6. Slow LFO -> **Feedback** jack: the whole thing erodes and regenerates. Walk away.

**Exercises:** gesture recorder, gesture offset, per-jack probability.
**Must verify:**
- [ ] gesture recorder captures events (function + hold-duration + phase) + one
      assignable CV lane
- [ ] gesture-loop length independent of the audio loop; offset shifts phase
- [ ] probability is menu-set; failed rolls show as dim ticks on the timeline
- [ ] gesture lane + probability run together as two event sources

## 7 — Multi-loop arrangement (sequencer-driven)

1. Build loop A (verse). **Loop Select** encoder -> slot 2; build loop B (chorus),
   own length. Slots 3, 4 likewise.
2. Each slot remembers its own Speed / Reverse / Window / Feedback.
3. Stepped CV (0/1/2/3 V) -> **Loop Select** jack, quantised to slot index;
   switches at end of the current loop.
4. Hard cut on a drop: hold the encoder while the CV moves — switch now, mid-loop,
   short crossfade.
5. Edits during the chorus stay on B; A is untouched.
6. Back to slot 1: loop A is exactly as left (Feedback was 100 %).

**Exercises:** Loop Select, per-slot state, CV addressing, quantised vs immediate.
**Must verify:**
- [ ] inactive slots freeze by default (menu option to keep them live)
- [ ] switch is end-of-loop quantised by default; modifier = immediate + crossfade
- [ ] per-slot state includes the gesture lane
- [ ] CV -> slot is 1 V/slot, clamped
- [ ] SDRAM budget holds with 4-8 slots + snapshot pool + gesture lanes
