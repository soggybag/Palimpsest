# Pico RGB Keypad bench rig

External control surface for prototyping the Palimpsest gesture grammar before
any custom hardware exists. Lets you test **tap = latch / hold = momentary** with
real fingers, and renders **colour is the contract** from live engine state.

## Hardware

- Pimoroni Pico RGB Keypad Base + Pico (RP2040)
- 16 silicone keys via a TCA9555 I2C expander
- 16 APA102 RGB LEDs under the keys (SPI)
- Link to the Daisy Patch: **bidirectional TRS MIDI**, 31250 baud

## Wiring (bench)

MIDI is a 3-wire current loop. The Patch MIDI IN jack is 3.5mm **TRS Type A**.

### Keys out — Pico -> Patch MIDI IN  (the only link you need to start)

| Pico | via | TRS (Type A) | DIN |
|---|---|---|---|
| GP0  (UART0 TX) | 33 Ohm | Tip    | pin 5 (data) |
| 3V3            | 10 Ohm | Ring   | pin 4 (source) |
| GND            | —      | Sleeve | pin 2 (shield) |

33/10 Ohm are correct for 3.3 V MIDI; a pair of 220 Ohm usually still enumerates
over a short bench run. UART0 (GP0/GP1) is unused by the keypad base (it uses
I2C for buttons, SPI for LEDs), so those pins are free.

### Colours back — Patch MIDI OUT -> Pico RX  (optional, add later)

Needs a real opto receiver on the Pico (6N138 + 1N4148 + resistor — the standard
MIDI IN circuit) into GP1. Until then the rig runs unidirectional and shows the
fixed default colour per key (see `DEFAULT` in `main.py`).

## Firmware

Flash `main.py` with the **Pimoroni MicroPython** build (bundles `picokeypad`).

### Protocol

- Rig -> Daisy: MIDI Note On (vel 127) / Note Off, note = key index 0..15
- Daisy -> Rig: MIDI Note On, note = key index, velocity = colour id
  (0 off, 1 red, 2 cyan, 3 green, 4 amber, 5 dim)

The Daisy side maps notes to `Controls` events (`{engaged, latch|momentary,
phase}`) — identical to a panel button or a gate — and sends colour updates back
so the LED reflects latched / momentary / armed state.

## Default key map (iterate on hardware)

```
row 0:  Feedback   Record     Overdub    Substitute
row 1:  Mute       Reverse    Retrigger  Window on
row 2:  Win start  Win len    Undo       --
row 3:  spare      spare      spare      spare
```
