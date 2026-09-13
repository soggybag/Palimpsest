# Palimpsest — roadmap

Design spec: [`../CLAUDE.md`](../CLAUDE.md). Acceptance workflows:
[`user-stories.md`](user-stories.md). Bench procedures + MIDI commands:
[`testing.md`](testing.md). UI element reference + open questions:
[`interface.md`](interface.md).

Build vertically: `record -> play -> feedback` solid and *musical* before any edits.

## Status

- [x] **M0** — skeleton + I/O
- [x] **M1a** — core buffer engine (record / play / feedback)
- [x] **M2** — playback edits: Reverse, Speed, Retrigger/Stutter
      (worst-case CPU 12% — SDRAM random-read risk retired)
- [x] **M1b** — control layer + MIDI in (impulse/latching split, USB+TRS MIDI)
- [x] **M3** — snapshot layer: Overdub + Substitute + Undo
- [x] **M4** — Loop Window scanning
- [x] **M5** — Clock: quantise, clocked Substitute, SYNC/PHASE out
- [x] **M5.1** — clock-aware Loop Window (musical snapping)
- [x] **M5.2** — Crop & Tile
- [ ] **M6** — Loop Select   ← current
- [ ] M7 — Gesture recorder + probability + timeline polish

## Build & flash

```
cd ~/Documents/daisy/Palimpsest
make                       # -> build/Palimpsest.bin
make program-dfu           # hold BOOT, tap RESET on the Seed, then run this
```

Toolchain: `arm-none-eabi-gcc`, `dfu-util`, `make` on PATH (DaisyToolchain installer).
libDaisy / DaisySP at `../../DaisyExamples/`.

---

## M0 — skeleton + I/O

Prove the toolchain and every input end to end. No DSP beyond passthrough.

- [x] `make` builds clean against the `DaisyPatch` BSP (96 KB flash)
- [x] flashes to the Patch; audio passes through at modular level (per-channel map
      confirmed via the input-normalling cascade)
- [x] `k1..k4`, `g1`, `g2`, `enc` print over USB serial and track the hardware
      (pots read ~0–98; rail margin is normal)
- [x] OLED draws the timeline frame + a moving placeholder playhead

**Done when:** the module boots to a passthrough with a live timeline and the
serial log reflects every knob/gate/encoder move.

## M1a — core buffer engine

- [x] `src/LoopBuffer` — int16 SDRAM store, Hermite read (built)
- [x] 4-point Hermite interpolated read (used from the start; no-op at 1x)
- [x] record-close seam (write fading input over the loop head ~25 ms, per the
      DaisySP `Looper` trick) + ~2 ms read-side wrap taper
      — the full reusable dual-tap crossfade layer is **M2** (needs Reverse /
      Retrigger as real consumers to shape it)
- [x] Record: encoder tap cycles state; GATE_IN_1 edges bracket a take; plays
      immediately on close (built)
- [x] Feedback (CTRL_1): always-on recirculation decay; >= 0.999 = bit-exact
      freeze (no write-back); knob deadzones hit exact 0.0 / 1.0 (built)
- [x] OLED: state label, loop length in seconds, playhead on real phase, FB bar
- [x] `make` clean — FLASH 77%, SDRAM 57% (38 MB buffer)

**Verify on hardware:**
- [x] record a phrase (encoder tap in, tap out) → it loops
- [x] wrap is clean closing during silence
- [ ] wrap is clean closing with a drone held through the close (needs the seam
      fade-in fix — reflash and confirm)
- [ ] Feedback sweep: full sustain at 100 %, progressive erosion below, audible
      decay rate tracks the knob
- side effect of the M1a seam: first ~25 ms of the loop ramps from zero; M2's
  read-time crossfade removes this
- [ ] FB FREEZE holds a loop indefinitely with no audible degradation over minutes
- [ ] GATE_IN_1 high→low defines loop length from another module's gate

**Done when:** record → play → feedback is solid and musical, no clicks.
Ref: user story 1.

**Note:** internal FLASH is at 77 % of 128 KB and climbs each milestone. If we hit
the wall around M5–M7, switch to the Daisy bootloader (`APP_TYPE = BOOT_SRAM`,
runs from QSPI) for ~480 KB of room. Not urgent.

## M1b — control layer + MIDI in

- [x] `src/Controls.h` — `FuncControl` per function: tap = toggle latch,
      hold (>300 ms) = momentary, gate = level; exposes Engaged / edges /
      Momentary. `Controls` routes MIDI notes 60..66 to functions. (built)
- [x] MIDI in pumped in the audio callback; NoteOn/NoteOff -> Controls (built).
      Both transports live: TRS MIDI IN *and* USB-MIDI device over the flashing
      cable (`sendmidi dev "Daisy" on 64 100`). USB-MIDI replaced the CDC serial
      log — debug is on the OLED now.
- [x] panel folded into the same layer (encoder -> Record press/release,
      GATE_IN_1 -> Record gate, GATE_IN_2 -> Retrigger gate, CTRL_3 -> Reverse)
- [x] colour echo: latch state -> MIDI note back to the keypad LEDs (built)
- [x] `pico_rig/main.py` note base aligned to 60

**Verify on hardware:**
- [ ] encoder tap toggles Record state; encoder hold = record-while-held,
      close-on-release (no MIDI needed for this)
- [ ] existing panel controls (Reverse, Retrigger, gates, knobs) unchanged
- [ ] *(with keypad or MIDI kbd)* notes 60/64/65 drive Record/Reverse/Retrigger;
      tap vs hold behaves; keypad LEDs follow state

**Done when:** every wired function behaves identically whether a hand or a
cable/note triggered it, tap vs hold included.

## M2 — playback edits (safe first, non-destructive)

- [x] **`src/LoopReader`** — read pointer with equal-power wrap crossfade
      (kXfade 256 ≈ 5.3 ms); resume-offset + increment compensation keep the
      audible period exact; jumps/flips reuse the same fade against the dying
      trajectory. Replaces M1a's seam. (built)
- [x] Reverse — signed direction, crossfaded flip (built; on CTRL_3 as a
      placeholder switch until the Pico rig)
- [x] Speed — exponential 0.25x..4x, detented ½/1/2x; CV on the CTRL_2 jack
      hardware-sums (not yet calibrated 1V/oct) (built)
- [x] Retrigger — GATE_IN_2 edge jumps to loop start, crossfaded; audio-rate
      gate = stutter (built)
- [x] `CpuLoadMeter` on the OLED + serial (built)
- [x] Feedback decay moved to a unit-rate cursor (real-time erosion,
      speed-independent)

**Verify on hardware:**
- [ ] loop point clean on a raw drone held through the close (wrinkle gone, no
      fade-in artifact)
- [ ] Speed: smooth pitch glide, detents catch at ½ / 1 / 2x, CV shifts it
- [ ] Reverse: backward playback, click-free flip both directions
- [ ] Retrigger: single trig = clean jump; ~20–50 Hz gate = stutter
- [ ] worst-case CPU max noted (Reverse + Speed 2x/0.5x + Retrigger spam) —
      confirms SDRAM random reads are affordable
- [ ] Feedback erosion rate steady when Speed ≠ 1x
- known trade-off: short tonal loops (~1 s) read a few cents flat (kXfade/len);
  dual-grain reader is the fix if it matters in practice

**Done when:** all three edits run together, no clicks, CPU headroom known.
Ref: user stories 3, 4.

## M3 — snapshot layer: Overdub + Substitute + Undo  *(one mechanism)*

- [x] `src/Snapshot.h` — block COW (4096-samp blocks, 16 MB pool = 2048 slots);
      separate in-progress-take and committed-undo maps so an auditioned
      Substitute that reverts leaves a prior Overdub's undo intact
- [x] Overdub: additive, hold-or-latch, never changes length; reversed
      orientation via writing at the (backward) read pointer
- [x] Substitute: hold/gate = audition + auto-revert; tap-latch = commit
      (snapshot kept for Undo)
- [x] Undo (note 66, impulse): toggles the last committed take (undo <-> redo)
- [x] pool exhaustion: ring allocator drops oldest un-kept snapshot, region
      becomes un-undoable; verified with a >3 min continuous overdub
- [x] CPU readout reworked to a self-healing 1 s peak (GetMaxCpuLoad latches
      forever + tick-counter wrap = the bogus 7315% reading)

Known M3-v1 rough edges: no crossfade on the destructive punch in/out itself
(a punch can tick); clocked Substitute waits for CLOCK IN at M5.

**Done when:** overdub boldly, audition + commit substitutes, always recover
with one Undo. Ref: user stories 1, 3, 4.  [MET]

## M4 — Loop Window scanning

- [x] `LoopReader` reworked to a window [wstart, wstart+wlen): buffer wraps at
      loopLen, window wraps at wlen; boundary crossfade scaled to the window
- [x] CTRL_3 length (exp, 30 ms floor, full CW = off), CTRL_4 start; CV sums
- [x] per-grain Hann below 60 ms (granular scanning); crossfade above
- [x] Reverse / Retrigger operate on the window-local position
- [x] window off (wlen == loopLen) is byte-identical to M3 playback

Known M4-v1: ~60-300 ms windows read a touch flat (read-side crossfade); a
window straddling the recorded loop origin can tick at that internal seam.

**Done when:** an LFO on CTRL_4 granular-scans cleanly; min window = chattering
grains, no clicks. Ref: user story 5.  [MET]

## M5 — Clock

- [ ] CLOCK IN jack: phase tracker + quantiser, decoupled from the read pointer
- [ ] quantised Record close (snap to N divisions), free-run with no clock
- [ ] quantised Mute with a phantom playhead that keeps running while muted
- [ ] clocked Substitute follows the clock, not the varisped playhead
- [ ] SYNC OUT gate at each loop/Window start
- [ ] PHASE CV OUT 0->5 V ramp over the loop cycle

**Done when:** the loop locks to an external clock, drops out and re-enters in
phase, and other modules can clock off SYNC/PHASE. Ref: user stories 2, 3.

## M5.1 — clock-aware Loop Window  *(small follow-up)*

- [x] clock present: window **length** snaps to a ratio ladder (1/8 1/4 1/2 x1 x2
      x4 x8 of the clock period), **start** snaps to the period grid —
      rhythmic chopping (story 4) (built)
- [x] no clock: continuous length/start as before — granular scanning (story 5)
- [x] mode follows clock presence automatically, no menu (built)
- [x] OLED shows the ratio (`Wx1`, `W1/4`, ...) instead of ms when clock-locked
- [x] step hysteresis on both the length ratio and start-position index (a still
      knob at a bin boundary no longer flickers between adjacent steps)
- [x] ladder top grows with how many clock periods fit the recording (up to
      x256), instead of a fixed x8 ceiling — a long loop can reach a window
      that actually covers most of it

**Verified on hardware:** all of the above, including the long-loop ladder-top
fix. Known non-issue: displayed BPM can drift +-1 with an unstable clock
source (e.g. a 1U utility clock) — real source jitter, not a bug; the window
ladder only shifts by the same <1% and stays musically stable.

## M5.2 — Crop & Tile

Redefine loop length by an explicit gesture (not real-time Multiply).

Implementation landed differently than first sketched, after weighing the
options: instead of a live-buffer base-offset (which would have forced
LoopReader/Snapshot to carry an address translation on every access), Crop &
Tile uses **dedicated SDRAM scratch** (`s_tile_unit` ~1 MB / 10 s cap,
`s_tile_build` + `s_tile_prev` ~2.9 MB each / 30 s cap) and does the memory
work **synchronously from the main loop, never the audio ISR** — the existing
Mute mechanism silences output for the (sub-second) duration, so a torn read
of the live buffer mid-copy is simply inaudible, with no per-block chunking
needed. SDRAM is at 92% (~5 MB genuinely free) with this in.

- [x] `TILE` function (note 67, impulse): source = window slice if windowed,
      else the whole loop
- [x] tap = crop to the slice (first tap of a session) / append one more copy
      (subsequent taps); window resets to off after each commit
- [x] ~5 ms equal-power junction crossfade baked at each tile boundary
- [x] one-level Undo/Redo toggle (swap live buffer <-> pre-commit backup);
      `Undo` now dispatches by *which* mechanism produced the last commit
      (Snapshot for Overdub/Substitute, or the Tile swap) so the one shared
      button still does the right thing
- [x] a Tile commit resets the Snapshot COW bookkeeping (`Snapshot::Reset()`)
      and a fresh Record resets the Tile session — neither can apply stale
      undo state to content it doesn't describe
- [x] over-capacity taps are silently ignored (session stays as-is, no crash)

**Verify on hardware:**
- [ ] window a slice, tap TILE → loop crops to just that slice, length
      readout drops accordingly, window off
- [ ] tap TILE again (and again) → length doubles / triples / ... each time,
      click-free at the new joins
- [ ] no window active, tap TILE on a plain recorded loop → doubles it
      directly (no crop needed first)
- [ ] `tap $UNDO` → reverts the last tile step; `tap $UNDO` again → redoes it
- [ ] do an Overdub, confirm Undo reverts it; then Tile, confirm Undo now
      reverts the *tile* step, not the overdub
- [ ] tile past the ~30 s build cap → further taps do nothing audible/harmful
- [ ] no click or dropout audible around a tile commit (the mute-during-copy
      should be inaudible or at most a very brief silence)

## M6 — Loop Select

- [ ] 4-8 fixed slots (config), each a view into the loop pool
- [ ] per-slot state: Speed / Reverse / Window / Feedback / gesture lane
- [ ] CV addressing: 1 V/slot, clamped
- [ ] end-of-loop quantised switch by default; hold encoder / modifier gate =
      immediate switch with a crossfade
- [ ] inactive slots freeze by default (menu option to keep recirculating)
- [ ] revisit the SDRAM table with real per-slot overhead
- [ ] *(v1.1)* shared arena allocator replacing fixed slots

**Done when:** a sequencer stepping CV switches cleanly between four
independently-built loops, and returning to a slot finds it as left.
Ref: user story 7.

## M7 — gesture recorder + probability + polish

- [ ] `src/GestureRec` — discrete events (function + hold-duration + phase) +
      one assignable CV lane; independent loop length; offset knob
- [ ] per-jack Bernoulli probability (encoder menu), outcome drawn on timeline
- [ ] timeline: Tier 1 always-on set; Tier 2/3 contextual reveal-and-fade
- [ ] encoder menu: quantise division, probability, gesture CV assign,
      keep-inactive-slots-live, dry/loop mix, save/recall

**Done when:** a recorded gesture pass loops and offsets against the audio
loop, and per-jack probability visibly thins a patched clock's edits.
Ref: user story 6.
