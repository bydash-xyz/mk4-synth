#pragma once

// Computer-keyboard play mode for synth_sim. Pure logic (no terminal I/O), so the key map and the
// note-off strategy can be unit-tested: keys go in, raw MIDI channel messages come out, and the
// simulator routes those through the same UsbHostMidi path as a physical MPK Mini MK4.
//
// A terminal delivers key presses only (no key-up events, and held keys arrive as autorepeat),
// so every note is released automatically after a gate time (Gate mode), or on the next press
// of the same key (Hold mode, for chords and drones).

#include "sim_util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace SimKeyboard {

enum class Command {
    None, Note, OctaveDown, OctaveUp, PresetPrev, PresetNext, Velocity,
    Panic, ToggleHold, ToggleBank, GateShorter, GateLonger, Help, Quit
};

struct KeyBinding {
    Command cmd;
    int value; // Note: semitone above the octave's C; Velocity: MIDI velocity
};

// Two-row piano: 'a' = C, 'w' = C#, ... 'k' = C one octave up, continuing to 'o l p ;' (C#..E)
inline KeyBinding mapKey(char raw) {
    static const char kPiano[] = "awsedftgyhujkolp;";
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
    for (int i = 0; kPiano[i]; ++i) {
        if (c == kPiano[i]) return {Command::Note, i};
    }
    if (c >= '1' && c <= '9') {
        const int level = c - '0';
        return {Command::Velocity, (level * 127 + 4) / 9}; // 1 = 14 ... 9 = 127
    }
    switch (c) {
        case 'z': return {Command::OctaveDown, 0};
        case 'x': return {Command::OctaveUp, 0};
        case ',': case '[': return {Command::PresetPrev, 0};
        case '.': case ']': return {Command::PresetNext, 0};
        case ' ': return {Command::Panic, 0};
        case 'm': return {Command::ToggleHold, 0};
        case 'b': return {Command::ToggleBank, 0};
        case '-': return {Command::GateShorter, 0};
        case '=': return {Command::GateLonger, 0};
        case '?': return {Command::Help, 0};
        case 'q': return {Command::Quit, 0};
        default:  return {Command::None, 0};
    }
}

struct MidiOut {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
};

class Player {
public:
    static constexpr int      kDefaultVelocity = 100;
    static constexpr int      kDefaultOctave   = 4;    // 'a' = C4 (MIDI 60)
    static constexpr int      kMinOctave       = -1;
    static constexpr int      kMaxOctave       = 8;    // top key ';' = E9 (MIDI 124)
    static constexpr uint32_t kDefaultGateMs   = 400;
    static constexpr uint32_t kMinGateMs       = 100;
    static constexpr uint32_t kMaxGateMs       = 3000;
    static constexpr uint32_t kGateStepMs      = 100;
    // Presses of the same key closer together than this are terminal autorepeat (key held down):
    // they extend the note instead of retriggering it (Gate mode) or are ignored (Hold mode)
    static constexpr uint32_t kRepeatWindowMs  = 150;
    static constexpr uint8_t  kChannel         = 0;    // MIDI channel 1

    // Applies a key press at time nowMs. Note, octave, velocity, gate, hold and panic are handled
    // here (MIDI goes to `out`); the returned command tells the caller what to do for the rest
    // (preset / bank / help / quit) and what to print.
    Command handleKey(char c, uint64_t nowMs, std::vector<MidiOut>& out) {
        const KeyBinding kb = mapKey(c);
        switch (kb.cmd) {
            case Command::Note:        pressNote(baseNote() + kb.value, nowMs, out); break;
            case Command::OctaveDown:  octave_ = std::max(kMinOctave, octave_ - 1); break;
            case Command::OctaveUp:    octave_ = std::min(kMaxOctave, octave_ + 1); break;
            case Command::Velocity:    velocity_ = kb.value; break;
            case Command::GateShorter: gateMs_ = std::max(kMinGateMs, gateMs_ - kGateStepMs); break;
            case Command::GateLonger:  gateMs_ = std::min(kMaxGateMs, gateMs_ + kGateStepMs); break;
            case Command::ToggleHold:
                releaseAll(out);
                holdMode_ = !holdMode_;
                break;
            case Command::Panic:
                held_.clear();
                out.push_back({static_cast<uint8_t>(0xB0 | kChannel), 120, 0}); // CC 120 All Sound Off
                break;
            case Command::Quit:
                releaseAll(out);
                break;
            default: break;
        }
        return kb.cmd;
    }

    // Releases Gate-mode notes whose gate has expired
    void tick(uint64_t nowMs, std::vector<MidiOut>& out) {
        for (size_t i = 0; i < held_.size();) {
            if (held_[i].releaseAtMs != 0 && nowMs >= held_[i].releaseAtMs) {
                out.push_back(noteOff(held_[i].note));
                held_.erase(held_.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }
    }

    void releaseAll(std::vector<MidiOut>& out) {
        for (const auto& h : held_) out.push_back(noteOff(h.note));
        held_.clear();
    }

    int      octave() const { return octave_; }
    int      baseNote() const { return (octave_ + 1) * 12; }
    int      velocity() const { return velocity_; }
    uint32_t gateMs() const { return gateMs_; }
    bool     holdMode() const { return holdMode_; }
    size_t   soundingCount() const { return held_.size(); }

    std::string statusLine() const {
        return "octave " + SimUtil::noteName(baseNote()) + ".." + SimUtil::noteName(baseNote() + 16) +
               " | vel " + std::to_string(velocity_) +
               " | " + (holdMode_ ? std::string("HOLD (press again to release)")
                                  : "GATE " + std::to_string(gateMs_) + " ms");
    }

    static std::string helpText() {
        return
            "Computer keyboard (Akai MPK Mini MK4 input keeps working alongside it):\n"
            "   w e   t y u   o p        black keys  C#  D#    F#  G#  A#    C#  D#\n"
            "  a s d f g h j k l ;       white keys  C  D  E  F  G  A  B  C  D  E\n"
            "  z / x      octave down / up          1..9   velocity (14..127)\n"
            "  , / .      preset prev / next        [ / ]  preset prev / next\n"
            "  b          knob bank A / B           m      GATE <-> HOLD note mode\n"
            "  - / =      gate time -/+ 100 ms      space  panic (all sound off)\n"
            "  ?          this help                 q      quit (or Ctrl+C)\n"
            "Terminals send no key-up events. GATE mode: each note is released after the gate\n"
            "time; holding a key (autorepeat) keeps it sounding. HOLD mode: a note sounds until\n"
            "its key is pressed again (tap, don't hold), so chords can be built one key at a time.\n";
    }

private:
    struct HeldNote {
        uint8_t  note;
        uint64_t releaseAtMs; // 0 = no automatic release (Hold mode)
    };
    static constexpr uint64_t kNever = ~uint64_t{0};

    static MidiOut noteOff(uint8_t note) { return {static_cast<uint8_t>(0x80 | kChannel), note, 0}; }

    void pressNote(int noteNumber, uint64_t nowMs, std::vector<MidiOut>& out) {
        if (noteNumber < 0 || noteNumber > 127) return;
        const uint8_t note = static_cast<uint8_t>(noteNumber);
        const bool autorepeat = lastPressMs_[note] != kNever && nowMs - lastPressMs_[note] < kRepeatWindowMs;
        lastPressMs_[note] = nowMs;
        auto it = std::find_if(held_.begin(), held_.end(), [note](const HeldNote& h) { return h.note == note; });

        if (autorepeat) {
            if (it != held_.end() && !holdMode_) it->releaseAtMs = nowMs + gateMs_;
            return;
        }
        if (it != held_.end()) {
            out.push_back(noteOff(note));
            held_.erase(it);
            if (holdMode_) return; // second press releases a held note
        }
        out.push_back({static_cast<uint8_t>(0x90 | kChannel), note, static_cast<uint8_t>(velocity_)});
        held_.push_back({note, holdMode_ ? 0 : nowMs + gateMs_});
    }

    int      octave_{kDefaultOctave};
    int      velocity_{kDefaultVelocity};
    uint32_t gateMs_{kDefaultGateMs};
    bool     holdMode_{false};
    std::vector<HeldNote> held_;
    std::array<uint64_t, 128> lastPressMs_ = makeNeverArray();

    static std::array<uint64_t, 128> makeNeverArray() {
        std::array<uint64_t, 128> a{};
        a.fill(kNever);
        return a;
    }
};

} // namespace SimKeyboard
