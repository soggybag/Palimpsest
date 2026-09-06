// Palimpsest — M1b: unified control layer + MIDI in.
//
// Every function now runs through Controls (tap = latch, hold = momentary, gate
// = level), fed by MIDI notes (Pico RGB Keypad or any MIDI keyboard, notes
// 36..42) and/or the panel. Colour state is echoed back over MIDI so the keypad
// LEDs show the contract.
//
//   note 36 / encoder / GATE_IN_1   Record   (edge = advance EMPTY->REC->PLAY->EMPTY)
//   note 37                         Overdub  (M3)
//   note 38                         Substitute (M3)
//   note 39                         Mute     (M5)
//   note 40 / CTRL_3>0.5            Reverse
//   note 41 / GATE_IN_2             Retrigger / stutter
//   note 42                         Undo     (M3)
//   CTRL_1  Feedback     CTRL_2  Speed (detented, CV sums)

#include <cmath>
#include <cstdio>
#include "daisy_patch.h"
#include "daisysp.h"
#include "util/CpuLoadMeter.h"
#include "src/LoopBuffer.h"
#include "src/LoopReader.h"
#include "src/Engine.h"
#include "src/Controls.h"

using namespace daisy;
using namespace daisysp;

DaisyPatch     hw;
MidiUsbHandler midi_usb; // "Daisy" USB-MIDI device (share the flashing cable)
LoopBuffer     loop;
Snapshot       snap;
Engine         engine;
Controls       controls;
CpuLoadMeter   cpu;

constexpr size_t kBufSamples  = 48000u * 400u;      // 38.4 MB loop buffer
constexpr size_t kPoolSamples = 8u * 1024u * 1024u; // 16 MB snapshot pool
static int16_t DSY_SDRAM_BSS s_buf[kBufSamples];
static int16_t DSY_SDRAM_BSS s_pool[kPoolSamples];

static float   s_dt_ms                      = 0.f;
static bool    s_enc_pressed                = false;
static uint8_t s_col_sent[(int)Func::COUNT] = {0};
static float   s_cpu_peak                   = 0.f; // peak load, last ~1 s window

static float FeedbackFromKnob()
{
    float k = hw.GetKnobValue(DaisyPatch::CTRL_1);
    return fclamp((k - 0.03f) / 0.94f, 0.f, 1.f);
}

static float SpeedFromKnob()
{
    float k = hw.GetKnobValue(DaisyPatch::CTRL_2);
    float s = powf(2.f, (k - 0.5f) * 4.f);
    if(fabsf(s - 0.5f) < 0.03f)
        s = 0.5f;
    else if(fabsf(s - 1.0f) < 0.04f)
        s = 1.0f;
    else if(fabsf(s - 2.0f) < 0.06f)
        s = 2.0f;
    return s;
}

static void RouteMidiEvent(MidiEvent m)
{
    if(m.type == NoteOn)
    {
        NoteOnEvent n = m.AsNoteOn();
        controls.Note(n.note, n.velocity > 0);
    }
    else if(m.type == NoteOff)
    {
        controls.Note(m.AsNoteOff().note, false);
    }
}

static void PumpMidi()
{
    hw.midi.Listen(); // TRS MIDI IN
    while(hw.midi.HasEvents())
        RouteMidiEvent(hw.midi.PopEvent());

    midi_usb.Listen(); // USB-MIDI over the flashing cable
    while(midi_usb.HasEvents())
        RouteMidiEvent(midi_usb.PopEvent());
}

// Echo latch/engaged state to the keypad LEDs: 0 off,1 red,2 cyan,3 green,5 dim.
static void SendColours()
{
    static const uint8_t base[(int)Func::COUNT]
        = {1, 1, 1, 2, 2, 2, 3}; // REC OVR SUB red; MUTE REV RET cyan; UNDO green
    for(int i = 0; i < (int)Func::COUNT; i++)
    {
        uint8_t c = controls[(Func)i].Engaged() ? base[i] : 5;
        if(c != s_col_sent[i])
        {
            uint8_t msg[3] = {0x90, kFuncNote[i], c};
            hw.midi.SendMessage(msg, 3);
            s_col_sent[i] = c;
        }
    }
}

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    cpu.OnBlockStart();
    hw.ProcessAllControls();
    PumpMidi();

    // panel -> Controls
    bool enc = hw.encoder.Pressed();
    if(enc && !s_enc_pressed)
        controls[Func::RECORD].Press();
    if(!enc && s_enc_pressed)
        controls[Func::RECORD].Release();
    s_enc_pressed = enc;

    controls.SetGate(Func::RECORD,
                     hw.gate_input[DaisyPatch::GATE_IN_1].State());
    controls.SetGate(Func::RETRIGGER,
                     hw.gate_input[DaisyPatch::GATE_IN_2].State());

    controls.Tick(s_dt_ms);

    // Controls -> Engine
    // Record is impulse: each press/gate-rising advances the state machine;
    // releasing a hold (or gate-falling) closes it.
    if(controls[Func::RECORD].Trigger())
        engine.TrigRecord();
    if(controls[Func::RECORD].ReleaseAfterHold()
       && engine.GetState() == Engine::State::REC_FIRST)
        engine.TrigRecord();

    engine.SetFeedback(FeedbackFromKnob());
    engine.SetSpeed(SpeedFromKnob());
    engine.SetReverse(controls[Func::REVERSE].Engaged());
    if(controls[Func::RETRIGGER].Trigger())
        engine.Retrigger();

    engine.Overdub(controls[Func::OVERDUB].Engaged());
    engine.Substitute(controls[Func::SUBSTITUTE].Engaged(),
                      controls[Func::SUBSTITUTE].Latched());
    if(controls[Func::UNDO].Trigger())
        engine.Undo();

    engine.SetWindow(hw.GetKnobValue(DaisyPatch::CTRL_4),  // start
                     hw.GetKnobValue(DaisyPatch::CTRL_3)); // length (max = off)

    for(size_t n = 0; n < size; n++)
    {
        float sig = engine.Process(in[0][n]);
        for(size_t ch = 0; ch < 4; ch++)
            out[ch][n] = sig;
    }

    SendColours();
    cpu.OnBlockEnd();
}

static void DrawUI()
{
    hw.display.Fill(false);

    const char* label = "EMPTY";
    switch(engine.GetState())
    {
        case Engine::State::REC_FIRST: label = "REC"; break;
        case Engine::State::PLAYING: label = "PLAY"; break;
        default: break;
    }
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(label, Font_7x10, true);

    char b[24];
    int  ms = (int)(engine.LoopSeconds() * 1000.f + 0.5f);
    snprintf(b, sizeof(b), "%d.%03ds", ms / 1000, ms % 1000);
    hw.display.SetCursor(56, 0);
    hw.display.WriteString(b, Font_7x10, true);

    // write / undo status, top-right
    const char* w = engine.Substituting() ? "SUB"
                    : engine.Overdubbing() ? "ODB"
                    : engine.Undone()      ? "UND"
                    : engine.CanUndo()     ? "u"
                                           : "";
    hw.display.SetCursor(110, 0);
    hw.display.WriteString(w, Font_6x8, true);

    const int x0 = 0, x1 = 127, y0 = 20, y1 = 38;
    hw.display.DrawRect(x0, y0, x1, y1, true);

    // window bracket: a filled bar just above the timeline (wraps if it spans
    // the loop origin)
    if(engine.Windowed())
    {
        int span = x1 - x0;
        int ws   = x0 + (int)(engine.WinStartPhase() * (float)span);
        int wpx  = (int)(engine.WinLenPhase() * (float)span);
        if(wpx < 2)
            wpx = 2;
        int we = ws + wpx;
        if(we <= x1)
            hw.display.DrawRect(ws, y0 - 3, we, y0 - 1, true, true);
        else
        {
            hw.display.DrawRect(ws, y0 - 3, x1, y0 - 1, true, true);
            hw.display.DrawRect(x0, y0 - 3, x0 + (we - x1), y0 - 1, true, true);
        }
        char wb[16];
        snprintf(wb, sizeof(wb), "W%dms", (int)(engine.WinMs() + 0.5f));
        hw.display.SetCursor(88, 10);
        hw.display.WriteString(wb, Font_6x8, true);
    }

    if(engine.GetState() == Engine::State::PLAYING)
    {
        int px = x0 + (int)(engine.Phase() * (float)(x1 - x0));
        hw.display.DrawLine(px, y0, px, y1, true);
    }

    float sr = engine.SpeedRatio();
    snprintf(b,
             sizeof(b),
             "%s%d.%02dx",
             sr < 0 ? "-" : " ",
             (int)fabsf(sr),
             (int)(fabsf(sr) * 100.f) % 100);
    hw.display.SetCursor(0, 42);
    hw.display.WriteString(b, Font_6x8, true);

    hw.display.SetCursor(48, 42);
    if(engine.Frozen())
        hw.display.WriteString("FRZ", Font_6x8, true);
    else
    {
        snprintf(b, sizeof(b), "FB%2d", (int)(FeedbackFromKnob() * 100.f + 0.5f));
        hw.display.WriteString(b, Font_6x8, true);
    }
    if(engine.Reversed())
    {
        hw.display.SetCursor(86, 42);
        hw.display.WriteString("<REV", Font_6x8, true);
    }

    // engaged-function row: letter shown when that Func is engaged (latch|gate)
    static const char kL[(int)Func::COUNT] = {'R', 'O', 'S', 'M', 'V', 'T', 'U'};
    char              fs[(int)Func::COUNT + 1];
    for(int i = 0; i < (int)Func::COUNT; i++)
        fs[i] = controls[(Func)i].Engaged() ? kL[i] : '.';
    fs[(int)Func::COUNT] = 0;
    hw.display.SetCursor(0, 54);
    hw.display.WriteString(fs, Font_6x8, true);

    int cpul = (int)(s_cpu_peak * 100.f + 0.5f);
    if(cpul > 999)
        cpul = 999;
    snprintf(b, sizeof(b), "CPU%2d%%", cpul);
    hw.display.SetCursor(80, 54);
    hw.display.WriteString(b, Font_6x8, true);

    hw.display.Update();
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(32);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    float sr = hw.AudioSampleRate();
    s_dt_ms  = 1000.f * (float)hw.AudioBlockSize() / sr;

    loop.Init(s_buf, kBufSamples);
    loop.ClearTo(kBufSamples);
    snap.Init(s_pool, kPoolSamples, kBufSamples);
    engine.Init(&loop, &snap, sr);
    controls.Init();
    cpu.Init(sr, hw.AudioBlockSize());

    // USB port -> MIDI device (not CDC log). Debug is on the OLED.
    MidiUsbHandler::Config mc;
    mc.transport_config.periph = MidiUsbTransport::Config::INTERNAL;
    midi_usb.Init(mc);

    hw.midi.StartReceive();
    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    uint32_t last = 0;
    while(1)
    {
        DrawUI();
        uint32_t now = System::GetNow();
        if(now - last >= 1000)
        {
            last    = now;
            float m = cpu.GetMaxCpuLoad();
            if(m >= 0.f && m < 4.f) // reject tick-counter wrap artefacts
                s_cpu_peak = m;
            cpu.Reset();
        }
    }
}
