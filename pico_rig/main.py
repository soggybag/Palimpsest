# Palimpsest bench rig — Pico RGB Keypad -> Daisy Patch over TRS MIDI.
#
# Flash this to the Pimoroni Pico RGB Keypad Base using the Pimoroni MicroPython
# build (it bundles the `picokeypad` module).
#
# Wiring (bench) — Patch MIDI IN is 3.5mm TRS Type A:
#   Pico GP0 (UART0 TX) --[33R]--> TRS Tip    -> Patch MIDI IN   (keys out)
#   Pico 3V3           --[10R]--> TRS Ring    -> Patch MIDI IN   (current source)
#   Pico GND           --------- TRS Sleeve  -> Patch MIDI IN   (shield)
#   Colours back (Patch MIDI OUT -> Pico GP1) is optional and needs an opto
#   receiver; until then the rig runs TX-only with fixed default colours.
#   MIDI baud is 31250. See README.md.
#
# Protocol:
#   Rig -> Daisy : Note On  (vel 127) / Note Off  , note = key index 0..15
#   Daisy -> Rig : CC #20   , value = key<<0 is NOT used; instead:
#                  Note On on channel 1, note = key index, velocity = colour id
#                  (0 off, 1 red, 2 cyan, 3 green, 4 amber, 5 dim-white)
#
# This is a starting sketch — expect to iterate the mapping on hardware.

import time
import picokeypad
from machine import UART, Pin

uart = UART(0, baudrate=31250, tx=Pin(0), rx=Pin(1))

picokeypad.init()
picokeypad.set_brightness(0.6)
NUM_KEYS = picokeypad.get_num_pads()  # 16

# colour id -> (r, g, b)
PALETTE = {
    0: (0, 0, 0),
    1: (255, 0, 0),      # red   — destructive
    2: (0, 180, 255),    # cyan  — playback only
    3: (0, 255, 60),     # green — undo
    4: (255, 150, 0),    # amber — feedback
    5: (40, 40, 40),     # dim   — armed / idle
}

# Default colour per key before the Daisy says otherwise. Mirrors the grammar so
# the rig is legible even with the MIDI-in link unplugged.
DEFAULT = [
    4, 1, 1, 1,   # Feedback, Record, Overdub, Substitute
    2, 2, 2, 2,   # Mute, Reverse, Retrigger, (Window on)
    2, 2, 3, 5,   # (Window start), (Window len), Undo, --
    5, 5, 5, 5,   # spare
]

colour = list(DEFAULT)
last_state = 0


def render():
    for i in range(NUM_KEYS):
        r, g, b = PALETTE[colour[i]]
        picokeypad.illuminate(i, r, g, b)
    picokeypad.update()


NOTE_BASE = 60  # key index 0 -> MIDI note 60 (middle C); matches src/Controls.h


def send_note(key, on):
    status = 0x90 if on else 0x80
    uart.write(bytes((status, (NOTE_BASE + key) & 0x7F, 127 if on else 0)))


def poll_midi_in():
    # Minimal parser: Note On (0x90) with note = NOTE_BASE + key sets a colour.
    while uart.any():
        b0 = uart.read(1)[0]
        if b0 == 0x90:
            while uart.any() < 2:
                pass
            data = uart.read(2)
            key, cid = data[0] - NOTE_BASE, data[1]
            if 0 <= key < NUM_KEYS and cid in PALETTE:
                colour[key] = cid


render()

while True:
    state = picokeypad.get_button_states()
    if state != last_state:
        changed = state ^ last_state
        for i in range(NUM_KEYS):
            if changed & (1 << i):
                send_note(i, bool(state & (1 << i)))
        last_state = state

    poll_midi_in()
    render()
    time.sleep(0.005)  # ~200 Hz scan
