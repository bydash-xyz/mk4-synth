#pragma once

#include <cstdint>

// ============================================================================
// MIDI CC numbers: synth parameters and the Akai MPK Mini MK4 controls.
//
// Two groups share CC numbers. Sections 1..5 are what SynthEngine::controlChange() understands.
// Section 6 is what the MK4 sends for its knobs and buttons. MpkMiniMk4::ControlSurface sees every
// incoming CC first: it turns the knob CCs (24..31) into the parameter CCs of the active bank, handles
// the button CCs itself, and passes only the rest to the engine. So an incoming CC 80 is the '-' button,
// never Filter Env Attack; the parameter CCs only reach the engine through the knob mapping.
// ============================================================================

namespace Synth {

// ----------------------------------------------------------------------------
// 1. Standard MIDI Performance & Channel Mode Controllers
// ----------------------------------------------------------------------------
constexpr uint8_t CC_BANK_SELECT_MSB       = 0;   // Bank Select MSB: selects preset slot (value % slot count)
constexpr uint8_t CC_MOD_WHEEL             = 1;   // Modulation Wheel / Vibrato Depth
constexpr uint8_t CC_PORTAMENTO_TIME       = 5;   // Portamento / Glide Rate (0..1000 ms)
constexpr uint8_t CC_VOLUME                = 7;   // Master Channel Volume
constexpr uint8_t CC_BANK_SELECT_LSB       = 32;  // Bank Select LSB: same as CC 0
constexpr uint8_t CC_SUSTAIN_PEDAL         = 64;  // Damper / Sustain Pedal (<64 Off, >=64 On)
constexpr uint8_t CC_PORTAMENTO_ENABLE     = 65;  // Portamento On/Off switch & TB-303 Slide
constexpr uint8_t CC_ALL_SOUND_OFF         = 120; // Panic / Immediate voice cut
constexpr uint8_t CC_ALL_NOTES_OFF         = 123; // Release all held voices
constexpr uint8_t CC_MONO_MODE             = 126; // Monophonic Voice Mode On
constexpr uint8_t CC_POLY_MODE             = 127; // Polyphonic Voice Mode On

// ----------------------------------------------------------------------------
// 2. Synth parameters usually on Bank A. The FM presets reuse some of them (second meaning after the '/').
// ----------------------------------------------------------------------------
constexpr uint8_t CC_OSC_WAVEFORM          = 70;  // Oscillator Waveform / Casio Tone
constexpr uint8_t CC_FILTER_RESONANCE      = 71;  // Filter Resonance Q / FM Ratio
constexpr uint8_t CC_AMP_RELEASE           = 72;  // Amp Envelope Release Time
constexpr uint8_t CC_AMP_ATTACK            = 73;  // Amp Envelope Attack Time
constexpr uint8_t CC_FILTER_CUTOFF         = 74;  // Filter Cutoff Frequency / FM Mod Depth
constexpr uint8_t CC_AMP_DECAY             = 75;  // Amp Envelope Decay Time
constexpr uint8_t CC_FILTER_ENV_AMT        = 76;  // Filter Envelope Depth / FM Mod Decay / Casio Pulse Width
constexpr uint8_t CC_SUB_OSC_LEVEL         = 77;  // Sub-Oscillator Level / FM Feedback
constexpr uint8_t CC_CHORUS_MIX            = 78;  // Stereo Chorus Mix (0..10 = chorus off)
constexpr uint8_t CC_AMP_SUSTAIN           = 79;  // Amp Envelope Sustain Level

// ----------------------------------------------------------------------------
// 3. Synth parameters usually on Bank B
// ----------------------------------------------------------------------------
constexpr uint8_t CC_FILTER_ENV_ATTACK     = 80;  // Filter Envelope Attack / FM Mod Sustain
constexpr uint8_t CC_FILTER_ENV_DECAY      = 81;  // Filter Envelope Decay / FM Mod Attack
constexpr uint8_t CC_FILTER_ENV_SUSTAIN    = 82;  // Filter Envelope Sustain / FM Tremolo Depth / Casio Octave
constexpr uint8_t CC_FILTER_ENV_RELEASE    = 83;  // Filter Envelope Release / FM Tremolo Rate
constexpr uint8_t CC_CHORUS_RATE           = 84;  // Chorus LFO Rate
constexpr uint8_t CC_CHORUS_DEPTH          = 85;  // Chorus Depth
constexpr uint8_t CC_DRIVE                 = 87;  // Pre-filter overdrive (1.0 .. 3.5)
constexpr uint8_t CC_ACCENT                = 88;  // TB-303 accent amount (accent triggers on velocity >= 100)
constexpr uint8_t CC_LFO_RATE              = 89;  // Vibrato LFO rate (0.5 .. 15 Hz)
constexpr uint8_t CC_LFO_DEPTH             = 90;  // Vibrato LFO depth (0 .. +/-0.6 semitone)
constexpr uint8_t CC_CHORUS_SEND           = 91;  // Chorus Mix, same as CC 78 (for external controllers)
constexpr uint8_t CC_VOICE_MODE            = 92;  // <64 Mono, >=64 Poly (knob-friendly alternative to CC 126/127)
constexpr uint8_t CC_GLIDE                 = 93;  // 0 = glide off, 1..127 = glide on with 0..1000 ms
constexpr uint8_t CC_DETUNE_DEPTH          = 94;  // Oscillator 2 Detune (cents)

// ----------------------------------------------------------------------------
// 4. Direct FM CCs. CC 16..20 are also MK4 custom-map buttons (section 6), so over USB only 21..23 reach the engine.
// ----------------------------------------------------------------------------
constexpr uint8_t CC_FM_MOD_DEPTH          = 16;  // FM Modulator Index / Depth
constexpr uint8_t CC_FM_MOD_RATIO          = 17;  // FM Modulator Frequency Ratio
constexpr uint8_t CC_FM_FILTER_DECAY       = 18;  // FM Modulator Envelope Decay
constexpr uint8_t CC_FM_AMP_RELEASE        = 19;  // FM Carrier Envelope Release
constexpr uint8_t CC_FM_FEEDBACK           = 20;  // FM Modulator Self-Feedback
constexpr uint8_t CC_FM_CHORUS_MIX         = 21;  // FM Stereo Chorus Mix
constexpr uint8_t CC_FM_TREMOLO_DEPTH      = 22;  // FM Tremolo Depth
constexpr uint8_t CC_FM_MASTER_VOL         = 23;  // FM Master Volume

// ----------------------------------------------------------------------------
// 4b. Wavetable / Phase Distortion / Pluck / Additive engine CCs (knob targets of those presets;
//     LSB numbers no controller sends, clear of the MK4's hardware CCs)
// ----------------------------------------------------------------------------
constexpr uint8_t CC_WT_POSITION           = 40;  // Wavetable frame position (0..1)
constexpr uint8_t CC_WT_ENV_AMT            = 41;  // Filter envelope -> wavetable position (0..1)
constexpr uint8_t CC_WT_TABLE              = 42;  // Wavetable bank, 4 buckets of 32
constexpr uint8_t CC_PD_DCW                = 43;  // Phase distortion DCW amount (0..1)
constexpr uint8_t CC_PD_WAVE               = 44;  // CZ waveform, 7 buckets (PdWave)
constexpr uint8_t CC_PLUCK_DECAY           = 45;  // String decay T60, 100 .. 10000 ms (log)
constexpr uint8_t CC_PLUCK_BRIGHT          = 46;  // Pluck brightness (0..1)
constexpr uint8_t CC_PLUCK_PICK            = 47;  // Pick position (0 = bridge .. 1 = middle)
constexpr uint8_t CC_PERC_LEVEL            = 48;  // Organ percussion level (0..1)
constexpr uint8_t CC_PERC_HARMONIC         = 49;  // <64 second (4'), >=64 third (2 2/3')
constexpr uint8_t CC_KEY_CLICK             = 51;  // Organ key click level (0..1)
constexpr uint8_t CC_TREMOLO_DEPTH         = 53;  // Stereo tremolo / Leslie depth, any engine (0..1)
constexpr uint8_t CC_TREMOLO_RATE          = 54;  // Tremolo / Leslie rate, any engine (0.5 .. 15.5 Hz)
constexpr uint8_t CC_DRAWBAR_1             = 55;  // Drawbars 16' .. 1' on CC 55..63, 0..8 in 9 steps
constexpr uint8_t CC_DRAWBAR_9             = 63;

// ----------------------------------------------------------------------------
// 5. Metronome, Tempo, Time Signature & Musical Key / Scale CCs (the Rhythm knob page)
// ----------------------------------------------------------------------------
constexpr uint8_t CC_METRO_VOLUME          = 103; // 0: Off, 1..127: Metronome click level
constexpr uint8_t CC_TEMPO_BPM             = 104; // 40..240 BPM
constexpr uint8_t CC_TIME_SIGNATURE        = 105; // TimeSignaturePreset (4/4, 3/4, 2/4, 5/4, 6/8, 7/8, 9/8, 12/8)
constexpr uint8_t CC_TIME_SIG_BEATS        = 106; // Direct beats-per-bar numerator (1..16)
constexpr uint8_t CC_ROOT_KEY              = 107; // Root key 0..11 (C..B)
constexpr uint8_t CC_SCALE_MODE            = 108; // ScaleMode quantizer (Chromatic, Major, Minor, etc.)
constexpr uint8_t CC_METRO_KEY_TUNE        = 109; // <64: Fixed woodblock click, >=64: Root-Key tuned click
constexpr uint8_t CC_METRO_CLICK           = 117; // One dial, 13 steps: WOOD (fixed woodblock), then a click tuned to C..B (sets the Root Key)
constexpr uint8_t CC_OUTPUT_MODE           = 116; // <64: Stereo, >=64: Mono on the left channel, right silent
constexpr uint8_t CC_METRO_SILENT          = 113; // <64: click audible, >=64: silent (beats still flash the LEDs and OLED)
constexpr uint8_t CC_METRONOME_TOGGLE      = 118; // Metronome toggle (custom maps; physical SHIFT+TAP = CC 17 + CC_HW_TAP_TEMPO)
constexpr uint8_t CC_TAP_TEMPO             = 119; // Tap tempo (custom maps; physical button emits CC_HW_TAP_TEMPO = 82)

// ----------------------------------------------------------------------------
// 6. Akai MPK Mini MK4 controls (handled by MpkMiniMk4::ControlSurface, never by the engine)
// ----------------------------------------------------------------------------
// Physical controls of the MK4 PLUGIN preset
constexpr uint8_t CC_HW_KNOB_BASE          = 24;  // Knobs K1..K8 emit CC 24..31
constexpr uint8_t CC_HW_KNOB_1             = 24;
constexpr uint8_t CC_HW_KNOB_2             = 25;
constexpr uint8_t CC_HW_KNOB_3             = 26;
constexpr uint8_t CC_HW_KNOB_4             = 27;
constexpr uint8_t CC_HW_KNOB_5             = 28;
constexpr uint8_t CC_HW_KNOB_6             = 29;
constexpr uint8_t CC_HW_KNOB_7             = 30;
constexpr uint8_t CC_HW_KNOB_8             = 31;
constexpr uint8_t CC_HW_BANK_AB_BUTTON     = 12;  // BANK A/B button
constexpr uint8_t CC_HW_PUSH               = 13;  // Push-encoder click (127 = press, 0 = release)
constexpr uint8_t CC_HW_SHIFT_BUTTON       = 17;  // SHIFT button (127 = held, 0 = released)
constexpr uint8_t CC_HW_NAV_ENCODER        = 72;  // Push-encoder rotation (relative: 1..63 CW, 65..127 CCW)
constexpr uint8_t CC_HW_DAW_TRANSPORT_BTN  = 74;  // DAW-port transport button (not SHIFT). Ignored.
constexpr uint8_t CC_HW_PRESET_PREV        = 80;  // '-' button
constexpr uint8_t CC_HW_PRESET_NEXT        = 81;  // '+' button
constexpr uint8_t CC_HW_TAP_TEMPO          = 82;  // TAP TEMPO button (127 = press, 0 = release; DAW port, ch 1).
                                                  // No SHIFT variant: SHIFT+TAP arrives as CC 17 held, then CC 82 (firmware 0x0801F9AA / 0x0801FFC0)

// Custom MK4 maps and other controllers
constexpr uint8_t CC_HW_CUSTOM_ENCODER     = 14;  // Push-encoder rotation
constexpr uint8_t CC_HW_CUSTOM_PUSH        = 15;  // Push-encoder click
constexpr uint8_t CC_HW_CUSTOM_BANK_LEFT   = 16;  // Select Bank A
constexpr uint8_t CC_HW_CUSTOM_SHIFT       = 18;  // SHIFT
constexpr uint8_t CC_HW_CUSTOM_METRO_BTN   = 19;  // Metronome on/off
constexpr uint8_t CC_HW_CUSTOM_TAP_BTN     = 20;  // Tap tempo
constexpr uint8_t CC_HW_BANK_AB_ALT        = 50;  // BANK A/B toggle
constexpr uint8_t CC_HW_CUSTOM_BANK_RIGHT  = 52;  // Select Bank B (not 17, which is the physical SHIFT)
constexpr uint8_t CC_HW_FOOTSWITCH_BANK    = 86;  // Foot switch: each press toggles Bank A / Bank B
constexpr uint8_t CC_HW_SHIFT_TOGGLE_2     = 102; // SHIFT (alternate)
constexpr uint8_t CC_HW_CURSOR_LEFT        = 110; // Cursor left: previous preset
constexpr uint8_t CC_HW_CURSOR_RIGHT       = 111; // Cursor right: next preset
constexpr uint8_t CC_HW_TRANSPORT_PREV     = 114; // Transport previous track: previous preset
constexpr uint8_t CC_HW_TRANSPORT_NEXT     = 115; // Transport next track: next preset

} // namespace Synth
