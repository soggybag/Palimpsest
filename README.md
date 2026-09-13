# Palimpsest

A Eurorack looper that ports the Echoplex Digital Pro's *live editing*
vocabulary — substitute, reverse, window, speed — to a modular, CV-controlled
form. The loop is raw material to be re-edited in real time, not just layered
up: every performance function is one gesture *and* a patch point. Built on
Electro-Smith Daisy (STM32H750), currently prototyped on a Daisy Patch.

This project was inspired by the looping guitar work of [Andre Lafosse](https://andrelafosse.bandcamp.com/album/pivot-to-video-six-string-mixtape-volume-2) who uses the Echoplex Digital Pro to amazing effect! 

## Status

Core editing engine is built and hardware-verified: record/loop/feedback,
Reverse/Speed/Retrigger, Overdub/Substitute/Undo (one copy-on-write mechanism),
a scannable Loop Window, and clock-quantised Record/Mute/Substitute with
SYNC/PHASE CV outputs. See [`docs/roadmap.md`](docs/roadmap.md) for the
milestone-by-milestone status and what's still ahead.

## Docs

- [`CLAUDE.md`](CLAUDE.md) — design spec: vision, hardware targets, memory
  budget, the full feature set and interface grammar
- [`docs/roadmap.md`](docs/roadmap.md) — milestones, what's done, what's next
- [`docs/interface.md`](docs/interface.md) — every pot/button/jack under
  consideration, with open UI decisions
- [`docs/user-stories.md`](docs/user-stories.md) — performance workflows the
  design is built to serve
- [`docs/testing.md`](docs/testing.md) — bench test procedures and MIDI
  commands for driving the module from a computer
- [`pico_rig/README.md`](pico_rig/README.md) — Pico RGB Keypad bench control
  surface
- [`web-controller/README.md`](web-controller/README.md) — clickable panel
  mockup that drives the hardware over Web MIDI (Chrome/Edge)

## Build & flash

```
make                # -> build/Palimpsest.bin
make program-dfu     # hold BOOT, tap RESET on the Seed, then run this
```

Requires the Daisy toolchain (`arm-none-eabi-gcc`, `dfu-util`, `make`) and a
libDaisy/DaisySP checkout — see `docs/roadmap.md` for paths.

## License

MIT — see [`LICENSE`](LICENSE).
